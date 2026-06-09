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
"""Tests for pw_elf.coredump"""

import io
import unittest

from elftools.elf.elffile import ELFFile

from pw_elf.builder import ElfClass, ElfMachine
from pw_elf.coredump import CoredumpBuilder


def _build_to_elf(builder: CoredumpBuilder) -> ELFFile:
    buffer = io.BytesIO()
    builder.build(buffer)
    buffer.seek(0)
    return ELFFile(buffer)


class CoredumpBuilderTest(unittest.TestCase):
    """CoredumpBuilder tests."""

    def test_coredump_builder_arm32(self) -> None:
        """Verify CoredumpBuilder correctly packages 32-bit ARM coredumps."""
        builder = CoredumpBuilder(ElfMachine.ARM, ElfClass.CLASS32, "little")
        # Add thread 1 (PID = 123) with standard 18 ARM GP registers context:
        # r0-r12, sp, lr, pc, psr, orig_r0
        regs = [0] * 13 + [
            0x20002000,
            0x08001000,
            0x08002000,
            0x01000000,
            0xFFFFFFFF,
        ]
        builder.add_thread(thread_id=123, registers=regs)

        # Add a mock thread stack memory region as PT_LOAD
        builder.add_memory_region(0x20001000, b"Stack data payload")

        elf = _build_to_elf(builder)

        # 1. Verify standard ELF header
        self.assertEqual(elf["e_type"], "ET_CORE")
        self.assertEqual(elf["e_machine"], "EM_ARM")
        self.assertEqual(elf.elfclass, 32)

        # 2. Verify dynamic segment headers lists
        segments = list(elf.iter_segments())
        self.assertEqual(len(segments), 2)  # 1 Note segment + 1 Load segment!

        note_seg = segments[0]
        self.assertEqual(note_seg["p_type"], "PT_NOTE")
        self.assertEqual(note_seg["p_align"], 4)  # ELF32 note align!

        load_seg = segments[1]
        self.assertEqual(load_seg["p_type"], "PT_LOAD")
        self.assertEqual(load_seg["p_vaddr"], 0x20001000)
        self.assertEqual(load_seg.data(), b"Stack data payload")

        # 3. Verify packaged notes content
        notes = list(note_seg.iter_notes())
        self.assertEqual(len(notes), 1)  # Exactly 1 PRSTATUS note!

        # PRSTATUS note
        pr_status = notes[0]
        self.assertEqual(pr_status["n_type"], "NT_PRSTATUS")
        self.assertEqual(pr_status["n_name"], "CORE")
        desc = pr_status["n_desc"]
        self.assertEqual(len(desc), 148)  # 32-bit ARM elf_prstatus struct size!
        # Verify pid field offset is correct (pid is at byte offset 24)
        self.assertEqual(int.from_bytes(desc[24:28], "little"), 123)

    def test_coredump_builder_build_id(self) -> None:
        """Verify that CoredumpBuilder can package build IDs in coredumps."""
        builder = CoredumpBuilder(ElfMachine.ARM, ElfClass.CLASS32, "little")
        build_id = b"ABCDEF0123456789ABCD"
        builder.add_build_id(build_id)

        elf = _build_to_elf(builder)
        # Verify the note segment is present with the exact build ID size.
        # We can't use pyelftools note_seg.iter_notes() because it mishandles
        # NT_GNU_BUILD_ID in ET_CORE files.
        # See https://github.com/eliben/pyelftools/issues/656
        segments = list(elf.iter_segments())
        self.assertEqual(len(segments), 1)
        note_seg = segments[0]
        self.assertEqual(note_seg["p_type"], "PT_NOTE")
        # ELF Note Header has 12 bytes + Name length (NUL terminated/aligned:
        # 4 bytes) + Desc length (20 bytes) = 36 bytes!
        self.assertEqual(note_seg["p_filesz"], 36)

    def test_coredump_builder_unsupported_fails(self) -> None:
        """Verify CoredumpBuilder raises expected exceptions on failures."""
        # 1. 64-bit target fails with NotImplementedError
        builder64 = CoredumpBuilder(ElfMachine.ARM, ElfClass.CLASS64, "little")
        with self.assertRaisesRegex(
            NotImplementedError,
            r"PRSTATUS builder not supported for target: ARM \(CLASS64\)",
        ):
            builder64.add_thread(thread_id=1, registers=[0] * 18)

        # 2. Non-ARM architecture target fails with NotImplementedError
        builder_xtensa = CoredumpBuilder(
            ElfMachine.XTENSA, ElfClass.CLASS32, "little"
        )
        with self.assertRaisesRegex(
            NotImplementedError,
            r"PRSTATUS builder not supported for target: XTENSA \(CLASS32\)",
        ):
            builder_xtensa.add_thread(thread_id=1, registers=[0] * 18)

        # 3. Bad registers list length fails with ValueError
        builder_arm = CoredumpBuilder(
            ElfMachine.ARM, ElfClass.CLASS32, "little"
        )
        with self.assertRaisesRegex(
            ValueError, "ARM coredumps require exactly 18 registers context"
        ):
            # Pass only 17 registers (invalid!)
            builder_arm.add_thread(thread_id=1, registers=[0] * 17)


if __name__ == "__main__":
    unittest.main()
