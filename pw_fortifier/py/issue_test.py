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
"""Tests for the Issue class in pw_fortifier."""

import json
import unittest

from pyfakefs.fake_filesystem_unittest import TestCaseMixin
from pw_fortifier.async_path import AsyncPath
from pw_fortifier.code_snippet import CodeSnippet
from pw_fortifier.issue import Issue


class TestIssue(unittest.IsolatedAsyncioTestCase, TestCaseMixin):
    """Unit tests for the Issue class."""

    def setUp(self) -> None:
        self.setUpPyfakefs()
        self.working_dir = AsyncPath('/working')
        self.fs.create_dir(self.working_dir.path)

    def test_issue_defaults(self) -> None:
        """Tests Issue default values."""
        issue = Issue(
            issue_id=None,
            title=None,
            description=None,
        )
        self.assertEqual(issue.priority, 4)
        self.assertEqual(issue.severity, 4)
        self.assertIsNone(issue.assignee)
        self.assertEqual(issue.comments, [])
        self.assertIsNone(issue.location)

    async def test_save_and_load(self) -> None:
        """Tests saving an issue to JSON and loading it back."""
        issue = Issue(
            issue_id=123,
            title='Test Issue',
            description='A test issue description.',
            priority=1,
            severity=2,
            assignee='test@google.com',
        )

        path = self.working_dir / 'issue.json'
        await issue.save(path)

        # Verify JSON content on disk
        self.assertTrue(await path.exists())
        content = await path.read_text()
        expected_dict = {
            'issue_id': 123,
            'title': 'Test Issue',
            'description': 'A test issue description.',
            'comments': [],
            'priority': 1,
            'severity': 2,
            'assignee': 'test@google.com',
            'cl_num': None,
            'location': None,
        }
        self.assertEqual(json.loads(content), expected_dict)

        # Load and verify
        loaded_issue = await Issue.load(path)
        self.assertEqual(loaded_issue, issue)

    async def test_location_save_and_load(self) -> None:
        """Tests saving an issue with location to JSON and loading it back."""
        issue = Issue(
            issue_id=123,
            title='Test Issue',
            description='A test issue description.',
            location=CodeSnippet(file='foo/bar.cc', lines=(10, 25)),
        )

        path = self.working_dir / 'issue_loc.json'
        await issue.save(path)

        loaded_issue = await Issue.load(path)
        self.assertEqual(
            loaded_issue.location,
            CodeSnippet(file='foo/bar.cc', lines=(10, 25)),
        )
        self.assertEqual(loaded_issue, issue)

    def test_replace(self) -> None:
        """Tests replacing fields in an Issue."""
        issue = Issue(
            issue_id=123,
            title='Test Issue',
            description='A test issue description.',
        )
        new_issue = issue._replace(issue_id=999, title='Updated Title')
        self.assertEqual(new_issue.issue_id, 999)
        self.assertEqual(new_issue.title, 'Updated Title')
        self.assertEqual(new_issue.description, 'A test issue description.')


if __name__ == '__main__':
    unittest.main()
