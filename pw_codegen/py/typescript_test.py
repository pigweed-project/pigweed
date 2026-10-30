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
"""Tests for pw_codegen.typescript."""

import unittest

from pw_codegen import typescript


class TypeScriptCodegenTest(unittest.TestCase):
    """Tests TypeScript literal helpers."""

    def test_byte_array(self) -> None:
        self.assertEqual(typescript.byte_array(b""), "new Uint8Array([])")
        self.assertEqual(
            typescript.byte_array(b"\x00\x0a\xff"),
            "new Uint8Array([0x00, 0x0a, 0xff])",
        )


if __name__ == "__main__":
    unittest.main()
