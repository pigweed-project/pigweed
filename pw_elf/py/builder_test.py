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
"""Tests for elf"""

import dataclasses
import io
import unittest

from elftools.elf.elffile import ELFFile
from elftools.elf.segments import NoteSegment

from pw_elf.builder import (
    ElfBuilder,
    ElfClass,
    ElfMachine,
    ElfType,
)


def _build_to_elf(builder: ElfBuilder) -> ELFFile:
    buffer = io.BytesIO()
    builder.build(buffer)
    buffer.seek(0)
    return ELFFile(buffer)


def _single(seq):
    results = list(seq)
    if len(results) != 1:
        raise ValueError("Expected one item")
    return results[0]


@dataclasses.dataclass(frozen=True)
class DataRegion:
    addr: int
    data: bytes


@dataclasses.dataclass(frozen=True)
class TestNote:
    type: int
    name: str
    desc: bytes


class ElfBuilderTest(unittest.TestCase):
    """ElfBuilder tests."""

    def test_elf_empty(self) -> None:  # pylint: disable=no-self-use
        builder = ElfBuilder(
            ElfMachine.ARM, ElfClass.CLASS32, "little", ElfType.CORE
        )
        _build_to_elf(builder)  # Verify it is a valid ELF file.

    def test_elf_type_core(self) -> None:
        builder = ElfBuilder(
            ElfMachine.ARM, ElfClass.CLASS32, "little", ElfType.CORE
        )
        elf = _build_to_elf(builder)
        self.assertEqual(elf.header["e_type"], "ET_CORE")

    def test_elf_type_exec(self) -> None:
        builder = ElfBuilder(
            ElfMachine.ARM, ElfClass.CLASS32, "little", ElfType.EXEC
        )
        elf = _build_to_elf(builder)
        self.assertEqual(elf.header["e_type"], "ET_EXEC")

    def test_elf_byteorder_little(self) -> None:
        builder = ElfBuilder(
            ElfMachine.ARM, ElfClass.CLASS32, "little", ElfType.CORE
        )
        buffer = io.BytesIO()
        builder.build(buffer)
        raw_bytes = buffer.getvalue()

        # Verify EI_DATA indicates little-endian (LSB = 1)
        self.assertEqual(raw_bytes[5], 1)
        # Verify e_machine (0x0028) is written as LSB: 28 00
        self.assertEqual(raw_bytes[18:20], b"\x28\x00")

    def test_elf_byteorder_big(self) -> None:
        builder = ElfBuilder(
            ElfMachine.ARM, ElfClass.CLASS32, "big", ElfType.CORE
        )
        buffer = io.BytesIO()
        builder.build(buffer)
        raw_bytes = buffer.getvalue()

        # Verify EI_DATA indicates big-endian (MSB = 2)
        self.assertEqual(raw_bytes[5], 2)
        # Verify e_machine (0x0028) is written as MSB: 00 28
        self.assertEqual(raw_bytes[18:20], b"\x00\x28")

    def test_elf_invalid_byteorder(self) -> None:
        with self.assertRaisesRegex(ValueError, "Invalid byteorder: invalid"):
            ElfBuilder(
                ElfMachine.ARM,
                ElfClass.CLASS32,
                "invalid",  # type: ignore[arg-type]
                ElfType.CORE,
            )

    def test_elf_invalid_class(self) -> None:
        with self.assertRaisesRegex(ValueError, "Invalid ELF class: 3"):
            ElfBuilder(
                ElfMachine.ARM,
                3,  # type: ignore[arg-type]
                "little",
                ElfType.CORE,
            )

    def test_elf64_empty(self) -> None:
        builder = ElfBuilder(
            ElfMachine.ARM, ElfClass.CLASS64, "little", ElfType.CORE
        )
        elf = _build_to_elf(builder)
        self.assertEqual(elf.elfclass, 64)
        self.assertEqual(elf.header["e_ehsize"], 64)
        self.assertEqual(elf.header["e_phentsize"], 56)

    def test_elf64_type_exec(self) -> None:
        builder = ElfBuilder(
            ElfMachine.ARM, ElfClass.CLASS64, "little", ElfType.EXEC
        )
        elf = _build_to_elf(builder)
        self.assertEqual(elf.header["e_type"], "ET_EXEC")

    def test_elf64_memory_regions(self) -> None:
        """Verify that ElfBuilder can write 64-bit memory regions."""
        regions = (
            DataRegion(addr=0xBBBB000011112222, data=b"Other 64-bit data"),
            DataRegion(addr=0xAAAA000033334444, data=b"Some test 64-bit data"),
        )

        builder = ElfBuilder(
            ElfMachine.ARM, ElfClass.CLASS64, "little", ElfType.CORE
        )
        for r in regions:
            builder.add_memory_region(r.addr, r.data)

        elf = _build_to_elf(builder)
        self.assertEqual(elf.elfclass, 64)

        for r, seg in zip(
            sorted(regions, key=lambda r: r.addr),
            elf.iter_segments(),
            strict=True,
        ):
            self.assertEqual(seg["p_vaddr"], r.addr)
            self.assertEqual(seg.data(), r.data)

    def test_elf64_notes(self) -> None:
        """Verify that 64-bit note segments are 8-byte aligned."""
        test_notes = (TestNote(type=123, name="A", desc=b"B"),)

        builder = ElfBuilder(
            ElfMachine.ARM, ElfClass.CLASS64, "little", ElfType.CORE
        )
        for n in test_notes:
            builder.add_note(
                note_type=n.type,
                name=n.name.encode("ascii"),
                desc=n.desc,
            )

        elf = _build_to_elf(builder)
        note_seg = _single(
            seg for seg in elf.iter_segments() if isinstance(seg, NoteSegment)
        )
        self.assertEqual(note_seg["p_align"], 8)

        elf_note = _single(note_seg.iter_notes())
        self.assertEqual(elf_note["n_name"], "A")
        self.assertEqual(elf_note["n_type"], 123)
        self.assertEqual(elf_note["n_desc"], b"B")

    def test_elf_memory_regions(self) -> None:
        """Verify that ElfBuilder can write memory regions."""
        regions = (
            DataRegion(addr=0xBBBB0000, data=b"Other data here"),
            DataRegion(addr=0xAAAA0000, data=b"Some test data"),
            DataRegion(addr=0x104, data=b"4567"),
            DataRegion(addr=0x100, data=b"0123"),
        )

        builder = ElfBuilder(
            ElfMachine.ARM, ElfClass.CLASS32, "little", ElfType.CORE
        )
        for r in regions:
            builder.add_memory_region(r.addr, r.data)

        elf = _build_to_elf(builder)

        for r, seg in zip(
            sorted(regions, key=lambda r: r.addr),
            elf.iter_segments(),
            strict=True,
        ):
            self.assertEqual(seg["p_vaddr"], r.addr)
            self.assertEqual(seg.data(), r.data)

    def test_mem_region_conflict_predecessor(self) -> None:
        """Verify add_memory_region() prevents overlap with predecessor."""
        builder = ElfBuilder(
            ElfMachine.ARM, ElfClass.CLASS32, "little", ElfType.CORE
        )
        builder.add_memory_region(0x100, b"12345678")  # 0x100 - 0x108
        with self.assertRaisesRegex(
            ValueError, "0x104-0x106 overlaps existing 0x100-0x108"
        ):
            builder.add_memory_region(0x104, b"xx")  # 0x104-0x106

    def test_mem_region_conflict_successor(self) -> None:
        """Verify add_memory_region() prevents overlap with successor."""
        builder = ElfBuilder(
            ElfMachine.ARM, ElfClass.CLASS32, "little", ElfType.CORE
        )
        builder.add_memory_region(0x108, b"12345678")  # 0x108 - 0x110
        with self.assertRaisesRegex(
            ValueError, "0x104-0x10c overlaps existing 0x108-0x110"
        ):
            builder.add_memory_region(0x104, b"12345678")  # 0x104-0x10c

    def test_elf_notes(self) -> None:
        """Verify that ElfBuilder can write notes."""
        test_notes = (
            TestNote(type=123, name="NaMe", desc=b"Test note data"),
            TestNote(type=5678, name="LongerName", desc=b"Other data"),
        )

        builder = ElfBuilder(
            ElfMachine.ARM, ElfClass.CLASS32, "little", ElfType.CORE
        )
        for n in test_notes:
            builder.add_note(
                note_type=n.type,
                name=n.name.encode("ascii"),
                desc=n.desc,
            )

        elf = _build_to_elf(builder)

        note_seg = _single(
            (seg for seg in elf.iter_segments() if isinstance(seg, NoteSegment))
        )
        elf_notes = list(note_seg.iter_notes())

        for test_note, elf_note in zip(test_notes, elf_notes, strict=True):
            self.assertEqual(elf_note["n_name"], test_note.name)
            self.assertEqual(elf_note["n_type"], test_note.type)
            self.assertEqual(elf_note["n_desc"], test_note.desc)

    def test_elf32_memory_region_custom_alignment(self) -> None:
        """Verify that memory regions respect custom alignment."""
        builder = ElfBuilder(
            ElfMachine.ARM, ElfClass.CLASS32, "little", ElfType.CORE
        )
        # Add region with custom alignment (4096 / 4KB boundary)
        builder.add_memory_region(0x10000, b"Aligned data", align=4096)

        elf = _build_to_elf(builder)
        seg = _single(elf.iter_segments())

        # Verify alignment documents properly in the program header
        self.assertEqual(seg["p_align"], 4096)
        # Verify the file offset and vaddr are congruent modulo p_align
        self.assertEqual(seg["p_offset"] % 4096, seg["p_vaddr"] % 4096)

    def test_elf_memory_region_invalid_alignment(self) -> None:
        """Verify non-power-of-two alignment fails."""
        builder = ElfBuilder(
            ElfMachine.ARM, ElfClass.CLASS32, "little", ElfType.CORE
        )
        with self.assertRaisesRegex(
            ValueError, "Alignment must be a positive power of two: 10"
        ):
            builder.add_memory_region(0x100, b"data", align=10)

    def test_elf_memory_region_unaligned_fails(self) -> None:
        """Verify that adding an unaligned virtual address raises ValueError."""
        builder = ElfBuilder(
            ElfMachine.ARM, ElfClass.CLASS32, "little", ElfType.CORE
        )
        # Verify unaligned virtual addresses fail validation check!
        with self.assertRaisesRegex(
            ValueError, "Address 0x1004 must be aligned to 16 bytes"
        ):
            builder.add_memory_region(0x1004, b"data", align=16)


if __name__ == "__main__":
    unittest.main()
