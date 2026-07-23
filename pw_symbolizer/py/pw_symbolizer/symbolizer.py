# Copyright 2021 The Pigweed Authors
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy of
# the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
# License for the specific language governing permissions and limitations under
# the License.
"""Utilities for address symbolization."""

from __future__ import annotations

import abc
from enum import Enum
from typing import Iterable, Sequence
from dataclasses import dataclass


@dataclass(frozen=True)
class Symbol:
    """Symbols produced by a symbolizer."""

    address: int
    name: str = ''
    file: str = ''
    line: int = 0

    def to_string(self, max_filename_len: int = 0) -> str:
        if not self.name:
            name = f'0x{self.address:08X}'
        else:
            name = self.name

        return f'{name} ({self.file_and_line(max_filename_len)})'

    def file_and_line(self, max_filename_len: int = 0) -> str:
        """Returns a file/line number string, with question marks if unknown."""

        if not self.file:
            return '??:?'

        if max_filename_len and len(self.file) > max_filename_len:
            return f'[...]{self.file[-max_filename_len:]}:{self.line}'

        return f'{self.file}:{self.line}'

    def __str__(self):
        return self.to_string()


class CpuArchitecture(Enum):
    """Supported CPU architectures for return address calculation."""

    UNKNOWN = 'unknown'
    ARMV7M = 'armv7m'
    ARMV8M = 'armv8m'
    ARMV7A = 'armv7a'
    AARCH32 = 'aarch32'
    AARCH64 = 'aarch64'
    X86 = 'x86'
    X86_64 = 'x86_64'
    XTENSA = 'xtensa'
    RISCV32 = 'riscv32'
    RISCV64 = 'riscv64'

    @classmethod
    def from_value(cls, val: CpuArchitecture | str | None) -> CpuArchitecture:
        """Parses string or enum values into a CpuArchitecture enum."""
        if isinstance(val, cls):
            return val
        if val is None or isinstance(val, bool):
            return cls.UNKNOWN

        if hasattr(val, 'name') and isinstance(val.name, str):
            clean_str = val.name.lower()
        else:
            clean_str = str(val).rsplit('.', maxsplit=1)[-1].lower()

        clean_str = clean_str.replace('-', '').replace('_', '')

        if clean_str in ('arm64', 'aarch64'):
            return cls.AARCH64

        if clean_str in ('arm', 'aarch32'):
            return cls.AARCH32

        if clean_str in ('riscv64', 'rv64e', 'rv64i'):
            return cls.RISCV64

        if clean_str in ('riscv32', 'rv32e', 'rv32i'):
            return cls.RISCV32

        for member in cls:
            member_clean = member.value.replace('-', '').replace('_', '')
            if clean_str == member_clean:
                return member
        return cls.UNKNOWN

    @classmethod
    def from_str(cls, val: CpuArchitecture | str | None) -> CpuArchitecture:
        """Backwards compatibility alias for from_value."""
        return cls.from_value(val)

    def is_arm_32bit(self) -> bool:
        """Returns True if the architecture is a 32-bit ARM architecture."""
        return self in (
            CpuArchitecture.ARMV7M,
            CpuArchitecture.ARMV8M,
            CpuArchitecture.ARMV7A,
            CpuArchitecture.AARCH32,
        )


class Symbolizer(abc.ABC):
    """An interface for symbolizing addresses."""

    cpu_arch = CpuArchitecture.UNKNOWN

    @abc.abstractmethod
    def symbolize(self, address: int) -> Symbol:
        """Symbolizes an address using a loaded binary or symbol database."""

    def adjust_return_address(
        self,
        address: int,
        cpu_arch: CpuArchitecture | str | None = None,
    ) -> int:
        """Adjusts a parent frame return address to point inside the calling
        instruction.

        Prevents symbol misattribution when a function call is located at the
        very end of a caller (e.g. calls to non-returning functions).
        """
        parsed_arch = CpuArchitecture.from_value(cpu_arch)
        if parsed_arch == CpuArchitecture.UNKNOWN and hasattr(self, 'cpu_arch'):
            parsed_arch = CpuArchitecture.from_value(self.cpu_arch)

        # ARM 32-bit architectures (AArch32, ARMV7M, ARMV8M, ARMV7A) use LSB=1
        # in the return address (LR) to indicate Thumb mode execution.
        if (address & 1) and parsed_arch.is_arm_32bit():
            # Thumb mode: clear Thumb bit (LSB) and use 2-byte offset. This is
            # valid for both 16-bit BLX Rm and 32-bit BL label instructions
            # since we just need the symbolizer to land inside the call
            # instruction.
            address &= ~1
            offset = 2
        elif parsed_arch in (
            CpuArchitecture.AARCH64,
            CpuArchitecture.AARCH32,
            CpuArchitecture.ARMV7A,
        ):
            offset = 4
        elif parsed_arch in (CpuArchitecture.X86, CpuArchitecture.X86_64):
            offset = 1
        elif parsed_arch in (
            CpuArchitecture.XTENSA,
            CpuArchitecture.RISCV32,
            CpuArchitecture.RISCV64,
        ):
            offset = 2
        elif parsed_arch == CpuArchitecture.UNKNOWN:
            # We make no adjustments if cpu_arch hasn't been set or if the
            # provided value isn't supported yet. This avoids that we break
            # symbolication any further or in mysterious ways for downstream
            # projects, keeping the same behavior from before the return
            # address adjustments were made. Downstream projects must set
            # cpu_arch correctly to benefit from this fix.
            offset = 0
        else:
            raise ValueError(f'Unsupported CPU architecture: {parsed_arch}')

        return max(0, address - offset)

    def dump_stack_trace(
        self,
        addresses: Sequence[int],
        most_recent_first: bool = True,
        cpu_arch: CpuArchitecture | str | None = None,
        adjust_parent_frames: bool = True,
    ) -> str:
        """Symbolizes and dumps a list of addresses as a stack trace.

        Args:
            addresses: A sequence of memory addresses to symbolize.
            most_recent_first: Controls top-of-stack hint and frame parent
                detection. If True, address at index 0 is the active PC; if
                False, address at index 0 is the root caller.
            cpu_arch: Target CPU architecture for return address calculation.
                If None or UNKNOWN, falls back to the symbolizer instance
                architecture.
            adjust_parent_frames: If True, parent frame return addresses are
                adjusted into the call site to prevent symbol misattribution
                at non-returning function boundaries.
        """
        order: str = 'first' if most_recent_first else 'last'

        stack_trace: list[str] = []
        stack_trace.append(f'Stack Trace (most recent call {order}):')

        num_addresses = len(addresses)
        max_width = len(str(num_addresses))

        parsed_arch = CpuArchitecture.from_str(cpu_arch)
        if parsed_arch == CpuArchitecture.UNKNOWN and hasattr(self, 'cpu_arch'):
            parsed_arch = CpuArchitecture.from_str(self.cpu_arch)

        is_64bit = parsed_arch in (
            CpuArchitecture.AARCH64,
            CpuArchitecture.X86_64,
            CpuArchitecture.RISCV64,
        ) or any(addr > 0xFFFFFFFF for addr in addresses)
        hex_width = 16 if is_64bit else 8

        for i, address in enumerate(addresses):
            depth = i + 1
            is_parent_frame = (
                (i > 0) if most_recent_first else (i < num_addresses - 1)
            )

            if is_parent_frame and adjust_parent_frames:
                lookup_address = self.adjust_return_address(
                    address, parsed_arch
                )
            else:
                lookup_address = address

            symbol = self.symbolize(lookup_address)

            if symbol.name:
                sym_desc = f'{symbol.name} (0x{address:0{hex_width}X})'
            else:
                sym_desc = f'(0x{address:0{hex_width}X})'

            stack_trace.append(f'  {depth:>{max_width}}: at {sym_desc}')
            stack_trace.append(f'      in {symbol.file_and_line()}')

        return '\n'.join(stack_trace)


class FakeSymbolizer(Symbolizer):
    """A fake symbolizer that only knows a fixed set of symbols."""

    def __init__(self, known_symbols: Iterable[Symbol] | None = None):
        if known_symbols is not None:
            self._db = {sym.address: sym for sym in known_symbols}
        else:
            self._db = {}

    def symbolize(self, address: int) -> Symbol:
        """Symbolizes a fixed symbol database."""
        if address in self._db:
            return self._db[address]

        return Symbol(address)
