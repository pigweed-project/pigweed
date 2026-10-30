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

    def test_string_literal(self) -> None:
        self.assertEqual(rust.string_literal(""), '""')
        self.assertEqual(rust.string_literal("hello"), '"hello"')
        self.assertEqual(
            rust.string_literal('a\\b"c\nd\re\t\0'),
            r'"a\\b\"c\nd\re\t\0"',
        )
        self.assertEqual(rust.string_literal("\x001"), r'"\01"')
        self.assertEqual(rust.string_literal("\x1b\x7f"), r'"\x1b\x7f"')

    def test_byte_slice(self) -> None:
        self.assertEqual(rust.byte_slice(b""), "&[]")
        self.assertEqual(rust.byte_slice(b"\x01\x02\xff"), "&[1, 2, 255]")


if __name__ == "__main__":
    unittest.main()
