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
"""Unit tests for generate_printf_cases."""

from pathlib import Path
import tempfile
import unittest

import generate_printf_cases
from generate_printf_cases import NormalizedArg, NormalizedTestCase


class GeneratePrintfCasesTest(unittest.TestCase):
    """Tests C++/Rust argument formatting and YAML test case loading."""

    def test_rust_arg_formatting(self) -> None:
        """Verifies Rust literal formatting for all argument kinds."""
        self.assertEqual(
            generate_printf_cases.rust_arg(NormalizedArg("signed_int", -42)),
            "TestArg::SignedInt(-42i64)",
        )
        self.assertEqual(
            generate_printf_cases.rust_arg(
                NormalizedArg("unsigned_int", 2**64 - 1)
            ),
            "TestArg::UnsignedInt(18446744073709551615u64)",
        )
        self.assertEqual(
            generate_printf_cases.rust_arg(
                NormalizedArg("floating_point", float("inf"))
            ),
            "TestArg::FloatingPoint(f64::INFINITY)",
        )
        self.assertEqual(
            generate_printf_cases.rust_arg(
                NormalizedArg("string_value", "hi\0")
            ),
            'TestArg::StringValue("hi\\0")',
        )
        self.assertEqual(
            generate_printf_cases.rust_arg(NormalizedArg("char_value", "é")),
            "TestArg::CharValue('é')",
        )
        self.assertEqual(
            generate_printf_cases.rust_arg(
                NormalizedArg("pointer_value", 0xABCD)
            ),
            "TestArg::PointerValue(43981u64)",
        )

    def test_rust_arg_rejects_invalid_values(self) -> None:
        """Verifies rust_arg rejects unsupported kinds and invalid values."""
        invalid_cases = [
            NormalizedArg("unknown_kind", 1),
            NormalizedArg("signed_int", True),
            NormalizedArg("unsigned_int", -1),
            NormalizedArg("floating_point", "3.14"),
            NormalizedArg("string_value", None),
            NormalizedArg("char_value", "ab"),
            NormalizedArg("char_value", None),
            NormalizedArg("pointer_value", -1),
        ]
        for arg in invalid_cases:
            with self.subTest(arg=arg):
                with self.assertRaises(ValueError):
                    generate_printf_cases.rust_arg(arg)

    def test_load_test_cases_and_cc_snprintf_synthesis(self) -> None:
        """Verifies YAML normalization and C++ snprintf argument synthesis."""
        with tempfile.TemporaryDirectory() as tmpdir:
            yaml_path = Path(tmpdir) / "cases.yaml"
            yaml_path.write_text(
                """
cases:
  - test_id: "sample"
    format_string: "%hhd:%s:%p:%.2f"
    args:
      - signed_int: 300
      - string_value: "ok"
      - pointer_value: 4660
      - floating_point: -0.0
    expected_output: "44:ok:0x1234:-0.00"
  - test_id: "empty_fmt"
    format_string: ""
    args: null
    expected_output: ""
  - test_id: "redundant_flags"
    format_string: "%0-8x"
    args:
      - unsigned_int: 42
    expected_output: "2a      "
  - test_id: "plus_space_flags"
    format_string: "%+ 8d"
    args:
      - signed_int: 42
    expected_output: "     +42"
  - test_id: "skipped_unicode_char"
    format_string: "%c"
    args:
      - char_value: "é"
    expected_output: "é"
    skip_snprintf: "C '%c' formats single-byte unsigned char"
""",
                encoding="utf-8",
            )
            loaded = generate_printf_cases.load_test_cases(yaml_path)
            self.assertEqual(
                loaded,
                [
                    NormalizedTestCase(
                        test_id="sample",
                        format_string="%hhd:%s:%p:%.2f",
                        args=[
                            NormalizedArg("signed_int", 300),
                            NormalizedArg("string_value", "ok"),
                            NormalizedArg("pointer_value", 4660),
                            NormalizedArg("floating_point", -0.0),
                        ],
                        expected_output="44:ok:0x1234:-0.00",
                        expected_output_utf8_len=18,
                        skip_snprintf=None,
                        cc_snprintf_args=(
                            "static_cast<int>(300)",
                            '"ok"',
                            "reinterpret_cast<const void*>("
                            "static_cast<std::uintptr_t>(4660))",
                            "-0.0",
                        ),
                        has_pointer_spec=True,
                        suppress_wformat=False,
                        suppress_wformat_zero_length=False,
                    ),
                    NormalizedTestCase(
                        test_id="empty_fmt",
                        format_string="",
                        args=[],
                        expected_output="",
                        expected_output_utf8_len=0,
                        skip_snprintf=None,
                        cc_snprintf_args=(),
                        has_pointer_spec=False,
                        suppress_wformat=False,
                        suppress_wformat_zero_length=True,
                    ),
                    NormalizedTestCase(
                        test_id="redundant_flags",
                        format_string="%0-8x",
                        args=[NormalizedArg("unsigned_int", 42)],
                        expected_output="2a      ",
                        expected_output_utf8_len=8,
                        skip_snprintf=None,
                        cc_snprintf_args=("static_cast<unsigned int>(42)",),
                        has_pointer_spec=False,
                        suppress_wformat=True,
                        suppress_wformat_zero_length=False,
                    ),
                    NormalizedTestCase(
                        test_id="plus_space_flags",
                        format_string="%+ 8d",
                        args=[NormalizedArg("signed_int", 42)],
                        expected_output="     +42",
                        expected_output_utf8_len=8,
                        skip_snprintf=None,
                        cc_snprintf_args=("static_cast<int>(42)",),
                        has_pointer_spec=False,
                        suppress_wformat=True,
                        suppress_wformat_zero_length=False,
                    ),
                    NormalizedTestCase(
                        test_id="skipped_unicode_char",
                        format_string="%c",
                        args=[NormalizedArg("char_value", "é")],
                        expected_output="é",
                        expected_output_utf8_len=2,
                        skip_snprintf=(
                            "C '%c' formats single-byte unsigned char"
                        ),
                    ),
                ],
            )

    def test_load_test_cases_rejects_invalid_yaml_and_snprintf_ub(self) -> None:
        """Verifies schema and C snprintf validation reject malformed cases."""
        with tempfile.TemporaryDirectory() as tmpdir:
            yaml_path = Path(tmpdir) / "bad.yaml"

            invalid_docs = [
                "",
                "null",
                "cases:\n",
                "- item",
                "unknown_top: 1\ncases: []",
                "cases: 123",
                "cases:\n  - not_a_mapping",
                (
                    "cases:\n  - test_id: x\n    extra: 1\n"
                    "    format_string: a\n    expected_output: a"
                ),
                "cases:\n  - format_string: a\n    expected_output: a",
                (
                    "cases:\n  - test_id: ''\n"
                    "    format_string: a\n    expected_output: a"
                ),
                (
                    "cases:\n  - test_id: UpperCase\n"
                    "    format_string: a\n    expected_output: a"
                ),
                (
                    "cases:\n  - test_id: fn\n"
                    "    format_string: a\n    expected_output: a"
                ),
                (
                    "cases:\n  - test_id: abstract\n"
                    "    format_string: a\n    expected_output: a"
                ),
                "cases:\n  - test_id: x\n    expected_output: a",
                "cases:\n  - test_id: x\n    format_string: a",
                (
                    "cases:\n  - test_id: x\n"
                    "    format_string: 123\n    expected_output: a"
                ),
                (
                    "cases:\n  - test_id: x\n"
                    "    format_string: a\n    expected_output: null"
                ),
                (
                    "cases:\n  - test_id: x\n"
                    "    format_string: a\n    args: bad\n"
                    "    expected_output: a"
                ),
                (
                    "cases:\n  - test_id: x\n"
                    "    format_string: a\n"
                    "    args:\n      - a: 1\n        b: 2\n"
                    "    expected_output: a"
                ),
                (
                    "cases:\n  - test_id: x\n"
                    "    format_string: a\n    args:\n      - 1: 2\n"
                    "    expected_output: a"
                ),
                # Bare bool or blank string for skip_snprintf is rejected.
                (
                    "cases:\n  - test_id: x\n"
                    "    format_string: a\n    expected_output: a\n"
                    "    skip_snprintf: true"
                ),
                (
                    "cases:\n  - test_id: x\n"
                    "    format_string: a\n    expected_output: a\n"
                    "    skip_snprintf: '   '"
                ),
                (
                    "cases:\n  - test_id: x\n"
                    "    format_string: a\n    expected_output: a\n"
                    "    expect_parse_error: true"
                ),
                (
                    "cases:\n  - test_id: dup\n"
                    "    format_string: a\n    expected_output: a\n"
                    "  - test_id: dup\n"
                    "    format_string: b\n    expected_output: b"
                ),
                # Multi-byte char_value without skip_snprintf raises ValueError.
                (
                    "cases:\n  - test_id: unicode_char\n"
                    "    format_string: '%c'\n"
                    "    args:\n      - char_value: 'é'\n"
                    "    expected_output: 'é'"
                ),
                # C snprintf UB / sign / width mismatches without skip_snprintf.
                (
                    "cases:\n  - test_id: ub_hash_s\n"
                    "    format_string: '%#s'\n"
                    "    args:\n      - string_value: 'hi'\n"
                    "    expected_output: 'hi'"
                ),
                (
                    "cases:\n  - test_id: ub_char_prec\n"
                    "    format_string: '%.3c'\n"
                    "    args:\n      - char_value: 'A'\n"
                    "    expected_output: 'A'"
                ),
                (
                    "cases:\n  - test_id: sign_mismatch\n"
                    "    format_string: '%u'\n"
                    "    args:\n      - signed_int: -1\n"
                    "    expected_output: '4294967295'"
                ),
                (
                    "cases:\n  - test_id: missing_arg\n"
                    "    format_string: '%d'\n"
                    "    expected_output: '%d'"
                ),
                (
                    "cases:\n  - test_id: extra_arg\n"
                    "    format_string: 'plain'\n"
                    "    args:\n      - signed_int: 1\n"
                    "    expected_output: 'plain'"
                ),
            ]
            for bad_yaml in invalid_docs:
                with self.subTest(bad_yaml=bad_yaml):
                    yaml_path.write_text(bad_yaml, encoding="utf-8")
                    with self.assertRaises(ValueError):
                        generate_printf_cases.load_test_cases(yaml_path)


if __name__ == "__main__":
    unittest.main()
