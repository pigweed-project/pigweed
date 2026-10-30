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
"""Tests for pw_codegen.cpp."""

import unittest

from pw_codegen import OutputFile, cpp


class CppIdentifiersTest(unittest.TestCase):
    """Tests C++ reserved word sets, validation, and identifier escaping."""

    def test_sets_partition(self) -> None:
        self.assertEqual(set(), cpp.KEYWORDS & cpp.STDLIB_MACROS)
        self.assertEqual(cpp.RESERVED_WORDS, cpp.KEYWORDS | cpp.STDLIB_MACROS)

    def test_keywords_and_macros(self) -> None:
        for word in ("alignas", "class", "consteval", "contract_assert", "xor"):
            self.assertIn(word, cpp.KEYWORDS)
            self.assertIn(word, cpp.RESERVED_WORDS)
        for macro in ("EOF", "NULL", "errno", "INT32_MAX", "stdout"):
            self.assertIn(macro, cpp.STDLIB_MACROS)
            self.assertIn(macro, cpp.RESERVED_WORDS)

    def test_is_identifier(self) -> None:
        self.assertTrue(cpp.is_identifier("my_var"))
        self.assertTrue(cpp.is_identifier("_leading"))
        self.assertTrue(cpp.is_identifier("MyEnum2"))
        self.assertFalse(cpp.is_identifier(""))
        self.assertFalse(cpp.is_identifier("2bad"))
        self.assertFalse(cpp.is_identifier("bad-name"))
        self.assertFalse(cpp.is_identifier("my::ns"))
        self.assertFalse(cpp.is_identifier("class"))
        self.assertFalse(cpp.is_identifier("errno"))
        self.assertFalse(cpp.is_identifier("EOF"))

    def test_fix_identifier(self) -> None:
        self.assertEqual(cpp.fix_identifier("normal_name"), "normal_name")
        self.assertEqual(cpp.fix_identifier("class"), "class_")
        self.assertEqual(cpp.fix_identifier("errno"), "errno_")
        self.assertEqual(cpp.fix_identifier("Message"), "Message")
        self.assertEqual(
            cpp.fix_identifier("Message", ("Fields", "Message")), "Message_"
        )

    def test_fix_enum_value_name(self) -> None:
        self.assertEqual(cpp.fix_enum_value_name("OK"), "OK")
        self.assertEqual(cpp.fix_enum_value_name("int"), "int")
        self.assertEqual(cpp.fix_enum_value_name("EOF"), "EOF_")
        self.assertEqual(cpp.fix_enum_value_name("eof"), "eof_")
        self.assertEqual(
            cpp.fix_enum_value_name("fields", {"FIELDS"}), "fields_"
        )
        self.assertEqual(
            cpp.fix_enum_value_name("message", ("Fields", "Message")), "message"
        )

    def test_constant_style(self) -> None:
        self.assertEqual(cpp.constant_style("A"), "kA")
        self.assertEqual(cpp.constant_style("IO_ERROR"), "kIoError")
        self.assertEqual(cpp.constant_style("HTTP2_ERROR"), "kHttp2Error")
        self.assertEqual(cpp.constant_style("V1_2"), "kV12")
        self.assertEqual(cpp.constant_style("my_field"), "kMyField")


class CppIntLiteralTest(unittest.TestCase):
    """Tests C++ integer types and literal formatting."""

    def test_int_types(self) -> None:
        self.assertEqual(cpp.INT_TYPES["int8"], ("int8_t", -128, 127))
        self.assertEqual(cpp.INT_TYPES["uint8"], ("uint8_t", 0, 255))
        self.assertEqual(
            cpp.INT_TYPES["int64"], ("int64_t", -(2**63), 2**63 - 1)
        )
        self.assertEqual(cpp.INT_TYPES["uint64"], ("uint64_t", 0, 2**64 - 1))

    def test_literals(self) -> None:
        cases = (
            ("int8", -128, "-128"),
            ("int8", 127, "127"),
            ("int32", 0, "0"),
            ("int32", 2**31 - 1, "2147483647"),
            ("int32", -(2**31 - 1), "-2147483647"),
            ("int32", -(2**31), "-2147483648ll"),
            ("int64", 2**40, "1099511627776ll"),
            ("int64", -(2**63), "(-9223372036854775807ll - 1)"),
            ("uint32", 42, "42"),
            ("uint32", 2**31, "2147483648u"),
            ("uint32", 2**32 - 1, "4294967295u"),
            ("uint64", 2**32, "4294967296ull"),
            ("uint64", 2**64 - 1, "18446744073709551615ull"),
        )
        for int_type, value, expected in cases:
            with self.subTest(int_type=int_type, value=value):
                self.assertEqual(cpp.int_literal(value, int_type), expected)

    def test_invalid_type_or_range(self) -> None:
        with self.assertRaisesRegex(ValueError, "Unsupported integer type"):
            cpp.int_literal(0, "int128")
        with self.assertRaisesRegex(ValueError, "out of range"):
            cpp.int_literal(256, "uint8")
        with self.assertRaisesRegex(ValueError, "out of range"):
            cpp.int_literal(-1, "uint32")
        with self.assertRaisesRegex(ValueError, "out of range"):
            cpp.int_literal(True, "int32")
        with self.assertRaisesRegex(ValueError, "out of range"):
            cpp.int_literal(1.5, "int32")  # type: ignore[arg-type]


class CppStringLiteralTest(unittest.TestCase):
    """Tests C++ string and byte literal escaping."""

    def test_basic_string(self) -> None:
        self.assertEqual(cpp.string_literal(""), '""')
        self.assertEqual(cpp.string_literal("hello"), '"hello"')

    def test_escapes(self) -> None:
        self.assertEqual(
            cpp.string_literal("hello\nworld\r\t\\"),
            r'"hello\nworld\r\t\\"',
        )
        self.assertEqual(cpp.string_literal('"quoted"'), r'"\"quoted\""')
        self.assertEqual(cpp.string_literal("\a\b\f\v"), r'"\a\b\f\v"')

    def test_bytes_and_utf8(self) -> None:
        self.assertEqual(cpp.string_literal(b"\x00\xff"), r'"\x00\xff"')
        self.assertEqual(cpp.string_literal(b"hello"), '"hello"')
        self.assertEqual(cpp.string_literal("é"), r'"\xc3\xa9"')

    def test_mixed_hex_and_ascii(self) -> None:
        self.assertEqual(cpp.string_literal(b"\xff\xfea"), r'"\xff\xfe""a"')
        self.assertEqual(cpp.string_literal(b"\xff\xfeF"), r'"\xff\xfe""F"')
        self.assertEqual(cpp.string_literal(b"\xff\xfe0"), r'"\xff\xfe""0"')
        self.assertEqual(cpp.string_literal(b"\xff\xfeg"), r'"\xff\xfeg"')
        self.assertEqual(cpp.string_literal(b"\xff\xfeG"), r'"\xff\xfeG"')
        self.assertEqual(cpp.string_literal(b"\xff\na"), r'"\xff\na"')


class CppDocCommentTest(unittest.TestCase):
    """Tests C++ /// doc comment generation."""

    def test_doc_comment(self) -> None:
        self.assertEqual(list(cpp.doc_comment("")), [])
        self.assertEqual(
            list(cpp.doc_comment("First line.\n\nSecond line.", indent="  ")),
            ["  /// First line.", "  ///", "  /// Second line."],
        )
        self.assertEqual(
            list(cpp.doc_comment("C:\\path\\file")),
            ["/// C:\\path\\file"],
        )

    def test_trailing_backslash_rejected(self) -> None:
        for doc in ("C:\\", "first\\\nsecond", "trailing space\\ "):
            with self.subTest(doc=doc):
                with self.assertRaisesRegex(ValueError, "backslash"):
                    list(cpp.doc_comment(doc))


class CppByteArrayTest(unittest.TestCase):
    """Tests C++ std::byte initializer formatting."""

    def test_single_line(self) -> None:
        self.assertEqual(cpp.byte_array(b""), "{}")
        self.assertEqual(
            cpp.byte_array(b"\x01\x0a\xff"),
            "{std::byte{0x01}, std::byte{0x0a}, std::byte{0xff}}",
        )

    def test_multiline_exact_and_remainder_chunks(self) -> None:
        self.assertEqual(cpp.byte_array(b"", bytes_per_line=4), "{}")
        self.assertEqual(
            cpp.byte_array(
                b"\x01\x02\x03\x04", bytes_per_line=2, indent="    "
            ),
            "{\n"
            "    std::byte{0x01}, std::byte{0x02},\n"
            "    std::byte{0x03}, std::byte{0x04},\n"
            "}",
        )
        self.assertEqual(
            cpp.byte_array(
                bytes(range(1, 11)), bytes_per_line=4, indent="    "
            ),
            "{\n"
            "    std::byte{0x01}, std::byte{0x02}, "
            "std::byte{0x03}, std::byte{0x04},\n"
            "    std::byte{0x05}, std::byte{0x06}, "
            "std::byte{0x07}, std::byte{0x08},\n"
            "    std::byte{0x09}, std::byte{0x0a},\n"
            "}",
        )

    def test_invalid_bytes_per_line(self) -> None:
        for bad in (0, -1):
            with self.assertRaisesRegex(ValueError, "bytes_per_line"):
                cpp.byte_array(b"\x01", bytes_per_line=bad)


class CppStructureTest(unittest.TestCase):
    """Tests C++ #include sorting and namespace helpers."""

    def test_includes_sorting_and_grouping(self) -> None:
        self.assertEqual(cpp.includes([]), [])
        self.assertEqual(
            cpp.includes(
                [
                    '"pw_foo/z.h"',
                    "<cstdint>",
                    '#include "pw_foo/a.h"',
                    "#include <array>",
                    "<cstdint>",
                ]
            ),
            [
                "#include <array>",
                "#include <cstdint>",
                "",
                '#include "pw_foo/a.h"',
                '#include "pw_foo/z.h"',
            ],
        )
        self.assertEqual(
            cpp.includes(['"b.h"', '"a.h"']),
            ['#include "a.h"', '#include "b.h"'],
        )
        self.assertEqual(
            cpp.includes(["<vector>", "<array>"]),
            ["#include <array>", "#include <vector>"],
        )

    def test_includes_invalid_rejected(self) -> None:
        for bad in ("foo.h", "<unclosed.h", '"unclosed.h', '""', "<>"):
            with self.subTest(bad=bad):
                with self.assertRaisesRegex(
                    ValueError, "Invalid C\\+\\+ #include"
                ):
                    cpp.includes([bad])

    def test_namespace_open_close(self) -> None:
        self.assertEqual(cpp.namespace_open("pw::foo"), "namespace pw::foo {")
        self.assertEqual(cpp.namespace_open("::pw::foo"), "namespace pw::foo {")
        self.assertEqual(
            cpp.namespace_close("::pw::foo"), "}  // namespace pw::foo"
        )
        for empty in ("", "::"):
            with self.assertRaisesRegex(ValueError, "cannot be empty"):
                cpp.namespace_open(empty)

    def test_namespace_context_manager(self) -> None:
        out = OutputFile("test.h")
        out.write_line("#pragma once")
        with cpp.namespace(out, "::pw::foo"):
            out.write_line("struct Bar {};")
        self.assertEqual(
            out.content(),
            "#pragma once\n"
            "\n"
            "namespace pw::foo {\n"
            "\n"
            "struct Bar {};\n"
            "\n"
            "}  // namespace pw::foo\n",
        )

        compact = OutputFile("compact.h")
        with cpp.namespace(compact, "pw", padding=False):
            compact.write_line("int x;")
        self.assertEqual(
            compact.content(),
            "namespace pw {\nint x;\n}  // namespace pw\n",
        )

        global_ns = OutputFile("global.h")
        with cpp.namespace(global_ns, ""):
            global_ns.write_line("int y;")
        with cpp.namespace(global_ns, None):
            global_ns.write_line("int z;")
        self.assertEqual(global_ns.content(), "int y;\nint z;\n")


if __name__ == "__main__":
    unittest.main()
