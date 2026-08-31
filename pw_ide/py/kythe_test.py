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
"""Tests for pw_ide.kythe."""

from pathlib import Path
import tempfile
import unittest

from pw_ide.kythe import (
    DEFAULT_CORPUS,
    _find_required_headers,
    extract_single_command,
    find_compilation_databases,
)


class KytheExtractorTest(unittest.TestCase):
    """Tests for Kythe compilation unit extraction."""

    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.workspace = Path(self.temp_dir.name)

        # Create sample source and header
        self.src_file = self.workspace / "test.cc"
        self.header_file = self.workspace / "test.h"
        self.transitive_header_file = self.workspace / "transitive.h"

        self.transitive_header_file.write_text(
            "#pragma once\nint getTransitiveValue();\n"
        )
        self.header_file.write_text(
            '#pragma once\n#include "transitive.h"\nint getValue();\n'
        )
        self.src_file.write_text(
            '#include "test.h"\nint getValue() { return 42; }\n'
        )

    def tearDown(self):
        self.temp_dir.cleanup()

    def test_find_compilation_databases(self):
        compdb_path = self.workspace / "compile_commands.json"
        compdb_path.write_text("[]")
        found = find_compilation_databases(self.workspace)
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0], compdb_path)

    def test_find_required_headers_recursive(self):
        headers = _find_required_headers(self.src_file, [], self.workspace)
        self.assertIn(self.header_file.resolve(), headers)
        self.assertIn(self.transitive_header_file.resolve(), headers)

    def test_extract_single_command_missing_file(self):
        out_dir = self.workspace / "out"
        out_dir.mkdir()
        entry = {
            "directory": str(self.workspace),
            "command": "clang++ -c non_existent.cc",
            "file": "non_existent.cc",
        }
        res = extract_single_command(
            entry, 0, out_dir, self.workspace, corpus=DEFAULT_CORPUS
        )
        self.assertIsNone(res)


if __name__ == "__main__":
    unittest.main()
