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
"""Tests for the pw_enum code generator."""

import tempfile
import unittest
from pathlib import Path
from pw_enum.generate import (
    generate_footer,
    _write_generated_header,
)
from pw_enum.generate_gn import _resolve_gn_compiler_and_flags
from pw_enum.parse import EnumDescriptor, EnumValue


class TestEnumGeneration(unittest.TestCase):
    """Test cases for C++ footer generation."""

    def test_basic_generation(self) -> None:
        """Test basic footer generation with namespaces."""
        enums = [
            EnumDescriptor(
                name="MyEnum",
                scopes=("my", "ns"),
                cc_full_name="::my::ns::MyEnum",
                line=0,
                values=(
                    EnumValue(cc_name="kMyValue", value=1, name="MY_VALUE"),
                    EnumValue(cc_name="kB", value=2, name="B"),
                ),
            )
        ]
        footer = "\n".join(generate_footer(enums)) + "\n"
        self.assertIn("#define MY_NS_MY_ENUM PW_LOG_TOKEN_FMT", footer)
        self.assertIn(
            '#define MY_NS_MY_ENUM_DOMAIN "::my::ns::_pw_enum_', footer
        )
        self.assertIn('PW_LOG_TOKEN_FMT("::my::ns::_pw_enum_', footer)
        self.assertIn("_PW_TOKENIZE_ENUM_DOMAIN(::my::ns::MyEnum,", footer)
        self.assertIn('(kMyValue, "MY_VALUE")', footer)
        self.assertIn('(kB, "B")', footer)

    def test_duplicate_values_grouped(self) -> None:
        """Test that duplicate enum values are grouped alphabetically."""
        enums = [
            EnumDescriptor(
                name="MyEnum",
                scopes=("my", "ns"),
                cc_full_name="::my::ns::MyEnum",
                line=0,
                values=(
                    EnumValue(cc_name="kB", value=1, name="B"),
                    EnumValue(cc_name="kA", value=1, name="A"),
                    EnumValue(cc_name="kC", value=2, name="C"),
                ),
            )
        ]
        footer = "\n".join(generate_footer(enums)) + "\n"
        # Since 'A' < 'B', they are grouped as "A|B" and the case label is 'kA'
        self.assertIn('(kA, "A|B")', footer)
        self.assertNotIn('(kB, "B")', footer)
        self.assertIn('(kC, "C")', footer)

    def test_duplicate_values_with_same_display_name_merged(self) -> None:
        """Test merging duplicate enum values with identical display names."""
        enums = [
            EnumDescriptor(
                name="MyEnum",
                scopes=("my", "ns"),
                cc_full_name="::my::ns::MyEnum",
                line=0,
                values=(
                    EnumValue(cc_name="kB", value=1, name="A"),
                    EnumValue(cc_name="kA", value=1, name="A"),
                    EnumValue(cc_name="kC", value=2, name="C"),
                ),
            )
        ]
        footer = "\n".join(generate_footer(enums)) + "\n"
        # Since both map to "A", they should be merged to "A" instead of "A|A".
        # Since 'kB' and 'kA' both have value 1 and display name "A", they sort
        # equally, so stable sort keeps 'kB' first.
        self.assertIn('(kB, "A")', footer)
        self.assertNotIn('(kA, "A")', footer)
        self.assertNotIn('(kB, "A|A")', footer)
        self.assertIn('(kC, "C")', footer)

    def test_version_ignores_cc_name(self) -> None:
        """Test that version hash is independent of C++ identifier name."""
        enum1 = EnumDescriptor(
            name="MyEnum",
            scopes=("my", "ns"),
            cc_full_name="::my::ns::MyEnum",
            line=0,
            values=(EnumValue(cc_name="kA", value=1, name="A"),),
        )
        enum2 = EnumDescriptor(
            name="MyEnum",
            scopes=("my", "ns"),
            cc_full_name="::my::ns::MyEnum",
            line=0,
            # cc_name changed from kA to kA_New
            values=(EnumValue(cc_name="kA_New", value=1, name="A"),),
        )
        self.assertEqual(enum1.version, enum2.version)

    def test_version_hashes_display_name(self) -> None:
        """Test that version hash includes the display name."""
        enum1 = EnumDescriptor(
            name="MyEnum",
            scopes=("my", "ns"),
            cc_full_name="::my::ns::MyEnum",
            line=0,
            values=(EnumValue(cc_name="kA", value=1, name="A"),),
        )
        enum2 = EnumDescriptor(
            name="MyEnum",
            scopes=("my", "ns"),
            cc_full_name="::my::ns::MyEnum",
            line=0,
            # name changed from A to A_New
            values=(EnumValue(cc_name="kA", value=1, name="A_New"),),
        )
        self.assertNotEqual(enum1.version, enum2.version)

    def test_no_namespace(self) -> None:
        """Test footer generation for enums without a namespace."""
        enums = [
            EnumDescriptor(
                name="MyEnum",
                scopes=(),
                cc_full_name="::MyEnum",
                line=0,
                values=(EnumValue(cc_name="kA", value=1, name="A"),),
            )
        ]
        footer = "\n".join(generate_footer(enums)) + "\n"
        self.assertIn("#define MY_ENUM PW_LOG_TOKEN_FMT", footer)
        self.assertIn("_PW_TOKENIZE_ENUM_DOMAIN(::MyEnum,", footer)

    def test_empty_enum_skipped(self) -> None:
        """Test that empty enums generate an empty footer."""
        enums = [
            EnumDescriptor(
                name="MyEnum",
                scopes=("my", "ns"),
                cc_full_name="::my::ns::MyEnum",
                line=0,
                values=(),
            )
        ]
        footer = "\n".join(generate_footer(enums))
        self.assertEqual(footer, "")

    def test_resolve_gn_compiler_and_flags(self) -> None:
        """Test parsing compiler paths and flags from mock GN Ninja files."""
        with tempfile.TemporaryDirectory() as td:
            temp_dir = Path(td)
            toolchain_file = temp_dir / "toolchain.ninja"
            target_file = temp_dir / "target.ninja"
            base_cc = Path("my_target.base.cc")

            toolchain_file.write_text(
                "rule cxx\n"
                "  command = arm-none-eabi-g++ $defines $include_dirs "
                "$cflags -c $in -o $out\n"
            )
            target_file.write_text(
                "defines = -DNAME=VALUE\n"
                "include_dirs = -Ipath\n"
                "cflags = -O2 -mcpu=cortex-m4\n"
            )

            compiler, raw_flags = _resolve_gn_compiler_and_flags(
                toolchain_file, target_file, base_cc
            )
            self.assertEqual(compiler, "arm-none-eabi-g++")
            self.assertEqual(
                raw_flags,
                [
                    "-DNAME=VALUE",
                    "-Ipath",
                    "-O2",
                    "-mcpu=cortex-m4",
                    "-c",
                    "my_target.base.cc",
                    "-o",
                    "my_target.base.o",
                ],
            )

            # Test static analysis toolchain command wrapped with END_OF_INVOKER
            toolchain_file.write_text(
                "rule cxx\n"
                "  command = python3 clang_tidy.py --source-file $in -- "
                "arm-none-eabi-g++ END_OF_INVOKER $defines $include_dirs "
                "$cflags -c $in -o $out\n"
            )
            compiler, raw_flags = _resolve_gn_compiler_and_flags(
                toolchain_file, target_file, base_cc
            )
            self.assertEqual(compiler, "arm-none-eabi-g++")
            self.assertEqual(
                raw_flags,
                [
                    "-DNAME=VALUE",
                    "-Ipath",
                    "-O2",
                    "-mcpu=cortex-m4",
                    "-c",
                    "my_target.base.cc",
                    "-o",
                    "my_target.base.o",
                ],
            )

    def test_write_generated_header(self) -> None:
        """Test line replacement behavior of _write_generated_header."""
        enums = [
            EnumDescriptor(
                name="MyEnum",
                scopes=("my", "ns"),
                cc_full_name="::my::ns::MyEnum",
                line=2,
                values=(EnumValue(cc_name="kA", value=1, name="A"),),
            )
        ]

        with tempfile.TemporaryDirectory() as td:
            temp_dir = Path(td)
            input_file = temp_dir / "input.h"
            output_file = temp_dir / "output.h"

            # Line 2 is: PW_ENUM(my::ns::MyEnum, kA); // PW_ENUM comment
            input_file.write_text(
                "// Header file\n"
                "PW_ENUM(my::ns::MyEnum, kA); // comment with "
                "PW_ENUM(x) and MY_PW_ENUM(y)\n"
                "// Footer\n"
            )

            _write_generated_header(input_file, output_file, enums)
            output_text = output_file.read_text()

            # Check that both PW_ENUM on line 2 are replaced, but
            # MY_PW_ENUM is NOT.
            self.assertIn(
                "_PW_ENUM_GENERATED(my::ns::MyEnum, kA); // comment with "
                "_PW_ENUM_GENERATED(x) and MY_PW_ENUM(y)",
                output_text,
            )


if __name__ == "__main__":
    unittest.main()
