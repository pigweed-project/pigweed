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
"""Tests for the Defect class in pw_fortifier."""

import json
import unittest

from pyfakefs.fake_filesystem_unittest import TestCaseMixin
from pw_fortifier.async_path import AsyncPath
from pw_fortifier.code_snippet import CodeSnippet
from pw_fortifier.defect import Defect, SOURCE_FILE_PATTERN
from pw_fortifier.issue import Issue


class TestDefect(unittest.IsolatedAsyncioTestCase, TestCaseMixin):
    """Unit tests for the Defect class."""

    def setUp(self) -> None:
        self.setUpPyfakefs()
        self.working_dir = AsyncPath('/working')
        self.fs.create_dir(self.working_dir.path)

    def test_defect_defaults(self) -> None:
        """Tests Defect default values."""
        defect = Defect(
            location=CodeSnippet(file='foo/bar.cc'),
            issue_id=None,
            title=None,
            description=None,
            cl_num=None,
        )
        self.assertEqual(defect.filename, 'foo/bar.cc')
        self.assertEqual(defect.location, CodeSnippet(file='foo/bar.cc'))
        self.assertEqual(defect.priority, 4)
        self.assertEqual(defect.severity, 4)
        self.assertIsNone(defect.assignee)
        self.assertEqual(defect.comments, [])
        self.assertEqual(defect.affected_files, [])

    async def test_save_and_load(self) -> None:
        """Tests saving a defect to JSON and loading it back."""
        defect = Defect(
            location=CodeSnippet(file='foo/bar.cc'),
            issue_id=123,
            title='Test Defect',
            description='A test defect description.',
            cl_num=789,
            priority=1,
            severity=2,
            assignee='test@google.com',
            affected_files=['file1.cc', 'file2.h'],
        )

        path = self.working_dir / 'defect.json'
        await defect.save(path)

        # Verify JSON content on disk
        self.assertTrue(await path.exists())
        content = await path.read_text()
        expected_dict = {
            'location': {'file': 'foo/bar.cc', 'lines': None},
            'issue_id': 123,
            'title': 'Test Defect',
            'description': 'A test defect description.',
            'comments': [],
            'cl_num': 789,
            'priority': 1,
            'severity': 2,
            'assignee': 'test@google.com',
            'affected_files': ['file1.cc', 'file2.h'],
        }
        self.assertEqual(json.loads(content), expected_dict)

        # Load and verify
        loaded_defect = await Defect.load(path)
        self.assertEqual(loaded_defect, defect)

    async def test_load_backward_compatibility(self) -> None:
        """Tests loading a defect from older JSON formats."""
        old_json_content = {
            'filename': 'foo/bar.cc',
            'issue_id': 123,
            'title': 'Old Defect',
            'description': 'Description',
            'cl_num': None,
        }
        path = self.working_dir / 'old_defect.json'
        await path.write_text(json.dumps(old_json_content))

        loaded_defect = await Defect.load(path)
        self.assertEqual(loaded_defect.filename, 'foo/bar.cc')
        self.assertEqual(loaded_defect.location, CodeSnippet(file='foo/bar.cc'))
        self.assertEqual(loaded_defect.issue_id, 123)
        self.assertEqual(loaded_defect.title, 'Old Defect')
        self.assertEqual(loaded_defect.description, 'Description')
        self.assertIsNone(loaded_defect.cl_num)
        self.assertEqual(loaded_defect.priority, 4)
        self.assertEqual(loaded_defect.severity, 4)
        self.assertIsNone(loaded_defect.assignee)
        self.assertEqual(loaded_defect.comments, [])
        self.assertEqual(loaded_defect.affected_files, [])

    def test_source_file_pattern(self) -> None:
        """Tests SOURCE_FILE_PATTERN matches C/C++ source and header paths."""

        sample_text = (
            'Found issues in pw_sync/mutex.cc and pw_sync/mutex.h, '
            'as well as third_party/foo.bar/baz.cpp and driver.c. '
            'Ignored: foo.py, README.md, mutex.cc_bak, bar.cppx.'
        )
        matches = SOURCE_FILE_PATTERN.findall(sample_text)
        self.assertEqual(
            matches,
            [
                'pw_sync/mutex.cc',
                'pw_sync/mutex.h',
                'third_party/foo.bar/baz.cpp',
                'driver.c',
            ],
        )

    def test_from_issue(self) -> None:
        """Tests Defect.from_issue parses tracker issue."""
        issue = Defect(
            issue_id=123,
            title='PW-SEC-001 - pw_sync/mutex.cc: Potential buffer overflow',
            description='Found defect in pw_sync/mutex.cc and pw_sync/mutex.h',
            priority=1,
            severity=2,
            assignee='user@google.com',
            cl_num=100,
        )
        converted = Defect.from_issue(issue)
        assert converted is not None
        self.assertEqual(converted.issue_id, 123)
        self.assertEqual(converted.filename, 'pw_sync/mutex.cc')
        self.assertEqual(
            converted.affected_files,
            ['pw_sync/mutex.cc', 'pw_sync/mutex.h'],
        )

    def test_from_issue_invalid_issue_id(self) -> None:
        """Tests Defect.from_issue logs warning when issue_id is invalid."""
        for invalid_id in [None, 0]:
            with self.subTest(issue_id=invalid_id):
                issue = Issue(
                    issue_id=invalid_id,
                    title='PW-SEC-001 - pw_sync/mutex.cc: Buffer overflow',
                )
                with self.assertLogs(
                    'pw_fortifier.defect', level='WARNING'
                ) as cm:
                    self.assertIsNone(Defect.from_issue(issue))
                self.assertTrue(
                    any(
                        'missing or invalid issue_id' in log
                        for log in cm.output
                    )
                )


if __name__ == '__main__':
    unittest.main()
