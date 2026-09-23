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
"""Tests for PW_ENUM() parsing."""

import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from pw_enum.parse import (
    ParseError,
    EnumValue,
    EnumDescriptor,
    _filter_flags,
    _find_line_and_file_for_offset,
    _parse_preprocessed_header,
    _parse_evaluation_output,
    _resolve_compiler,
    _same_file,
    camel_to_upper_snake,
    _default_display_name,
)

# pylint: disable=too-many-public-methods,missing-function-docstring


class TestEnumHParser(unittest.TestCase):
    """Test cases for .enum.h parsing."""

    def setUp(self):
        self.test_dir = (
            tempfile.TemporaryDirectory()  # pylint: disable=consider-using-with
        )
        self.addCleanup(self.test_dir.cleanup)

    def test_basic_parse(self) -> None:
        test_file = Path(self.test_dir.name) / "test_enum.h"
        test_file.write_text("")
        text = (
            f'# 1 "{test_file.resolve()}"\n'
            '_PW_ENUM_MUST_BE_COMPILED_BY_pw_cc_enum( '
            '"my::ns::MyEnum", "kValueA", "kValueB" ) _PW_ENUM_MACRO_END;'
        )
        enums = _parse_preprocessed_header(text, test_file)
        self.assertEqual(len(enums), 1)
        e = enums[0]
        self.assertEqual(e.name, "MyEnum")
        self.assertEqual(e.scopes, ("my", "ns"))
        self.assertEqual(e.cc_full_name, "::my::ns::MyEnum")
        self.assertEqual(e.values[0], ("kValueA", "VALUE_A"))
        self.assertEqual(e.values[1], ("kValueB", "VALUE_B"))

        # Test evaluation simulation (similar to parse_enums replacement)
        eval_values = {0: 0, 1: 1}
        final_descriptors = []
        target_idx = 0
        for enum in enums:
            new_values = []
            for cc_name, display_name in enum.values:
                val = eval_values[target_idx]
                target_idx += 1
                new_values.append(
                    EnumValue(cc_name=cc_name, value=val, name=display_name)
                )
            final_descriptors.append(
                EnumDescriptor(
                    name=enum.name,
                    scopes=enum.scopes,
                    cc_full_name=enum.cc_full_name,
                    line=enum.line,
                    values=tuple(new_values),
                )
            )

        self.assertEqual(len(final_descriptors), 1)
        desc = final_descriptors[0]
        self.assertEqual(desc.values[0].cc_name, "kValueA")
        self.assertEqual(desc.values[0].value, 0)
        self.assertEqual(desc.values[1].cc_name, "kValueB")
        self.assertEqual(desc.values[1].value, 1)

    def test_custom_strings(self) -> None:
        test_file = Path(self.test_dir.name) / "test_enum.h"
        test_file.write_text("")
        text = (
            f'# 1 "{test_file.resolve()}"\n'
            '_PW_ENUM_MUST_BE_COMPILED_BY_pw_cc_enum(\n'
            '  "my::ns::MyEnum",\n'
            '  "kValueA = \\"CUSTOM A\\"",\n'
            '  "kValueB = \\"custom \\\\\\\"nested\\\\\\\" b\\"",\n'
            '  "kValueC = \\"a + b - c\\"",\n'
            '  "kValueD"\n'
            ') _PW_ENUM_MACRO_END;'
        )
        enums = _parse_preprocessed_header(text, test_file)
        self.assertEqual(len(enums), 1)
        e = enums[0]
        self.assertEqual(e.values[0], ("kValueA", "CUSTOM A"))
        self.assertEqual(e.values[1], ("kValueB", 'custom "nested" b'))
        self.assertEqual(e.values[2], ("kValueC", "a + b - c"))
        self.assertEqual(e.values[3], ("kValueD", "VALUE_D"))

        # Test evaluation simulation string resolution
        eval_values = {0: 1, 1: 2, 2: 3, 3: 4}
        final_descriptors = []
        target_idx = 0
        for enum in enums:
            new_values = []
            for cc_name, display_name in enum.values:
                val = eval_values[target_idx]
                target_idx += 1
                new_values.append(
                    EnumValue(cc_name=cc_name, value=val, name=display_name)
                )
            final_descriptors.append(
                EnumDescriptor(
                    name=enum.name,
                    scopes=enum.scopes,
                    cc_full_name=enum.cc_full_name,
                    line=enum.line,
                    values=tuple(new_values),
                )
            )

        self.assertEqual(final_descriptors[0].values[0].name, "CUSTOM A")
        self.assertEqual(final_descriptors[0].values[0].value, 1)
        self.assertEqual(
            final_descriptors[0].values[1].name, 'custom "nested" b'
        )
        self.assertEqual(final_descriptors[0].values[1].value, 2)
        self.assertEqual(final_descriptors[0].values[2].name, "a + b - c")
        self.assertEqual(final_descriptors[0].values[2].value, 3)
        self.assertEqual(final_descriptors[0].values[3].name, "VALUE_D")
        self.assertEqual(final_descriptors[0].values[3].value, 4)

    def test_class_nested_enum(self) -> None:
        test_file = Path(self.test_dir.name) / "test_enum.h"
        test_file.write_text("")
        text = (
            f'# 1 "{test_file.resolve()}"\n'
            '_PW_ENUM_MUST_BE_COMPILED_BY_pw_cc_enum( '
            '"my::ns::MyClass::MyEnum", "kValueA", "kValueB" ) '
            '_PW_ENUM_MACRO_END;'
        )
        enums = _parse_preprocessed_header(text, test_file)
        e = enums[0]
        self.assertEqual(e.name, "MyEnum")
        self.assertEqual(e.scopes, ("my", "ns", "MyClass"))

    def test_multiple_enums(self) -> None:
        test_file = Path(self.test_dir.name) / "test_enum.h"
        test_file.write_text("")
        text = (
            f'# 1 "{test_file.resolve()}"\n'
            '_PW_ENUM_MUST_BE_COMPILED_BY_pw_cc_enum( '
            '"my::ns::E1", "kA" ) _PW_ENUM_MACRO_END;\n'
            f'# 3 "{test_file.resolve()}"\n'
            '_PW_ENUM_MUST_BE_COMPILED_BY_pw_cc_enum( '
            '"my::ns::E2", "kB" ) _PW_ENUM_MACRO_END;\n'
        )
        enums = _parse_preprocessed_header(text, test_file)
        self.assertEqual(len(enums), 2)
        self.assertEqual(enums[0].name, "E1")
        self.assertEqual(enums[1].name, "E2")

    def test_nested_namespaces(self) -> None:
        test_file = Path(self.test_dir.name) / "test_enum.h"
        test_file.write_text("")
        text = (
            f'# 1 "{test_file.resolve()}"\n'
            '_PW_ENUM_MUST_BE_COMPILED_BY_pw_cc_enum( '
            '"a::b::E", "kA" ) _PW_ENUM_MACRO_END;'
        )
        enums = _parse_preprocessed_header(text, test_file)
        self.assertEqual(enums[0].scopes, ("a", "b"))

    def test_leading_colon_scopes(self) -> None:
        test_file = Path(self.test_dir.name) / "test_enum.h"
        test_file.write_text("")
        text = (
            f'# 1 "{test_file.resolve()}"\n'
            '_PW_ENUM_MUST_BE_COMPILED_BY_pw_cc_enum( '
            '"::a::b::E", "kA" ) _PW_ENUM_MACRO_END;'
        )
        enums = _parse_preprocessed_header(text, test_file)
        self.assertEqual(enums[0].name, "E")
        self.assertEqual(enums[0].scopes, ("a", "b"))
        self.assertEqual(enums[0].cc_full_name, "::a::b::E")
        desc = EnumDescriptor(
            name=enums[0].name,
            scopes=enums[0].scopes,
            cc_full_name=enums[0].cc_full_name,
            line=enums[0].line,
            values=(),
        )
        self.assertTrue(desc.cc_versioned_name.startswith("::a::b::"))

    def test_top_level_enum_error(self) -> None:
        test_file = Path(self.test_dir.name) / "test_enum.h"
        test_file.write_text("PW_ENUM(TopLevel, kA);")
        text = (
            f'# 1 "{test_file.resolve()}"\n'
            '_PW_ENUM_MUST_BE_COMPILED_BY_pw_cc_enum( '
            '"TopLevel", "kA" ) _PW_ENUM_MACRO_END;'
        )
        with self.assertRaisesRegex(
            ParseError,
            "PW_ENUM requires a fully qualified name with at least one "
            "namespace",
        ):
            _parse_preprocessed_header(text, test_file)

    def test_empty_enum(self) -> None:
        test_file = Path(self.test_dir.name) / "test_enum.h"
        test_file.write_text("")
        text = (
            f'# 1 "{test_file.resolve()}"\n'
            '_PW_ENUM_MUST_BE_COMPILED_BY_pw_cc_enum( '
            '"ns::Empty", ) _PW_ENUM_MACRO_END;'
        )
        enums = _parse_preprocessed_header(text, test_file)
        self.assertEqual(len(enums), 1)
        self.assertEqual(enums[0].name, "Empty")
        self.assertEqual(len(enums[0].values), 0)

    def test_single_enumerator_enum(self) -> None:
        test_file = Path(self.test_dir.name) / "test_enum.h"
        test_file.write_text("")
        text = (
            f'# 1 "{test_file.resolve()}"\n'
            '_PW_ENUM_MUST_BE_COMPILED_BY_pw_cc_enum( '
            '"ns::One", "kVal", ) _PW_ENUM_MACRO_END;'
        )
        enums = _parse_preprocessed_header(text, test_file)
        self.assertEqual(len(enums), 1)
        self.assertEqual(enums[0].name, "One")
        self.assertEqual(
            tuple(cc_name for cc_name, _ in enums[0].values), ("kVal",)
        )

    def test_empty_custom_string(self) -> None:
        test_file = Path(self.test_dir.name) / "test_enum.h"
        test_file.write_text("")

        text = (
            f'# 1 "{test_file.resolve()}"\n'
            '_PW_ENUM_MUST_BE_COMPILED_BY_pw_cc_enum( '
            '"ns::EmptyCustom", "kVal = \\"\\"", ) _PW_ENUM_MACRO_END;'
        )
        with self.assertRaisesRegex(
            ParseError,
            "cannot be empty",
        ):
            _parse_preprocessed_header(text, test_file)

    def test_whitespace_and_nonprintable_custom_strings(self) -> None:
        test_file = Path(self.test_dir.name) / "test_enum.h"
        test_file.write_text("")

        # Whitespace-only custom string is allowed
        text = (
            f'# 1 "{test_file.resolve()}"\n'
            '_PW_ENUM_MUST_BE_COMPILED_BY_pw_cc_enum( '
            '"ns::SpaceCustom", "kVal = \\"   \\"", ) _PW_ENUM_MACRO_END;'
        )
        enums = _parse_preprocessed_header(text, test_file)
        self.assertEqual(enums[0].values[0][1], "   ")

        # C++ hex escapes are handled correctly
        text = (
            f'# 1 "{test_file.resolve()}"\n'
            '_PW_ENUM_MUST_BE_COMPILED_BY_pw_cc_enum( '
            '"ns::HexCustom", "kVal = \\"\\\\x1b\\"", ) _PW_ENUM_MACRO_END;'
        )
        enums = _parse_preprocessed_header(text, test_file)
        self.assertEqual(enums[0].values[0][1], "\x1b")

        # C++ newline escapes become newlines
        text = (
            f'# 1 "{test_file.resolve()}"\n'
            '_PW_ENUM_MUST_BE_COMPILED_BY_pw_cc_enum( '
            '"ns::NewlineCustom", "kVal = \\"Foo\\\\nBar\\"", ) '
            '_PW_ENUM_MACRO_END;'
        )
        enums = _parse_preprocessed_header(text, test_file)
        self.assertEqual(enums[0].values[0][1], "Foo\nBar")

    def test_parse_evaluation_output_error(self) -> None:
        test_file = Path(self.test_dir.name) / "test_enum.h"
        test_file.write_text("some content line 1\n...\nline 10 | error here")
        mock_stderr = (
            f'{test_file.resolve()}:10:5: error: '
            "use of undeclared identifier 'UNKNOWN'\n"
        )

        with self.assertRaises(ParseError) as cm:
            list(
                _parse_evaluation_output(
                    mock_stderr,
                    [("kA", "A")],
                )
            )
        self.assertIn(
            "Failed to evaluate value for kA!",
            str(cm.exception),
        )

    def test_parse_evaluation_output_success(self) -> None:
        test_file = Path(self.test_dir.name) / "test_enum.h"
        test_file.write_text("")
        mock_stderr = (
            "PwEnumValueExtractor<0, 42>\nPwEnumValueExtractor<1, 100>\n"
        )
        targets = [
            ("kA", "A"),
            ("kB", "B"),
        ]

        eval_values = list(
            _parse_evaluation_output(
                mock_stderr,
                targets,
            )
        )
        self.assertEqual(len(eval_values), 2)
        self.assertEqual(eval_values[0], EnumValue("kA", 42, "A"))
        self.assertEqual(eval_values[1], EnumValue("kB", 100, "B"))

    def test_missing_macro_error(self) -> None:
        test_file = Path(self.test_dir.name) / "test_enum.h"
        test_file.write_text("")
        with self.assertRaisesRegex(
            ParseError,
            "pw_cc_enum headers must contain at least one call to PW_ENUM",
        ):
            _parse_preprocessed_header("", test_file)

    def test_non_utf8_custom_string_error(self) -> None:
        test_file = Path(self.test_dir.name) / "test_enum.h"
        test_file.write_text("")

        # Hex escape representing invalid UTF-8 byte 0xFF
        text = (
            f'# 1 "{test_file.resolve()}"\n'
            '_PW_ENUM_MUST_BE_COMPILED_BY_pw_cc_enum( '
            '"ns::InvalidCustom", "kVal = \\"\\\\xff\\"", ) '
            '_PW_ENUM_MACRO_END;'
        )
        with self.assertRaisesRegex(
            ParseError,
            "is not valid UTF-8",
        ):
            _parse_preprocessed_header(text, test_file)

    def test_ignore_included_headers_enums(self) -> None:
        foo_file = Path(self.test_dir.name) / "foo.h"
        bar_file = Path(self.test_dir.name) / "bar.h"
        foo_file.write_text("")
        bar_file.write_text("")

        # Simulated preprocessed output where foo.h includes bar.h.
        # Markers for both files are present.
        text = (
            f'# 1 "{foo_file.resolve()}"\n'
            '_PW_ENUM_MUST_BE_COMPILED_BY_pw_cc_enum( '
            '"ns::FooEnum", "kFooVal" ) _PW_ENUM_MACRO_END;\n'
            f'# 1 "{bar_file.resolve()}"\n'
            '_PW_ENUM_MUST_BE_COMPILED_BY_pw_cc_enum( '
            '"ns::BarEnum", "kBarVal" ) _PW_ENUM_MACRO_END;\n'
        )

        # Parsing foo_file should only return FooEnum and ignore BarEnum.
        enums = _parse_preprocessed_header(text, foo_file)
        self.assertEqual(len(enums), 1)
        self.assertEqual(enums[0].name, "FooEnum")

        # Parsing bar_file should only return BarEnum and ignore FooEnum.
        enums = _parse_preprocessed_header(text, bar_file)
        self.assertEqual(len(enums), 1)
        self.assertEqual(enums[0].name, "BarEnum")

    def test_missing_macro_error_with_other_file_enums(self) -> None:
        foo_file = Path(self.test_dir.name) / "foo.h"
        bar_file = Path(self.test_dir.name) / "bar.h"
        foo_file.write_text("")
        bar_file.write_text("")

        # Text has enums for bar.h, but we are parsing foo.h.
        text = (
            f'# 1 "{bar_file.resolve()}"\n'
            '_PW_ENUM_MUST_BE_COMPILED_BY_pw_cc_enum( '
            '"ns::BarEnum", "kBarVal" ) _PW_ENUM_MACRO_END;\n'
        )

        with self.assertRaisesRegex(
            ParseError,
            "pw_cc_enum headers must contain at least one call to PW_ENUM",
        ):
            _parse_preprocessed_header(text, foo_file)

    def test_empty_enumerator_error(self) -> None:
        test_file = Path(self.test_dir.name) / "test_enum.h"
        test_file.write_text("")
        text = (
            f'# 1 "{test_file.resolve()}"\n'
            '_PW_ENUM_MUST_BE_COMPILED_BY_pw_cc_enum( '
            '"ns::MyEnum", "kVal", "", "kVal2" ) _PW_ENUM_MACRO_END;'
        )
        with self.assertRaisesRegex(
            ParseError,
            "Invalid enumerator name",
        ):
            _parse_preprocessed_header(text, test_file)

    def test_windows_escaped_line_markers(self) -> None:
        test_file = Path(self.test_dir.name) / "test_enum.h"
        test_file.write_text("")
        # GCC on Windows escapes backslashes in preprocessor # line directives,
        # e.g. `# 1 "C:\\dir\\test_enum.h"`. Convert to POSIX first so `/` is
        # always the separator before replacing with `\\` on both Windows and
        # POSIX hosts.
        escaped_path = test_file.resolve().as_posix().replace("/", "\\\\")
        self.assertIn("\\\\", escaped_path)
        self.assertNotIn("/", escaped_path)
        text = (
            f'# 1 "{escaped_path}"\n'
            '_PW_ENUM_MUST_BE_COMPILED_BY_pw_cc_enum( '
            '"my::ns::MyEnum", "kValueA", "kValueB" ) _PW_ENUM_MACRO_END;'
        )
        self.assertEqual(
            _find_line_and_file_for_offset(text, len(text))[1],
            test_file.resolve(),
        )
        enums = _parse_preprocessed_header(text, test_file)
        self.assertEqual(len(enums), 1)
        self.assertEqual(enums[0].name, "MyEnum")
        self.assertEqual(enums[0].values[0], ("kValueA", "VALUE_A"))

    def test_find_line_and_file_windows_literal_paths(self) -> None:
        # Verify both escaped (double) and unescaped (single) backslash paths in
        # # line directives using literal Windows strings.
        for raw_marker_path in (
            r"C:\\Users\\pw\\dir\\my_enum.h",
            r"C:\Users\pw\dir\my_enum.h",
        ):
            source = f'# 42 "{raw_marker_path}"\nint x;\nint y;\n'
            line, file_path = _find_line_and_file_for_offset(
                source, len(source)
            )
            self.assertEqual(line, 44, msg=raw_marker_path)
            self.assertEqual(
                file_path.as_posix(),
                "C:/Users/pw/dir/my_enum.h",
                msg=raw_marker_path,
            )


class TestSameFile(unittest.TestCase):
    """Test path comparison across OS path quirks."""

    def setUp(self) -> None:
        self.test_dir = (
            tempfile.TemporaryDirectory()  # pylint: disable=consider-using-with
        )
        self.addCleanup(self.test_dir.cleanup)
        self.base = Path(self.test_dir.name)
        (self.base / "sub").mkdir()

    def test_existing_file_reached_by_different_paths(self) -> None:
        test_file = self.base / "test_enum.h"
        test_file.write_text("")
        indirect = self.base / "sub" / ".." / "test_enum.h"
        self.assertTrue(_same_file(test_file, indirect))

    def test_different_existing_files(self) -> None:
        first = self.base / "a.h"
        second = self.base / "b.h"
        first.write_text("")
        second.write_text("")
        self.assertFalse(_same_file(first, second))

    def test_missing_files_are_compared_by_path(self) -> None:
        indirect = self.base / "sub" / ".." / "no.h"
        self.assertTrue(_same_file(self.base / "no.h", indirect))
        self.assertFalse(_same_file(self.base / "no.h", self.base / "other.h"))

    def test_pseudo_file_line_markers(self) -> None:
        # GCC names pseudo-files in line markers, e.g. `# 1 "<built-in>"`.
        # These cannot be stat()ed, and are not even legal Windows file names.
        test_file = self.base / "test_enum.h"
        test_file.write_text("")
        self.assertFalse(_same_file(Path("<built-in>"), test_file))
        self.assertTrue(_same_file(Path("<built-in>"), Path("<built-in>")))

    def test_case_is_folded_on_windows_only(self) -> None:
        self.assertEqual(
            _same_file(self.base / "no.h", self.base / "NO.h"),
            os.name == "nt",
        )


class TestResolveCompiler(unittest.TestCase):
    """Test Windows compiler executable resolution."""

    def test_not_windows_is_a_no_op(self) -> None:
        with mock.patch("pw_enum.parse.shutil.which") as which:
            self.assertEqual(
                _resolve_compiler("../bin/clang++", windows=False),
                "../bin/clang++",
            )
        which.assert_not_called()

    def test_bare_name_is_not_resolved(self) -> None:
        # Windows appends PATHEXT extensions to bare names on its own.
        with mock.patch("pw_enum.parse.shutil.which") as which:
            self.assertEqual(
                _resolve_compiler("clang++", windows=True), "clang++"
            )
        which.assert_not_called()

    def test_path_is_resolved_to_executable(self) -> None:
        with mock.patch(
            "pw_enum.parse.shutil.which", return_value="../bin/clang++.exe"
        ):
            self.assertEqual(
                _resolve_compiler("../bin/clang++", windows=True),
                "../bin/clang++.exe",
            )

    def test_unresolvable_path_is_returned_unmodified(self) -> None:
        with mock.patch("pw_enum.parse.shutil.which", return_value=None):
            self.assertEqual(
                _resolve_compiler("../bin/clang++", windows=True),
                "../bin/clang++",
            )


class TestFilterFlags(unittest.TestCase):
    """Test compiler flag filtering."""

    def test_filter_flags_windows_paths(self) -> None:
        flags = [
            "-Ifoo/bar",
            "-c",
            r"pw_strict_host_gcc_debug\gen\my_enum\my_enum.base.cc",
            "-o",
            r"pw_strict_host_gcc_debug\gen\my_enum\my_enum.base.o",
            "-DDEBUG",
        ]
        base_cc = "pw_strict_host_gcc_debug/gen/my_enum/my_enum.base.cc"
        filtered = list(_filter_flags(flags, base_cc))
        self.assertEqual(filtered, ["-Ifoo/bar", "-DDEBUG"])


class TestDefaultDisplayName(unittest.TestCase):
    """Test default display name resolution logic."""

    def test_default_display_name(self) -> None:
        self.assertEqual(_default_display_name("kFooBar"), "FOO_BAR")
        self.assertEqual(_default_display_name("FooBar"), "FooBar")
        self.assertEqual(_default_display_name("kool"), "kool")
        self.assertEqual(_default_display_name("kO"), "O")
        self.assertEqual(_default_display_name("k"), "k")


class TestCamelToUpperSnake(unittest.TestCase):
    """Test CamelCase to UPPER_SNAKE_CASE conversion."""

    def test_camel_to_upper_snake(self) -> None:
        self.assertEqual(camel_to_upper_snake("MyEnum"), "MY_ENUM")
        self.assertEqual(camel_to_upper_snake("myEnum"), "MY_ENUM")
        self.assertEqual(camel_to_upper_snake("HTTPServer"), "HTTP_SERVER")
        self.assertEqual(camel_to_upper_snake("JSONParser"), "JSON_PARSER")
        self.assertEqual(camel_to_upper_snake("URL"), "URL")


if __name__ == "__main__":
    unittest.main()
