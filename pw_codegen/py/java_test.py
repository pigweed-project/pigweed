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
"""Tests for pw_codegen.java."""

import unittest

from pw_codegen import java


class JavaCodegenTest(unittest.TestCase):
    """Tests Java literal formatting."""

    def test_string_literal(self) -> None:
        self.assertEqual(java.string_literal(""), '""')
        self.assertEqual(java.string_literal("hello"), '"hello"')
        self.assertEqual(
            java.string_literal('a\\b"c\nd\re\tf\bg\fh'),
            r'"a\\b\"c\nd\re\tf\bg\fh"',
        )
        self.assertEqual(
            java.string_literal("\x00\x1b\x7f"), r'"\u0000\u001b\u007f"'
        )

    def test_byte_array(self) -> None:
        self.assertEqual(java.byte_array(b""), "new byte[] {}")
        self.assertEqual(
            java.byte_array(b"\x00\x01\xff"),
            "new byte[] {(byte) 0x00, (byte) 0x01, (byte) 0xff}",
        )


if __name__ == "__main__":
    unittest.main()
