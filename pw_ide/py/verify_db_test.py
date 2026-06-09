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
"""Tests for pw_ide.verify_db"""

import unittest
from unittest import mock
from pathlib import Path

from pw_ide import verify_db

_TEST_DB_EMPTY: list[dict] = []
_TEST_DB_ONE_ENTRY = [
    {
        "file": "a.cc",
        "directory": "/home/user/pigweed",
        "arguments": ["clang", "-c", "a.cc"],
        "output": "a.o",
    }
]
_TEST_DB_UNKNOWN_FIELD = [
    {
        "file": "a.cc",
        "unknown": "field",
        "directory": "/home/user/pigweed",
        "arguments": ["clang", "-c", "a.cc"],
    }
]
_TEST_DB_MISSING_REQUIRED_FIELD = [
    {"directory": "/home/user/pigweed", "arguments": ["clang", "-c", "a.cc"]}
]
_TEST_DB_MISSING_OPTIONAL_FIELD = [
    {
        "file": "a.cc",
        "directory": "/home/user/pigweed",
        "arguments": ["clang", "-c", "a.cc"],
        # "output" is optional
    }
]
_TEST_DB_COMMAND_ENTRY = [
    {
        "file": "a.cc",
        "directory": "/home/user/pigweed",
        "command": "clang -c a.cc",
    }
]
_TEST_DB_COMMAND_AND_ARGUMENTS = [
    {
        "file": "a.cc",
        "directory": "/home/user/pigweed",
        "command": "clang -c a.cc",
        "arguments": ["clang", "-c", "a.cc"],
    }
]
_TEST_DB_DUPLICATE_ENTRY = [
    {
        "file": "a.cc",
        "directory": "/home/user/pigweed",
        "arguments": ["clang", "-c", "a.cc"],
    },
    {
        "file": "a.cc",
        "directory": "/home/user/pigweed",
        "arguments": ["clang", "-c", "a.cc"],
    },
]
_TEST_DB_UNNECESSARY_ENTRY = [
    {
        "file": "a.cc",
        "directory": "/home/user/pigweed",
        "arguments": ["clang", "-c", "a.cc"],
    },
    {
        "file": "b.cc",
        "directory": "/home/user/pigweed",
        "arguments": ["clang", "-c", "b.cc"],
    },
]
_TEST_DB_VIRTUAL_INCLUDE = [
    {
        "file": "a.cc",
        "directory": "/home/user/pigweed",
        "arguments": [
            "clang",
            "-c",
            "a.cc",
            "-Ibazel-out/_virtual_includes/foo",
        ],
    }
]
_TEST_DB_UNEXPECTED_SOURCE_FILE = [
    {
        "file": "a.cc",
        "directory": "/home/user/pigweed",
        "arguments": ["clang", "a.cc", "b.cc", "-o", "out.exe"],
    }
]
_TEST_DB_HEADER_ENTRY = [
    {
        "file": "a.h",
        "directory": "/home/user/pigweed",
        "arguments": ["clang", "a.cc"],
    },
    {
        "file": "a.cc",
        "directory": "/home/user/pigweed",
        "arguments": ["clang", "a.cc"],
    },
]


class VerifyDbTest(unittest.TestCase):
    """Tests for pw_ide.verify_db."""

    # pylint: disable=too-many-public-methods

    def test_format_check_empty_db(self):
        # An empty database is valid.
        db = _TEST_DB_EMPTY
        self.assertTrue(verify_db.format_check(db, strict=False))

    def test_format_check_valid_strict(self):
        # A single entry database is valid in strict mode.
        db = _TEST_DB_ONE_ENTRY
        self.assertTrue(verify_db.format_check(db, strict=True))

    def test_format_check_valid_lax(self):
        # A single entry database is valid in lax mode.
        db = _TEST_DB_ONE_ENTRY
        self.assertTrue(verify_db.format_check(db, strict=False))

    def test_format_check_unknown_field_strict(self):
        # An unknown field is invalid in strict mode.
        db = _TEST_DB_UNKNOWN_FIELD
        self.assertFalse(verify_db.format_check(db, strict=True))

    def test_format_check_unknown_field_lax(self):
        # An unknown field is valid in lax mode.
        db = _TEST_DB_UNKNOWN_FIELD
        self.assertTrue(verify_db.format_check(db, strict=False))

    def test_format_check_missing_required_field_strict(self):
        # A missing required field is invalid in strict mode.
        db = _TEST_DB_MISSING_REQUIRED_FIELD
        self.assertFalse(verify_db.format_check(db, strict=True))

    def test_format_check_missing_required_field_lax(self):
        # A missing required field is valid in lax mode.
        db = _TEST_DB_MISSING_REQUIRED_FIELD
        self.assertFalse(verify_db.format_check(db, strict=False))

    def test_format_check_missing_optional_field_strict(self):
        # A missing optional field is valid in strict mode.
        db = _TEST_DB_MISSING_OPTIONAL_FIELD
        self.assertTrue(verify_db.format_check(db, strict=True))

    def test_format_check_missing_optional_field_lax(self):
        # A missing optional field is valid in lax mode.
        db = _TEST_DB_MISSING_OPTIONAL_FIELD
        self.assertTrue(verify_db.format_check(db, strict=False))

    def test_format_check_command_entry_strict(self):
        # A command entry is valid in strict mode.
        db = _TEST_DB_COMMAND_ENTRY
        self.assertTrue(verify_db.format_check(db, strict=True))

    def test_format_check_command_entry_lax(self):
        # A command entry is valid in lax mode.
        db = _TEST_DB_COMMAND_ENTRY
        self.assertTrue(verify_db.format_check(db, strict=False))

    def test_format_check_command_and_arguments_strict(self):
        # A command and arguments entry is invalid in strict mode.
        db = _TEST_DB_COMMAND_AND_ARGUMENTS
        self.assertFalse(verify_db.format_check(db, strict=True))

    def test_format_check_command_and_arguments_lax(self):
        # A command and arguments entry is invalid in lax mode.
        db = _TEST_DB_COMMAND_AND_ARGUMENTS
        self.assertFalse(verify_db.format_check(db, strict=False))

    def test_missing_check_no_files(self):
        # No files are missing.
        target_files = []
        db_files = []
        self.assertTrue(verify_db.missing_check(target_files, db_files))

    def test_missing_check_one_file_missing(self):
        # One file is missing.
        target_files = [Path('a.cc')]
        db_files = []
        self.assertFalse(verify_db.missing_check(target_files, db_files))

    def test_missing_check_one_file_present(self):
        # One file is present.
        target_files = [Path('a.cc')]
        db_files = [Path('a.cc')]
        self.assertTrue(verify_db.missing_check(target_files, db_files))

    def test_missing_check_multiple_files_missing(self):
        # Multiple files are missing.
        target_files = [Path('a.cc'), Path('b.cc')]
        db_files = []
        self.assertFalse(verify_db.missing_check(target_files, db_files))

    def test_missing_check_multiple_files_present(self):
        # Multiple files are present.
        target_files = [Path('a.cc'), Path('b.cc')]
        db_files = [Path('a.cc'), Path('b.cc')]
        self.assertTrue(verify_db.missing_check(target_files, db_files))

    def test_missing_check_multiple_files_mixed(self):
        # Multiple files, some present, some missing.
        target_files = [Path('a.cc'), Path('b.cc')]
        db_files = [Path('a.cc')]
        self.assertFalse(verify_db.missing_check(target_files, db_files))

    def test_missing_check_no_continue(self):
        # Multiple files, some present, some missing, but stop on first error.
        target_files = [Path('a.cc'), Path('b.cc'), Path('c.cc')]
        db_files = [Path('a.cc')]
        self.assertFalse(
            verify_db.missing_check(
                target_files, db_files, should_continue=False
            )
        )

    def test_missing_check_continue(self):
        # Multiple files, some present, some missing, but continue on error.
        target_files = [Path('a.cc'), Path('b.cc'), Path('c.cc')]
        db_files = [Path('a.cc')]
        self.assertFalse(
            verify_db.missing_check(
                target_files, db_files, should_continue=True
            )
        )

    def test_duplicate_check_no_duplicates(self):
        # No duplicates.
        db_files = [Path('a.cc'), Path('b.cc')]
        self.assertTrue(verify_db.duplicate_check(db_files))

    def test_duplicate_check_one_duplicate(self):
        # One duplicate.
        db_files = [Path('a.cc'), Path('a.cc')]
        self.assertFalse(verify_db.duplicate_check(db_files))

    def test_duplicate_check_multiple_duplicates(self):
        # Multiple duplicates.
        db_files = [Path('a.cc'), Path('a.cc'), Path('b.cc'), Path('b.cc')]
        self.assertFalse(verify_db.duplicate_check(db_files))

    def test_duplicate_check_no_continue(self):
        # Multiple duplicates, but stop on first error.
        db_files = [Path('a.cc'), Path('a.cc'), Path('b.cc'), Path('b.cc')]
        self.assertFalse(
            verify_db.duplicate_check(db_files, should_continue=False)
        )

    def test_duplicate_check_continue(self):
        # Multiple duplicates, but continue on error.
        db_files = [Path('a.cc'), Path('a.cc'), Path('b.cc'), Path('b.cc')]
        self.assertFalse(
            verify_db.duplicate_check(db_files, should_continue=True)
        )

    def test_duplicate_check_header_entries(self):
        # Find duplicate header entries
        db_files = [Path('a.h'), Path('a.h')]
        self.assertFalse(verify_db.duplicate_check(db_files))

    def test_duplicate_check_header_entries_ignore(self):
        # Ignore duplicate header entries
        db_files = [Path('a.h'), Path('a.h')]
        self.assertTrue(
            verify_db.duplicate_check(db_files, ignore_header_entries=True)
        )

    def test_unnecessary_check_no_unnecessary(self):
        # No unnecessary files.
        target_files = [Path('a.cc'), Path('b.cc')]
        db_files = [Path('a.cc'), Path('b.cc')]
        self.assertTrue(verify_db.unnecessary_check(target_files, db_files))

    def test_unnecessary_check_one_unnecessary(self):
        # One unnecessary file.
        target_files = [Path('a.cc')]
        db_files = [Path('a.cc'), Path('b.cc')]
        self.assertFalse(verify_db.unnecessary_check(target_files, db_files))

    def test_unnecessary_check_multiple_unnecessary(self):
        # Multiple unnecessary files.
        target_files = [Path('a.cc')]
        db_files = [Path('a.cc'), Path('b.cc'), Path('c.cc')]
        self.assertFalse(verify_db.unnecessary_check(target_files, db_files))

    def test_unnecessary_check_no_continue(self):
        # Multiple unnecessary files, but stop on first error.
        target_files = [Path('a.cc')]
        db_files = [Path('a.cc'), Path('b.cc'), Path('c.cc')]
        self.assertFalse(
            verify_db.unnecessary_check(
                target_files, db_files, should_continue=False
            )
        )

    def test_unnecessary_check_continue(self):
        # Multiple unnecessary files, but continue on error.
        target_files = [Path('a.cc')]
        db_files = [Path('a.cc'), Path('b.cc'), Path('c.cc')]
        self.assertFalse(
            verify_db.unnecessary_check(
                target_files, db_files, should_continue=True
            )
        )

    def test_unnecessary_check_ignore_headers_true(self):
        # Header files are ignored.
        target_files = [Path('a.cc')]
        db_files = [Path('a.cc'), Path('b.h')]
        self.assertTrue(
            verify_db.unnecessary_check(
                target_files, db_files, ignore_header_entries=True
            )
        )

    def test_unnecessary_check_ignore_headers_false(self):
        # Header files are not ignored.
        target_files = [Path('a.cc')]
        db_files = [Path('a.cc'), Path('b.h')]
        self.assertFalse(
            verify_db.unnecessary_check(
                target_files, db_files, ignore_header_entries=False
            )
        )

    def test_virtual_include_check_no_virtual_includes(self):
        # No virtual includes.
        db = _TEST_DB_ONE_ENTRY
        self.assertTrue(
            verify_db.virtual_include_check(db, should_continue=True)
        )

    @mock.patch('pw_ide.verify_db.Path.exists', return_value=True)
    def test_virtual_include_check_one_virtual_include(self, _mock_exists):
        # One virtual include.
        db = _TEST_DB_VIRTUAL_INCLUDE
        self.assertFalse(
            verify_db.virtual_include_check(db, should_continue=True)
        )

    def test_unexpected_source_file_check_no_unexpected_source_files(self):
        # No unexpected source files.
        db = _TEST_DB_ONE_ENTRY
        self.assertTrue(
            verify_db.unexpected_source_files_check(db, should_continue=True)
        )

    def test_unexpected_source_file_check_one_unexpected_source_file(self):
        # One unexpected source file.
        db = _TEST_DB_UNEXPECTED_SOURCE_FILE
        self.assertFalse(
            verify_db.unexpected_source_files_check(db, should_continue=True)
        )

    def test_unexpected_source_file_check_ignore_headers_true(self):
        # Header file entries are ignored.
        db = _TEST_DB_HEADER_ENTRY
        self.assertTrue(
            verify_db.unexpected_source_files_check(
                db, ignore_header_entries=True
            )
        )

    def test_unexpected_source_file_check_ignore_headers_false(self):
        # Header file entries are not ignored.
        # Header entries will always trigger an unexpected source file in the
        # arguments because the "file" does not match the file being compiled.
        db = _TEST_DB_HEADER_ENTRY
        self.assertFalse(
            verify_db.unexpected_source_files_check(
                db, ignore_header_entries=False
            )
        )

    def test_verify_db_default(self):
        # Default options (all checks enabled, no target files).
        # Non-empty DB fails unnecessary check.
        self.assertFalse(verify_db.verify_db(_TEST_DB_ONE_ENTRY))
        # Empty DB succeeds all checks.
        self.assertTrue(verify_db.verify_db(_TEST_DB_EMPTY))

    def test_verify_db_no_checks(self):
        # No checks performed.
        target_files = [Path('a.cc')]
        db = _TEST_DB_ONE_ENTRY
        options = verify_db.Options()
        options.format_check(False)
        options.missing_check(False)
        options.duplicate_check(False)
        options.unnecessary_check(False)
        options.virtual_include_check(False)
        options.unexpected_source_file_check(False)
        self.assertTrue(verify_db.verify_db(db, target_files, options))

    def test_verify_db_format_check_only_valid_strict(self):
        # Format check only, valid database, strict mode.
        target_files = [Path('a.cc')]
        db = _TEST_DB_ONE_ENTRY
        options = verify_db.Options()
        options.format_check(True, strict=True)
        options.missing_check(False)
        options.duplicate_check(False)
        options.unnecessary_check(False)
        options.virtual_include_check(False)
        options.unexpected_source_file_check(False)
        self.assertTrue(verify_db.verify_db(db, target_files, options))

    def test_verify_db_format_check_only_invalid_strict(self):
        # Format check only, invalid database (unknown field), strict mode.
        target_files = [Path('a.cc')]
        db = _TEST_DB_UNKNOWN_FIELD
        options = verify_db.Options()
        options.format_check(True, strict=True)
        options.missing_check(False)
        options.duplicate_check(False)
        options.unnecessary_check(False)
        options.virtual_include_check(False)
        options.unexpected_source_file_check(False)
        self.assertFalse(verify_db.verify_db(db, target_files, options))

    def test_verify_db_missing_check_only_valid(self):
        # Missing check only, valid database.
        target_files = [Path('a.cc')]
        db = _TEST_DB_ONE_ENTRY
        options = verify_db.Options()
        options.format_check(False)
        options.missing_check(True)
        options.duplicate_check(False)
        options.unnecessary_check(False)
        options.virtual_include_check(False)
        options.unexpected_source_file_check(False)
        self.assertTrue(verify_db.verify_db(db, target_files, options))

    def test_verify_db_missing_check_only_invalid(self):
        # Missing check only, invalid database (missing entry).
        target_files = [Path('a.cc'), Path('b.cc')]
        db = _TEST_DB_ONE_ENTRY
        options = verify_db.Options()
        options.format_check(False)
        options.missing_check(True)
        options.duplicate_check(False)
        options.unnecessary_check(False)
        options.virtual_include_check(False)
        options.unexpected_source_file_check(False)
        self.assertFalse(verify_db.verify_db(db, target_files, options))

    def test_verify_db_duplicate_check_only_valid(self):
        # Duplicate check only, valid database.
        target_files = [Path('a.cc')]
        db = _TEST_DB_ONE_ENTRY
        options = verify_db.Options()
        options.format_check(False)
        options.missing_check(False)
        options.duplicate_check(True)
        options.unnecessary_check(False)
        options.virtual_include_check(False)
        options.unexpected_source_file_check(False)
        self.assertTrue(verify_db.verify_db(db, target_files, options))

    def test_verify_db_duplicate_check_only_invalid(self):
        # Duplicate check only, invalid database (duplicate entry).
        target_files = [Path('a.cc')]
        db = _TEST_DB_DUPLICATE_ENTRY
        options = verify_db.Options()
        options.format_check(False)
        options.missing_check(False)
        options.duplicate_check(True)
        options.unnecessary_check(False)
        options.virtual_include_check(False)
        options.unexpected_source_file_check(False)
        self.assertFalse(verify_db.verify_db(db, target_files, options))

    def test_verify_db_unnecessary_check_only_valid(self):
        # Unnecessary check only, valid database.
        target_files = [Path('a.cc')]
        db = _TEST_DB_ONE_ENTRY
        options = verify_db.Options()
        options.format_check(False)
        options.missing_check(False)
        options.duplicate_check(False)
        options.unnecessary_check(True)
        options.virtual_include_check(False)
        options.unexpected_source_file_check(False)
        self.assertTrue(verify_db.verify_db(db, target_files, options))

    def test_verify_db_unnecessary_check_only_invalid(self):
        # Unnecessary check only, invalid database (unnecessary entry).
        target_files = [Path('a.cc')]
        db = _TEST_DB_UNNECESSARY_ENTRY
        options = verify_db.Options()
        options.format_check(False)
        options.missing_check(False)
        options.duplicate_check(False)
        options.unnecessary_check(True)
        options.virtual_include_check(False)
        options.unexpected_source_file_check(False)
        self.assertFalse(verify_db.verify_db(db, target_files, options))

    def test_verify_db_virtual_include_check_only_valid(self):
        # Virtual include check only, valid database.
        target_files = [Path('a.cc')]
        db = _TEST_DB_ONE_ENTRY
        options = verify_db.Options()
        options.format_check(False)
        options.missing_check(False)
        options.duplicate_check(False)
        options.unnecessary_check(False)
        options.virtual_include_check(True)
        options.unexpected_source_file_check(False)
        self.assertTrue(verify_db.verify_db(db, target_files, options))

    @mock.patch('pw_ide.verify_db.Path.exists', return_value=True)
    def test_verify_db_virtual_include_check_only_invalid(self, _mock_exists):
        # Virtual include check only, invalid database (virtual include).
        target_files = [Path('a.cc')]
        db = _TEST_DB_VIRTUAL_INCLUDE
        options = verify_db.Options()
        options.format_check(False)
        options.missing_check(False)
        options.duplicate_check(False)
        options.unnecessary_check(False)
        options.virtual_include_check(True)
        options.unexpected_source_file_check(False)
        self.assertFalse(verify_db.verify_db(db, target_files, options))

    def test_verify_db_unexpected_source_file_check_only_valid(self):
        # Unexpected source file check only, valid database.
        target_files = [Path('a.cc')]
        db = _TEST_DB_ONE_ENTRY
        options = verify_db.Options()
        options.format_check(False)
        options.missing_check(False)
        options.duplicate_check(False)
        options.unnecessary_check(False)
        options.virtual_include_check(False)
        options.unexpected_source_file_check(True)
        self.assertTrue(verify_db.verify_db(db, target_files, options))

    def test_verify_db_unexpected_source_file_check_only_invalid(self):
        # Unexpected source file check only, invalid database (unexpected
        # source file).
        target_files = [Path('a.cc')]
        db = _TEST_DB_UNEXPECTED_SOURCE_FILE
        options = verify_db.Options()
        options.format_check(False)
        options.missing_check(False)
        options.duplicate_check(False)
        options.unnecessary_check(False)
        options.virtual_include_check(False)
        options.unexpected_source_file_check(True)
        self.assertFalse(verify_db.verify_db(db, target_files, options))

    def test_verify_db_all_checks_valid(self):
        # All checks performed, valid database.
        target_files = [Path('a.cc')]
        db = _TEST_DB_ONE_ENTRY
        options = verify_db.Options()
        options.should_continue = True
        self.assertTrue(verify_db.verify_db(db, target_files, options))


if __name__ == '__main__':
    unittest.main()
