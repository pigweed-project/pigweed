# Copyright 2026 The Pigweed Authors
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
"""Classes for programmatically building crash coredump ELF files."""

import io

from pw_bytes.io import BinaryWriter, ByteOrder
from pw_elf.builder import ElfBuilder, ElfClass, ElfMachine, ElfType

_NT_PRSTATUS = 1
_NT_GNU_BUILD_ID = 3


def _format_prstatus_arm32(
    thread_id: int,
    registers: list[int],
    byteorder: ByteOrder,
) -> bytes:
    """Formats a 32-bit ARM elf_prstatus struct descriptor."""
    if len(registers) != 18:
        raise ValueError(
            f"ARM coredumps require exactly 18 registers context "
            f"(got {len(registers)})"
        )

    buf = io.BytesIO()
    w = BinaryWriter(buf, byteorder)

    # struct elf_prstatus (148 bytes total on 32-bit ARM)
    # References:
    # https://github.com/ptesarik/libkdumpfile/blob/v0.5.5/src/kdumpfile/arm.c
    # https://android.googlesource.com/platform/bionic/+/848247a/libc/kernel/arch-arm/asm/ptrace.h
    # struct elf_siginfo pr_info
    w.write_u32(0)  # si_signo (signal number)
    w.write_u32(0)  # si_code (extra code)
    w.write_u32(0)  # si_errno (errno)

    w.write_u16(0)  # pr_cursig
    w.write_u16(0)  # _pad1
    w.write_u32(0)  # pr_sigpend
    w.write_u32(0)  # pr_sighold
    w.write_u32(thread_id)  # pr_pid
    w.write_u32(0)  # pr_ppid
    w.write_u32(0)  # pr_pgrp
    w.write_u32(0)  # pr_sid
    w.write_u64(0)  # pr_utime (timeval: 2 * 32-bit is 64-bit total)
    w.write_u64(0)  # pr_stime (timeval: 2 * 32-bit is 64-bit total)
    w.write_u64(0)  # pr_cutime (timeval: 2 * 32-bit is 64-bit total)
    w.write_u64(0)  # pr_cstime (timeval: 2 * 32-bit is 64-bit total)

    # pr_reg: GP registers array (18 registers, 4 bytes each).
    # This maps the standard ELF EABI `elf_gregset_t` registers context
    # expected by GDB under EM_ARM coredump files.
    # The format is generic and applies to all 32-bit ARM profiles (Cortex-A,
    # Cortex-R, and Cortex-M), mapping exactly 18 general-purpose registers:
    # r0-r12, sp (r13), lr (r14), pc (r15), psr (r16), and orig_r0 (r17).
    for reg in registers:
        w.write_u32(reg)

    w.write_u32(0)  # pr_fpvalid (float valid flag)

    return buf.getvalue()


# Architectural registry mapping target platform key (Machine, Class) to
# its corresponding elf_prstatus note formatter function.
_PRSTATUS_FORMATTERS = {
    (ElfMachine.ARM, ElfClass.CLASS32): _format_prstatus_arm32,
}


class CoredumpBuilder(ElfBuilder):
    """Facilitates programmatically building crash coredump ELF files."""

    def __init__(
        self,
        elf_machine: ElfMachine,
        elf_class: ElfClass,
        byteorder: ByteOrder,
    ) -> None:
        """Initializes the CoredumpBuilder.

        Args:
            elf_machine: Target CPU architecture (ARM, etc.).
            elf_class: Target ELF class bit width (CLASS32 or CLASS64).
            byteorder: Endianness representation (little or big).
        """
        # Coredump files are strictly of type CORE!
        super().__init__(
            elf_machine=elf_machine,
            elf_class=elf_class,
            byteorder=byteorder,
            elf_type=ElfType.CORE,
        )

    def add_build_id(self, build_id: bytes) -> None:
        """Adds a GNU build ID note segment to the coredump.

        Note:
            This note is informational only. Standard debuggers (GDB, LLDB)
            resolve a crashed process's build ID post-mortem by parsing its
            memory-mapped first page (which maps the binary's ELF header).

            Because bare-metal embedded targets boot directly from flash memory
            (Execute In Place) and do not map their ELF headers in memory,
            debuggers cannot resolve the build ID automatically.

            Adding a GNU build ID note segment to the coredump itself allows
            host-side triage processors to easily match the post-mortem dump
            to its corresponding symbols binary.

        Args:
            build_id: The binary software build uuid / signature.
        """
        self.add_note(note_type=_NT_GNU_BUILD_ID, name=b"GNU", desc=build_id)

    def add_thread(self, *, thread_id: int, registers: list[int]) -> None:
        """Adds a thread register dump (PRSTATUS note) to the coredump.

        Args:
            thread_id: The unique numeric thread identifier (PID).
            registers: The general-purpose register values (GP registers).
                For ARM (32-bit), exactly 18 register values must be supplied
                mapping r0-r12, sp, lr, pc, psr, and orig_r0.

        Raises:
            ValueError: If target architecture/class is unsupported, or
                the register list size is incorrect.
        """
        key = (self._elf_machine, self._elf_class)
        formatter = _PRSTATUS_FORMATTERS.get(key)
        if formatter is None:
            raise NotImplementedError(
                f"PRSTATUS builder not supported for target: "
                f"{self._elf_machine.name} ({self._elf_class.name})"
            )

        desc = formatter(thread_id, registers, self._byteorder)
        self.add_note(note_type=_NT_PRSTATUS, name=b"CORE", desc=desc)
