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
"""Tests for pw_codegen.rust."""

import unittest

from pw_codegen import rust


class RustCodegenTest(unittest.TestCase):
    """Tests Rust literal formatting."""

    def test_is_identifier(self) -> None:
        """Verifies Rust identifier and keyword validation."""
        self.assertTrue(rust.is_identifier("my_var"))
        self.assertTrue(rust.is_identifier("_leading"))
        self.assertTrue(rust.is_identifier("MyStruct2"))
        self.assertFalse(rust.is_identifier(""))
        self.assertFalse(rust.is_identifier("_"))
        self.assertFalse(rust.is_identifier("2bad"))
        self.assertFalse(rust.is_identifier("bad-name"))
        self.assertFalse(rust.is_identifier("fn"))
        self.assertFalse(rust.is_identifier("abstract"))
        self.assertFalse(rust.is_identifier("Self"))
        self.assertFalse(rust.is_identifier("gen"))
        self.assertFalse(rust.is_identifier("try"))

    def test_bool_literal(self) -> None:
        """Verifies Rust boolean literal formatting."""
        self.assertEqual(rust.bool_literal(True), "true")
        self.assertEqual(rust.bool_literal(False), "false")

    def test_int_types_metadata(self) -> None:
        self.assertEqual(
            set(rust.INT_TYPES),
            {
                "i8",
                "i16",
                "i32",
                "i64",
                "i128",
                "u8",
                "u16",
                "u32",
                "u64",
                "u128",
            },
        )
        self.assertEqual(rust.INT_TYPES["i8"], rust.IntType("i8", -128, 127))
        self.assertEqual(rust.INT_TYPES["u8"], rust.IntType("u8", 0, 255))

    def test_int_literal_helpers(self) -> None:
        """Verifies explicit Rust integer literal helpers and bounds checks."""
        self.assertEqual(rust.i8_literal(-128), "-128i8")
        self.assertEqual(rust.i16_literal(-32768), "-32768i16")
        self.assertEqual(rust.i32_literal(-42), "-42i32")
        self.assertEqual(
            rust.i64_literal(-(2**63)),
            "-9223372036854775808i64",
        )
        self.assertEqual(rust.i128_literal(-(2**127)), f"{-(2**127)}i128")
        self.assertEqual(rust.u8_literal(255), "255u8")
        self.assertEqual(rust.u16_literal(65535), "65535u16")
        self.assertEqual(rust.u32_literal(0), "0u32")
        self.assertEqual(
            rust.u64_literal(2**64 - 1),
            "18446744073709551615u64",
        )
        self.assertEqual(
            rust.u128_literal(2**128 - 1),
            f"{2**128 - 1}u128",
        )
        self.assertEqual(rust.int_literal(-42, "i64"), "-42i64")
        with self.assertRaises(KeyError):
            rust.int_literal(0, "int64")
        with self.assertRaisesRegex(ValueError, "out of range"):
            rust.u8_literal(256)
        with self.assertRaisesRegex(ValueError, "out of range"):
            rust.u32_literal(-1)
        with self.assertRaisesRegex(ValueError, "out of range"):
            rust.i8_literal(128)

    def test_f32_and_f64_literals(self) -> None:
        """Verifies Rust f32/f64 literal formatting, non-finites, and bounds."""
        self.assertEqual(rust.f64_literal(3.14), "3.14f64")
        self.assertEqual(rust.f64_literal(-0.0), "-0.0f64")
        self.assertEqual(rust.f64_literal(-42), "-42.0f64")
        self.assertEqual(rust.f64_literal(float("nan")), "f64::NAN")
        self.assertEqual(rust.f64_literal(float("inf")), "f64::INFINITY")
        self.assertEqual(rust.f64_literal(float("-inf")), "f64::NEG_INFINITY")
        self.assertEqual(rust.f32_literal(0), "0.0f32")
        self.assertEqual(rust.f32_literal(2.5), "2.5f32")
        self.assertEqual(
            rust.f32_literal(3.4028234663852886e38),
            "3.4028234663852886e+38f32",
        )
        self.assertEqual(rust.f32_literal(float("nan")), "f32::NAN")
        self.assertEqual(rust.f32_literal(float("inf")), "f32::INFINITY")
        self.assertEqual(rust.f32_literal(float("-inf")), "f32::NEG_INFINITY")
        for overflow_f32 in (1e300, -1e300, 3.4028236e38, -3.4028236e38):
            with self.subTest(overflow_f32=overflow_f32):
                with self.assertRaises(ValueError):
                    rust.f32_literal(overflow_f32)
        with self.assertRaises(ValueError):
            rust.f64_literal(10**400)

    def test_string_literal(self) -> None:
        """Verifies Rust string literal escaping and surrogate rejection."""
        self.assertEqual(rust.string_literal(""), '""')
        self.assertEqual(rust.string_literal("hello"), '"hello"')
        self.assertEqual(rust.string_literal("it's"), '"it\'s"')
        self.assertEqual(
            rust.string_literal('a\\b"c\nd\re\t\0'),
            r'"a\\b\"c\nd\re\t\0"',
        )
        self.assertEqual(rust.string_literal("\x001"), r'"\01"')
        self.assertEqual(rust.string_literal("\x1b\x7f"), r'"\x1b\x7f"')
        self.assertEqual(rust.string_literal("\x80\x9f"), r'"\u{80}\u{9f}"')
        self.assertEqual(rust.string_literal("maté 🚀"), '"maté 🚀"')
        with self.assertRaises(ValueError):
            rust.string_literal("\ud800")

    def test_char_literal(self) -> None:
        """Verifies Rust char literal escaping and validation."""
        self.assertEqual(rust.char_literal("a"), "'a'")
        self.assertEqual(rust.char_literal('"'), "'\"'")
        self.assertEqual(rust.char_literal("é"), "'é'")
        self.assertEqual(rust.char_literal("🚀"), "'🚀'")
        self.assertEqual(rust.char_literal("'"), r"'\''")
        self.assertEqual(rust.char_literal("\\"), r"'\\'")
        self.assertEqual(rust.char_literal("\n"), r"'\n'")
        self.assertEqual(rust.char_literal("\r"), r"'\r'")
        self.assertEqual(rust.char_literal("\t"), r"'\t'")
        self.assertEqual(rust.char_literal("\0"), r"'\0'")
        self.assertEqual(rust.char_literal("\x07"), r"'\x07'")
        self.assertEqual(rust.char_literal("\x7f"), r"'\x7f'")
        self.assertEqual(rust.char_literal("\x80"), r"'\u{80}'")
        self.assertEqual(rust.char_literal("\x9f"), r"'\u{9f}'")
        self.assertEqual(rust.char_literal("\xa0"), "'\xa0'")
        self.assertEqual(rust.char_literal("\ud7ff"), "'\ud7ff'")
        self.assertEqual(rust.char_literal("\ue000"), "'\ue000'")
        for invalid in ("", "ab", "\ud800", "\udfff"):
            with self.subTest(invalid=invalid):
                with self.assertRaises(ValueError):
                    rust.char_literal(invalid)

    def test_byte_slice(self) -> None:
        """Verifies Rust byte slice formatting."""
        self.assertEqual(rust.byte_slice(b""), "&[]")
        self.assertEqual(rust.byte_slice(b"\x01\x02\xff"), "&[1, 2, 255]")
        self.assertEqual(rust.byte_slice(bytearray(b"\x03\x04")), "&[3, 4]")
        self.assertEqual(rust.byte_slice(memoryview(b"\x05\x06")), "&[5, 6]")


if __name__ == "__main__":
    unittest.main()
