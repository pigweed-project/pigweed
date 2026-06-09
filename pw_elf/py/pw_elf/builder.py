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
"""Classes for working with ELF files."""

import abc
import bisect
import dataclasses
import enum
from typing import IO

from pw_bytes.io import BinaryWriter, ByteOrder


_EI_NIDENT = 16
_ELFCLASS32 = 1
_ELFCLASS64 = 2
_ELFDATA2LSB = 1
_ELFDATA2MSB = 2
_EV_CURRENT = 1
_PT_LOAD = 1
_PT_NOTE = 4


class ElfMachine(enum.IntEnum):
    ARM = 40
    XTENSA = 94


class ElfClass(enum.IntEnum):
    CLASS32 = 1
    CLASS64 = 2


class ElfType(enum.IntEnum):
    NONE = 0
    REL = 1
    EXEC = 2
    DYN = 3
    CORE = 4


@dataclasses.dataclass
class _ElfPhdr:
    """Represents an ELF Program header (struct Elf32_Phdr / Elf64_Phdr)"""

    p_type: int = 0
    p_offset: int = 0
    p_vaddr: int = 0
    p_paddr: int = 0
    p_filesz: int = 0
    p_memsz: int = 0
    p_flags: int = 0
    p_align: int = 0


@dataclasses.dataclass
class _ElfNote:
    """Represents an ELF Note."""

    note_type: int
    name: bytes
    desc: bytes


class _ElfWriter(abc.ABC):
    """Abstract base class for directly writing ELF files."""

    def __init__(self, writer: IO[bytes], byteorder: ByteOrder):
        self.writer = BinaryWriter(writer, byteorder)
        self.byteorder: ByteOrder = byteorder

    @property
    def offset(self) -> int:
        return self.writer.tell()

    def seek(self, off: int) -> None:
        self.writer.seek(off)

    def write(self, data: bytes) -> None:
        self.writer.write(data)

    def write_half(self, val: int) -> None:
        """Writes a 16-bit ELF half-word (Elf32_Half / Elf64_Half)."""
        self.writer.write_u16(val)

    def write_word(self, val: int) -> None:
        """Writes a 32-bit ELF word (Elf32_Word / Elf64_Word)."""
        self.writer.write_u32(val)

    def align_to(self, align: int) -> None:
        while self.offset % align != 0:
            self.writer.write_u8(0)

    @property
    @abc.abstractmethod
    def elf_class(self) -> ElfClass:
        """The ELF class (32-bit or 64-bit) of the writer."""

    @property
    @abc.abstractmethod
    def ehdr_size(self) -> int:
        """Size of the ELF file header (Ehdr / EHDR_SIZE)."""

    @property
    @abc.abstractmethod
    def phdr_size(self) -> int:
        """Size of a program header entry (Phdr / PHDR_SIZE)."""

    @property
    @abc.abstractmethod
    def shdr_size(self) -> int:
        """Size of a section header entry (Shdr / SHDR_SIZE)."""

    @property
    @abc.abstractmethod
    def note_align(self) -> int:
        """Alignment constraint for note sections (SHT_NOTE)."""

    @abc.abstractmethod
    def write_addr(self, val: int) -> None:
        """Write an ELF address (Elf32_Addr / Elf64_Addr)."""

    @abc.abstractmethod
    def write_off(self, val: int) -> None:
        """Write an ELF offset (Elf32_Off / Elf64_Off)."""

    @abc.abstractmethod
    def write_phdr(self, phdr: _ElfPhdr) -> None:
        """Write an ELF program header."""

    def write_ehdr(
        self,
        num_phdr: int,
        machine: int,
        elf_type: ElfType,
    ) -> None:
        """Writes the ELF header."""
        ident = bytearray(_EI_NIDENT)
        ident[0:4] = b"\x7fELF"
        ident[4] = self.elf_class
        if self.byteorder == "little":
            ident[5] = _ELFDATA2LSB
        elif self.byteorder == "big":
            ident[5] = _ELFDATA2MSB
        else:
            raise ValueError(f"Invalid byteorder: {self.byteorder}")
        ident[6] = _EV_CURRENT

        phoff = self.ehdr_size if num_phdr > 0 else 0

        self.write(ident)  # e_ident
        self.write_half(elf_type)  # e_type
        self.write_half(machine)  # e_machine
        self.write_word(_EV_CURRENT)  # e_version
        self.write_addr(0)  # e_entry
        self.write_off(phoff)  # e_phoff
        self.write_off(0)  # e_shoff
        self.write_word(0)  # e_flags
        self.write_half(self.ehdr_size)  # e_ehsize
        self.write_half(self.phdr_size)  # e_phentsize
        self.write_half(num_phdr)  # e_phnum
        self.write_half(self.shdr_size)  # e_shentsize
        self.write_half(0)  # e_shnum
        self.write_half(0)  # e_shstrndx

    def write_note(self, note: _ElfNote) -> None:
        align = self.note_align
        # The ELF specification requires note entries to be aligned.
        if self.offset % align != 0:
            raise ValueError(f"ELF note must start on a {align}-byte boundary")
        self.write_word(len(note.name) + 1)  # n_namesz (NUL terminated)
        self.write_word(len(note.desc))  # n_descsz
        self.write_word(note.note_type)  # n_type

        self.write(note.name + b"\x00")  # name
        self.align_to(align)

        self.write(note.desc)
        self.align_to(align)

        assert self.offset % align == 0


class _ElfWriter32(_ElfWriter):
    """Facilitates directly writing 32-bit ELF files."""

    @property
    def elf_class(self) -> ElfClass:
        return ElfClass.CLASS32

    @property
    def ehdr_size(self) -> int:
        return 52

    @property
    def phdr_size(self) -> int:
        return 32

    @property
    def shdr_size(self) -> int:
        return 40

    @property
    def note_align(self) -> int:
        return 4

    def write_addr(self, val: int) -> None:
        self.writer.write_u32(val)

    def write_off(self, val: int) -> None:
        self.writer.write_u32(val)

    def write_phdr(self, phdr: _ElfPhdr) -> None:
        self.write_word(phdr.p_type)
        self.write_off(phdr.p_offset)
        self.write_addr(phdr.p_vaddr)
        self.write_addr(phdr.p_paddr)
        self.write_word(phdr.p_filesz)
        self.write_word(phdr.p_memsz)
        self.write_word(phdr.p_flags)
        self.write_word(phdr.p_align)


class _ElfWriter64(_ElfWriter):
    """Facilitates directly writing 64-bit ELF files."""

    @property
    def elf_class(self) -> ElfClass:
        return ElfClass.CLASS64

    @property
    def ehdr_size(self) -> int:
        return 64

    @property
    def phdr_size(self) -> int:
        return 56

    @property
    def shdr_size(self) -> int:
        return 64

    @property
    def note_align(self) -> int:
        return 8

    def write_addr(self, val: int) -> None:
        self.writer.write_u64(val)

    def write_off(self, val: int) -> None:
        self.writer.write_u64(val)

    def write_xword(self, val: int) -> None:
        self.writer.write_u64(val)

    def write_phdr(self, phdr: _ElfPhdr) -> None:
        self.write_word(phdr.p_type)
        self.write_word(phdr.p_flags)  # p_flags is moved up!
        self.write_off(phdr.p_offset)
        self.write_addr(phdr.p_vaddr)
        self.write_addr(phdr.p_paddr)
        self.write_xword(phdr.p_filesz)
        self.write_xword(phdr.p_memsz)
        self.write_xword(phdr.p_align)


@dataclasses.dataclass(frozen=True)
class _MemoryRegion:
    address: int
    data: bytes
    align: int

    @property
    def length(self) -> int:
        return len(self.data)

    @property
    def end_address(self) -> int:
        """The first address *after* this region."""
        return self.address + self.length

    def contains(self, address: int) -> bool:
        """Checks if an address is within this memory region."""
        return self.address <= address < self.end_address

    def __str__(self) -> str:
        return f"{self.address:#x}-{self.end_address:#x}"


class ElfBuilder:
    """Facilitates building up an ELF image and then writing it to a file."""

    _WRITERS: dict[ElfClass, type[_ElfWriter]] = {
        ElfClass.CLASS32: _ElfWriter32,
        ElfClass.CLASS64: _ElfWriter64,
    }

    def __init__(
        self,
        elf_machine: ElfMachine,
        elf_class: ElfClass,
        byteorder: ByteOrder,
        elf_type: ElfType,
    ) -> None:
        """Initializes the ElfBuilder.

        Args:
            elf_machine: Target CPU architecture (ARM, XTENSA, etc.).
            elf_class: Target ELF class bit width (CLASS32 or CLASS64).
            byteorder: Endianness representation (little or big).
            elf_type: Target ELF object file type (CORE, EXEC, etc.).

        Raises:
            ValueError: If class or byteorder parameters are invalid.
        """
        if elf_class not in (ElfClass.CLASS32, ElfClass.CLASS64):
            raise ValueError(f"Invalid ELF class: {elf_class}")
        if byteorder not in ("little", "big"):
            raise ValueError(f"Invalid byteorder: {byteorder}")
        self._notes: list[_ElfNote] = []
        self._mem_regions: list[_MemoryRegion] = []
        self._elf_machine: ElfMachine = elf_machine
        self._elf_class: ElfClass = elf_class
        self._byteorder: ByteOrder = byteorder
        self._elf_type: ElfType = elf_type

    def add_note(self, note_type: int, name: bytes, desc: bytes) -> None:
        """Registers an ELF note to be written into a note segment.

        Args:
            note_type: Platform-specific note type code.
            name: Dynamic note name string (owner identifier).
            desc: Dynamic note descriptor payload.
        """
        note = _ElfNote(
            note_type=note_type,
            name=name,
            desc=desc,
        )
        self._notes.append(note)

    def add_memory_region(
        self,
        vaddr: int,
        data: bytes,
        align: int = 1,
    ) -> None:
        """Registers a loadable memory segment in the builder.

        Args:
            vaddr: Target virtual address where the segment is loaded.
            data: Raw binary byte segment payload.
            align: Dynamic segment alignment constraint in memory. Must
                be a positive power-of-two.

        Raises:
            ValueError: If alignment is invalid, the address is unaligned,
                or the segment overlaps with an existing region.
        """
        # Enforce spec constraint: align must be a positive power of two.
        if align <= 0 or (align & (align - 1)) != 0:
            raise ValueError(
                f"Alignment must be a positive power of two: {align}"
            )
        # Enforce spec constraint: virtual address must be aligned to align.
        if vaddr % align != 0:
            raise ValueError(
                f"Address {vaddr:#x} must be aligned to {align} bytes"
            )
        new_region = _MemoryRegion(address=vaddr, data=data, align=align)

        # Find insertion point (index of element to insert before).
        idx = bisect.bisect_left(
            self._mem_regions,
            new_region.address,
            key=lambda mem: mem.address,
        )

        # Check for overlap with predecessor.
        if idx > 0:
            predecessor = self._mem_regions[idx - 1]
            if predecessor.contains(new_region.address):
                raise ValueError(
                    f"New region {new_region} overlaps existing {predecessor}"
                )

        # Check for overlap with successor.
        if idx < len(self._mem_regions):
            successor = self._mem_regions[idx]
            # NOTE: this check is inverted from above
            if new_region.contains(successor.address):
                raise ValueError(
                    f"New region {new_region} overlaps existing {successor}"
                )

        self._mem_regions.insert(idx, new_region)

    def build(self, writer: IO[bytes]) -> None:
        """Builds and writes the ELF image to the underlying target.

        Args:
            writer: Output binary stream to write the ELF target into.
        """
        elf = self._WRITERS[self._elf_class](writer, byteorder=self._byteorder)

        # Write ELF header
        elf.write_ehdr(
            num_phdr=self._num_phdrs,
            machine=self._elf_machine,
            elf_type=self._elf_type,
        )

        # Write placeholder program headers; real ones will be filled out later.
        program_headers_offset = elf.ehdr_size
        assert elf.offset == program_headers_offset
        for _ in range(self._num_phdrs):
            elf.write_phdr(_ElfPhdr())
        phdrs = []

        # Start writing data, recording locations in phdrs.
        phdrs += self._write_notes(elf)
        phdrs += self._write_mem_regions(elf)

        # Done writing data, go back and write actual program headers.
        assert len(phdrs) == self._num_phdrs
        elf.seek(program_headers_offset)
        for phdr in phdrs:
            elf.write_phdr(phdr)

    @property
    def _num_phdrs(self) -> int:
        """Gets the total number of program headers to be written."""
        num = 0
        num += 1 if self._notes else 0
        num += len(self._mem_regions)
        return num

    def _write_notes(self, elf: _ElfWriter) -> list[_ElfPhdr]:
        if not self._notes:
            return []

        elf.align_to(elf.note_align)

        notes_off = elf.offset
        for note in self._notes:
            elf.write_note(note)
        notes_size = elf.offset - notes_off

        return [
            _ElfPhdr(
                p_type=_PT_NOTE,
                p_offset=notes_off,
                p_filesz=notes_size,
                p_align=elf.note_align,
            )
        ]

    def _write_mem_regions(self, elf: _ElfWriter) -> list[_ElfPhdr]:
        phdrs = []

        for mem in self._mem_regions:
            # Align in-file offset to the segment's requested alignment.
            elf.align_to(mem.align)

            region_off = elf.offset
            elf.write(mem.data)
            region_size = elf.offset - region_off

            phdrs.append(
                _ElfPhdr(
                    p_type=_PT_LOAD,
                    p_vaddr=mem.address,
                    p_offset=region_off,
                    p_filesz=region_size,
                    p_memsz=region_size,
                    p_align=mem.align,
                )
            )

        return phdrs
