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
from pw_enum.parse import EnumDescriptor, EnumValue, StatusInfo


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
        self.assertIn("#define MY_NS_MY_ENUM_FMT PW_LOG_TOKEN_FMT", footer)
        self.assertIn(
            '#define MY_NS_MY_ENUM_DOMAIN "::my::ns::_pw_enum_', footer
        )
        self.assertIn('PW_LOG_TOKEN_FMT("::my::ns::_pw_enum_', footer)
        self.assertIn("struct EnumTraits<::my::ns::MyEnum>", footer)
        # pw::has_enum_traits_v detects this tag, so every specialization must
        # carry it.
        self.assertIn(
            "static constexpr bool _pw_enum_internal_tag_do_not_use = true;",
            footer,
        )
        self.assertIn(
            'static constexpr std::string_view kName = "MyEnum";', footer
        )
        self.assertIn(
            "static constexpr std::string_view kFullyQualifiedName = "
            '"::my::ns::MyEnum";',
            footer,
        )
        self.assertIn(
            "static constexpr std::string_view kTokenDomain = "
            '"::my::ns::_pw_enum_',
            footer,
        )
        self.assertIn(
            "static constexpr size_t kDistinctValueCount = 2;", footer
        )
        self.assertIn("static constexpr bool kIsContiguous = true;", footer)
        self.assertIn(
            "static constexpr enum_type kMin = ::my::ns::MyEnum::kMyValue;",
            footer,
        )
        self.assertIn(
            "static constexpr enum_type kMax = ::my::ns::MyEnum::kB;", footer
        )
        self.assertIn(
            "static constexpr std::array<enum_type, kDistinctValueCount> "
            "kValues = {{",
            footer,
        )
        self.assertIn(
            "  [[nodiscard]] static constexpr bool IsValid(enum_type value) {\n"
            "    return value >= kMin && value <= kMax;\n"
            "  }",
            footer,
        )
        self.assertIn(
            "  [[nodiscard]] static constexpr const char* ToString("
            "enum_type value) {\n"
            "    switch (value) {\n"
            "      case ::my::ns::MyEnum::kMyValue:\n"
            '        return "MY_VALUE";\n'
            "      case ::my::ns::MyEnum::kB:\n"
            '        return "B";\n'
            "    }\n"
            '    return "Unknown ::my::ns::MyEnum value";\n'
            "  }",
            footer,
        )
        self.assertIn("_PW_TOKENIZE_ENUM_DOMAIN(::my::ns::MyEnum,", footer)
        self.assertIn('(kMyValue, "MY_VALUE")', footer)
        self.assertIn('(kB, "B")', footer)

    def test_traits_single_value(self) -> None:
        """Test that a single-valued enum is validated with an equality."""
        enums = [
            EnumDescriptor(
                name="OnlyOne",
                scopes=("test",),
                cc_full_name="::test::OnlyOne",
                line=0,
                values=(EnumValue(cc_name="kOnly", value=7, name="ONLY"),),
            )
        ]
        footer = "\n".join(generate_footer(enums)) + "\n"
        self.assertIn(
            "static constexpr size_t kDistinctValueCount = 1;", footer
        )
        self.assertIn("static constexpr bool kIsContiguous = true;", footer)
        self.assertIn("return value == kMin;", footer)

    def test_traits_non_contiguous_with_aliases(self) -> None:
        """Test traits generation for non-contiguous enums with aliases."""
        enums = [
            EnumDescriptor(
                name="SparseEnum",
                scopes=("test",),
                cc_full_name="::test::SparseEnum",
                line=0,
                values=(
                    EnumValue(cc_name="kA", value=1, name="A"),
                    EnumValue(cc_name="kAliasA", value=1, name="ALIAS_A"),
                    EnumValue(cc_name="kB", value=5, name="B"),
                ),
            )
        ]
        footer = "\n".join(generate_footer(enums)) + "\n"
        self.assertIn("struct EnumTraits<::test::SparseEnum>", footer)
        # Aliases count as a single distinct value.
        self.assertIn(
            "static constexpr size_t kDistinctValueCount = 2;", footer
        )
        self.assertIn("static constexpr bool kIsContiguous = false;", footer)
        self.assertIn(
            "static constexpr enum_type kMin = ::test::SparseEnum::kA;", footer
        )
        self.assertIn(
            "static constexpr enum_type kMax = ::test::SparseEnum::kB;", footer
        )
        # kValues contains only kA and kB; the alias is omitted.
        self.assertIn("::test::SparseEnum::kA,", footer)
        self.assertIn("::test::SparseEnum::kB,", footer)
        self.assertNotIn("::test::SparseEnum::kAliasA,", footer)
        # Non-contiguous enums fall back to a switch in IsValid.
        self.assertIn(
            "  [[nodiscard]] static constexpr bool IsValid(enum_type value) {\n"
            "    switch (value) {\n"
            "      case ::test::SparseEnum::kA:\n"
            "      case ::test::SparseEnum::kB:\n"
            "        return true;\n"
            "      default:\n"
            "        return false;\n"
            "    }\n"
            "  }",
            footer,
        )
        self.assertNotIn("case ::test::SparseEnum::kAliasA:", footer)

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

    def test_version_separates_fields(self) -> None:
        """Test that values and names that concatenate equally differ."""
        enum1 = EnumDescriptor(
            name="MyEnum",
            scopes=("my", "ns"),
            cc_full_name="::my::ns::MyEnum",
            line=0,
            values=(EnumValue(cc_name="kA", value=1, name="0X"),),
        )
        enum2 = EnumDescriptor(
            name="MyEnum",
            scopes=("my", "ns"),
            cc_full_name="::my::ns::MyEnum",
            line=0,
            # "1" + "0X" and "10" + "X" are both "10X".
            values=(EnumValue(cc_name="kA", value=10, name="X"),),
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
        self.assertIn("#define MY_ENUM_FMT PW_LOG_TOKEN_FMT", footer)
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

    def test_resolve_gn_escaped_quotes_in_defines(self) -> None:
        """Test that shell-escaped quotes survive on either platform."""
        with tempfile.TemporaryDirectory() as td:
            temp_dir = Path(td)
            toolchain_file = temp_dir / "toolchain.ninja"
            target_file = temp_dir / "target.ninja"

            toolchain_file.write_text(
                "rule cxx\n  command = clang++ $defines -c $in -o $out\n"
            )
            target_file.write_text('defines = -DNAME=\\"value\\"\n')

            for windows in (False, True):
                _, raw_flags = _resolve_gn_compiler_and_flags(
                    toolchain_file,
                    target_file,
                    Path("my_target.base.cc"),
                    windows=windows,
                )
                self.assertEqual(
                    raw_flags[0], '-DNAME="value"', msg=f"{windows=}"
                )

    def test_resolve_gn_windows_paths(self) -> None:
        """Test parsing backslash paths from ninja files written on Windows."""
        with tempfile.TemporaryDirectory() as td:
            temp_dir = Path(td)
            toolchain_file = temp_dir / "toolchain.ninja"
            target_file = temp_dir / "target.ninja"

            toolchain_file.write_text(
                "rule cxx\n"
                "  command = ..\\..\\environment\\cipd\\packages\\arm\\bin\\"
                "arm-none-eabi-g++ $defines $include_dirs $cflags "
                "-c $in -o $out\n"
            )
            # The trailing separator on the second include dir must not escape
            # the space that follows it.
            target_file.write_text(
                "defines = -DNAME=VALUE\n"
                "include_dirs = -I..\\..\\path\\to\\include -Igen\\include\\\n"
                "cflags = -O2\n"
            )
            win_base_cc = Path(
                r"pw_strict_host_gcc_debug\gen\my_target\my_target.base.cc"
            )
            compiler, raw_flags = _resolve_gn_compiler_and_flags(
                toolchain_file, target_file, win_base_cc, windows=True
            )
            self.assertEqual(
                compiler,
                "../../environment/cipd/packages/arm/bin/arm-none-eabi-g++",
            )
            self.assertEqual(
                raw_flags,
                [
                    "-DNAME=VALUE",
                    "-I../../path/to/include",
                    "-Igen/include/",
                    "-O2",
                    "-c",
                    "pw_strict_host_gcc_debug/gen/my_target/my_target.base.cc",
                    "-o",
                    "pw_strict_host_gcc_debug/gen/my_target/my_target.base.o",
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

    def test_status_enum_generation(self) -> None:
        """Test footer generation for PW_ENUM_STATUS."""
        enums = [
            EnumDescriptor(
                name="MyStatusCode",
                scopes=("my", "ns"),
                cc_full_name="::my::ns::MyStatusCode",
                line=5,
                values=(
                    EnumValue(cc_name="kOk", value=0, name="OK"),
                    EnumValue(cc_name="kNotFound", value=1, name="NOT_FOUND"),
                ),
                status=StatusInfo(
                    name="MyStatus",
                    scopes=("my", "ns"),
                    cc_full_name="::my::ns::MyStatus",
                    ok_code="kOk",
                ),
            )
        ]
        footer = "\n".join(generate_footer(enums)) + "\n"
        self.assertIn('#include "pw_status/status_base.h"', footer)
        self.assertIn('#include "pw_tokenizer/enum.h"', footer)
        self.assertIn("namespace my::ns {", footer)
        self.assertIn(
            "class [[nodiscard]] MyStatus final\n"
            "    : public ::pw::StatusBase<MyStatus, MyStatusCode::kOk> {",
            footer,
        )
        self.assertNotIn("using code_type", footer)
        self.assertNotIn("using Code =", footer)
        self.assertNotIn("using enum ", footer)
        self.assertNotIn("static constexpr MyStatus Ok()", footer)
        self.assertIn(
            "[[nodiscard]] static constexpr MyStatus NotFound() {\n"
            "    return MyStatus(Code::kNotFound);\n"
            "  }",
            footer,
        )
        self.assertIn(
            "  [[nodiscard]] constexpr bool IsNotFound() const {\n"
            "    return code() == Code::kNotFound;\n"
            "  }",
            footer,
        )
        self.assertNotIn("IsOk()", footer)
        self.assertIn(
            "  // PW_ENUM_STATUS: No Is*() function for kOk: use ok() instead.",
            footer,
        )
        self.assertIn("constexpr MyStatus() = default;", footer)
        self.assertIn("explicit constexpr MyStatus(Code code)", footer)
        self.assertIn(
            ": ::pw::StatusBase<MyStatus, MyStatusCode::kOk>(code) {}",
            footer,
        )
        self.assertIn("struct EnumTraits<::my::ns::MyStatusCode>", footer)
        self.assertIn(
            'static constexpr std::string_view kName = "MyStatusCode";', footer
        )
        self.assertIn(
            'static constexpr std::string_view kFullyQualifiedName = '
            '"::my::ns::MyStatusCode";',
            footer,
        )
        self.assertIn(
            "_PW_TOKENIZE_ENUM_DOMAIN(::my::ns::MyStatusCode,", footer
        )

    def test_status_enum_no_factory_for_ok_code(self) -> None:
        """Factories are omitted for the OK code and its aliases."""
        enums = [
            EnumDescriptor(
                name="HwStatusCode",
                scopes=("hw",),
                cc_full_name="::hw::HwStatusCode",
                line=5,
                values=(
                    EnumValue(cc_name="kBusy", value=0, name="BUSY"),
                    EnumValue(cc_name="kReady", value=1, name="READY"),
                    EnumValue(cc_name="kDone", value=1, name="DONE"),
                    EnumValue(cc_name="kError", value=2, name="ERROR"),
                ),
                status=StatusInfo(
                    name="HwStatus",
                    scopes=("hw",),
                    cc_full_name="::hw::HwStatus",
                    ok_code="kReady",
                ),
            )
        ]
        footer = "\n".join(generate_footer(enums)) + "\n"
        self.assertIn(
            "    : public ::pw::StatusBase<HwStatus, HwStatusCode::kReady> {",
            footer,
        )
        self.assertIn("static constexpr HwStatus Busy()", footer)
        self.assertIn("static constexpr HwStatus Error()", footer)
        self.assertNotIn("static constexpr HwStatus Ready()", footer)
        self.assertNotIn("static constexpr HwStatus Done()", footer)
        # Is*() functions are generated for OK codes not named OK.
        self.assertIn("constexpr bool IsBusy() const", footer)
        self.assertIn("constexpr bool IsReady() const", footer)
        self.assertIn("constexpr bool IsDone() const", footer)
        self.assertIn("constexpr bool IsError() const", footer)
        self.assertIn(
            "  // PW_ENUM_STATUS: No factory for kReady: OK code; use "
            "HwStatus() instead.",
            footer,
        )
        self.assertIn(
            "  // PW_ENUM_STATUS: No factory for kDone: OK code; use "
            "HwStatus() instead.",
            footer,
        )

    def test_status_enum_factory_skip_comments(self) -> None:
        """Skipped factories are noted in comments in the generated code."""
        enums = [
            EnumDescriptor(
                name="SkipCode",
                scopes=("ns",),
                cc_full_name="::ns::SkipCode",
                line=5,
                values=(
                    EnumValue(cc_name="kOk", value=0, name="OK"),
                    EnumValue(cc_name="notKStyle", value=1, name="notKStyle"),
                    EnumValue(cc_name="kCode", value=2, name="CODE"),
                    EnumValue(cc_name="kSkip", value=3, name="SKIP"),
                ),
                status=StatusInfo(
                    name="Skip",
                    scopes=("ns",),
                    cc_full_name="::ns::Skip",
                    ok_code="kOk",
                ),
            )
        ]
        footer = "\n".join(generate_footer(enums)) + "\n"
        self.assertIn(
            "  // PW_ENUM_STATUS: No factory for notKStyle: not a "
            "kEnumName-style name.",
            footer,
        )
        self.assertIn(
            "  // PW_ENUM_STATUS: No Is*() function for notKStyle: not a "
            "kEnumName-style name.",
            footer,
        )
        self.assertIn(
            "  // PW_ENUM_STATUS: No factory for kCode: Code() conflicts with "
            "a StatusBase member.",
            footer,
        )
        # Is*() functions are still generated when the factory is skipped.
        self.assertIn("constexpr bool IsCode() const", footer)
        self.assertIn("constexpr bool IsSkip() const", footer)
        self.assertIn(
            "  // PW_ENUM_STATUS: No factory for kSkip: Skip() conflicts with "
            "a StatusBase member.",
            footer,
        )

    def test_status_enum_is_function_conflicts_with_factory(self) -> None:
        """Is*() functions that collide with a factory are skipped."""
        enums = [
            EnumDescriptor(
                name="ConflictCode",
                scopes=("ns",),
                cc_full_name="::ns::ConflictCode",
                line=5,
                values=(
                    EnumValue(cc_name="kOk", value=0, name="OK"),
                    EnumValue(cc_name="kIsFull", value=1, name="IS_FULL"),
                    EnumValue(cc_name="kFull", value=2, name="FULL"),
                ),
                status=StatusInfo(
                    name="Conflict",
                    scopes=("ns",),
                    cc_full_name="::ns::Conflict",
                    ok_code="kOk",
                ),
            )
        ]
        footer = "\n".join(generate_footer(enums)) + "\n"
        self.assertIn("static constexpr Conflict IsFull() {", footer)
        self.assertIn("constexpr bool IsIsFull() const", footer)
        self.assertNotIn("constexpr bool IsFull() const", footer)
        self.assertIn(
            "  // PW_ENUM_STATUS: No Is*() function for kFull: IsFull() is "
            "already defined.",
            footer,
        )

    def test_write_generated_header_status_enum(self) -> None:
        """Test header rewriting for PW_ENUM_STATUS."""
        enums = [
            EnumDescriptor(
                name="MyStatusCode",
                scopes=("my", "ns"),
                cc_full_name="::my::ns::MyStatusCode",
                line=10,
                values=(
                    EnumValue(cc_name="kOk", value=0, name="OK"),
                    EnumValue(cc_name="kNotFound", value=1, name="NOT_FOUND"),
                ),
                status=StatusInfo(
                    name="MyStatus",
                    scopes=("my", "ns"),
                    cc_full_name="::my::ns::MyStatus",
                    ok_code="kOk",
                ),
            )
        ]

        text = (
            "namespace my::ns {\n\n"
            "enum class MyStatusCode : uint8_t {\n"
            "  kOk,\n"
            "  kNotFound,\n"
            "};\n\n"
            "}  // namespace my::ns\n\n"
            "PW_ENUM_STATUS("
            "my::ns::MyStatus, "
            "my::ns::MyStatusCode::kOk, "
            "kOk, kNotFound);\n"
        )

        with tempfile.TemporaryDirectory() as td:
            temp_dir = Path(td)
            input_file = temp_dir / "input.h"
            output_file = temp_dir / "output.h"
            input_file.write_text(text)
            _write_generated_header(input_file, output_file, enums)
            output_text = output_file.read_text()

        self.assertIn(
            "enum class MyStatusCode : uint8_t {",
            output_text,
        )
        self.assertIn(
            "_PW_ENUM_GENERATED(my::ns::MyStatus, my::ns::MyStatusCode::kOk, "
            "kOk, kNotFound);",
            output_text,
        )
        self.assertIn('#include "pw_status/status_base.h"', output_text)


if __name__ == "__main__":
    unittest.main()
