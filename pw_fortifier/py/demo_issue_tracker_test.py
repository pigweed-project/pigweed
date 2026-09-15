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
"""Tests for DemoIssueTracker."""

# pylint: disable=protected-access

from datetime import date
import io
import json
import unittest
from unittest.mock import patch

from pyfakefs.fake_filesystem_unittest import TestCaseMixin
from pw_fortifier.async_path import AsyncPath
from pw_fortifier.code_snippet import CodeSnippet
from pw_fortifier.demo_issue_tracker import DemoIssueTracker, main
from pw_fortifier.freshness_result import FreshnessResult, PackageVersion
from pw_fortifier.issue import Issue


class TestDemoIssueTracker(unittest.IsolatedAsyncioTestCase, TestCaseMixin):
    """Unit tests for DemoIssueTracker."""

    def setUp(self) -> None:
        self.setUpPyfakefs()
        self.working_dir = AsyncPath('/working')
        self.fs.create_dir(self.working_dir.path)

    def test_init_sets_attributes(self) -> None:
        """Tests that __init__ configures tracker attributes correctly."""
        tracker = DemoIssueTracker(self.working_dir)

        self.assertEqual(tracker.component_id, 1337)
        self.assertEqual(tracker.ccs, ['fake1@fake.fake', 'fake2@fake.fake'])
        self.assertEqual(tracker.primary_hotlist_id, 8675309)
        self.assertEqual(tracker.extra_hotlist_ids, [1000000, 2000000, 3000000])
        self.assertEqual(
            tracker.hotlist_ids, [8675309, 1000000, 2000000, 3000000]
        )
        self.assertEqual(tracker.default_assignee, 'rotation@fake.fake')
        self.assertEqual(tracker._issues, self.working_dir / 'b')
        self.assertFalse(tracker._issues.path.exists())
        self.assertEqual(tracker._next_issue_id, 123456789)

    async def test_create_and_read_base_issue(self) -> None:
        """Tests creating and reading a base Issue."""
        tracker = DemoIssueTracker(self.working_dir)

        issue = Issue(
            title='Test Bug Title',
            description='Test bug description',
            comments=['Comment 1', 'Comment 2'],
            priority=2,
            severity=3,
            assignee='test@fake.fake',
            cl_num=54321,
            location=CodeSnippet(file='foo/bar.cc', lines=(10, 20)),
        )

        created = await tracker.create(issue)
        self.assertEqual(created.issue_id, 123456789)
        self.assertEqual(created.title, 'Test Bug Title')
        self.assertGreaterEqual(tracker._next_issue_id, 123456789 + 100)
        self.assertLessEqual(tracker._next_issue_id, 123456789 + 1000)

        # Verify JSON file on disk
        issue_file = tracker._issues.path / '123456789'
        self.assertTrue(issue_file.is_file())
        with open(issue_file, 'r', encoding='utf-8') as f:
            data = json.load(f)

        self.assertEqual(data['issue_id'], 123456789)
        self.assertEqual(data['title'], 'Test Bug Title')
        self.assertEqual(data['description'], 'Test bug description')
        self.assertEqual(data['comments'], ['Comment 1', 'Comment 2'])
        self.assertEqual(data['priority'], 2)
        self.assertEqual(data['severity'], 3)
        self.assertEqual(data['assignee'], 'test@fake.fake')
        self.assertEqual(data['cl_num'], 54321)
        self.assertEqual(
            data['location'], {'file': 'foo/bar.cc', 'lines': [10, 20]}
        )

        # Read issue back
        loaded = await tracker.read(123456789)
        self.assertIsInstance(loaded, Issue)
        self.assertEqual(loaded.issue_id, 123456789)
        self.assertEqual(loaded.title, 'Test Bug Title')
        self.assertEqual(loaded.description, 'Test bug description')
        self.assertEqual(loaded.comments, ['Comment 1', 'Comment 2'])
        self.assertEqual(loaded.priority, 2)
        self.assertEqual(loaded.severity, 3)
        self.assertEqual(loaded.assignee, 'test@fake.fake')
        self.assertEqual(loaded.cl_num, 54321)
        self.assertIsNotNone(loaded.location)
        assert loaded.location is not None
        self.assertEqual(loaded.location.file, 'foo/bar.cc')
        self.assertEqual(loaded.location.lines, (10, 20))

    async def test_create_freshness_result(self) -> None:
        """Tests creating a FreshnessResult issue."""
        tracker = DemoIssueTracker(self.working_dir)

        freshness_result = FreshnessResult(
            title='Stale package test',
            description='Package needs update',
            package='some-package',
            pkg_type='bazel_dep',
            current=PackageVersion('1.0.0', date(2026, 1, 1)),
            earliest=PackageVersion('2.0.0', date(2026, 6, 1)),
            tier=1,
            location=CodeSnippet(file='MODULE.bazel', lines=(5, 10)),
        )

        created = await tracker.create(freshness_result)
        self.assertIsInstance(created, FreshnessResult)
        assert isinstance(created, FreshnessResult)
        self.assertEqual(created.issue_id, 123456789)
        self.assertEqual(created.package, 'some-package')

        # Verify JSON file on disk
        issue_file = tracker._issues.path / '123456789'
        self.assertTrue(issue_file.is_file())

    async def test_read_missing_issue_raises(self) -> None:
        """Tests that reading a nonexistent issue ID asserts."""
        tracker = DemoIssueTracker(self.working_dir)
        with self.assertRaises(AssertionError):
            await tracker.read(999999999)

    async def test_read_hotlist(self) -> None:
        """Tests querying issues by hotlist ID."""
        tracker = DemoIssueTracker(self.working_dir)

        issue1 = Issue(title='Issue 1')
        issue2 = Issue(title='Issue 2')
        created1 = await tracker.create(issue1)
        created2 = await tracker.create(issue2)

        # Primary hotlist ID
        primary_id = tracker.primary_hotlist_id
        assert primary_id is not None
        primary_issues = [i async for i in tracker.read_hotlist(primary_id)]
        self.assertEqual(len(primary_issues), 2)
        issue_ids = [i.issue_id for i in primary_issues]
        self.assertIn(created1.issue_id, issue_ids)
        self.assertIn(created2.issue_id, issue_ids)

        # Extra hotlist ID
        extra_issues = [
            i async for i in tracker.read_hotlist(tracker.extra_hotlist_ids[0])
        ]
        self.assertEqual(len(extra_issues), 2)
        issue_ids = [i.issue_id for i in extra_issues]
        self.assertIn(created1.issue_id, issue_ids)
        self.assertIn(created2.issue_id, issue_ids)

        # Non-matching hotlist ID
        unmatched_issues = [i async for i in tracker.read_hotlist(9999999)]
        self.assertEqual(len(unmatched_issues), 0)

    async def test_read_hotlist_nonexistent_dir(self) -> None:
        """Tests querying hotlist when _issues directory is missing."""
        tracker = DemoIssueTracker(self.working_dir)
        primary_id = tracker.primary_hotlist_id
        assert primary_id is not None
        issues = [i async for i in tracker.read_hotlist(primary_id)]
        self.assertEqual(len(issues), 0)

    def test_parse_args(self) -> None:
        """Tests CLI argument parsing."""
        tracker = DemoIssueTracker(self.working_dir)

        args_empty = tracker.parse_args([])
        self.assertIsNone(args_empty.issue)

        args_short = tracker.parse_args(['-i', '123456789'])
        self.assertEqual(args_short.issue, 123456789)

        args_long = tracker.parse_args(['--issue', '987654321'])
        self.assertEqual(args_long.issue, 987654321)

        args_work_dir = tracker.parse_args(['-w', '/custom/work'])
        self.assertEqual(args_work_dir.working_dir, '/custom/work')

    async def test_list_issues(self) -> None:
        """Tests listing issue files."""
        tracker = DemoIssueTracker(self.working_dir)

        # Initially empty (and directory doesn't exist yet)
        with patch('sys.stdout', new=io.StringIO()) as mock_stdout:
            tracker.list_issues()
            self.assertEqual(mock_stdout.getvalue(), '')

        # Create issues via tracker
        await tracker.create(Issue(title='Issue 100'))
        await tracker.create(Issue(title='Issue 200'))

        with patch('sys.stdout', new=io.StringIO()) as mock_stdout:
            tracker.list_issues()
            output = mock_stdout.getvalue()
            self.assertIn('123456789\n', output)

    async def test_display_issue(self) -> None:
        """Tests reading and pretty-printing an issue."""
        tracker = DemoIssueTracker(self.working_dir)
        issue = Issue(title='Display Bug', description='Testing display')
        created = await tracker.create(issue)

        assert created.issue_id is not None
        with patch('sys.stdout', new=io.StringIO()) as mock_stdout:
            await tracker.display_issue(created.issue_id)
            output = mock_stdout.getvalue()
            self.assertIn('"title": "Display Bug"', output)
            self.assertIn(f'"issue_id": {created.issue_id}', output)

    async def test_display_missing_issue_raises(self) -> None:
        """Tests that display_issue on missing issue asserts."""
        tracker = DemoIssueTracker(self.working_dir)
        with self.assertRaises(AssertionError):
            await tracker.display_issue(999999999)

    def test_main_list_issues(self) -> None:
        """Tests running main() with no issue specified."""
        with (
            patch.object(DemoIssueTracker, 'list_issues') as mock_list,
            patch.object(DemoIssueTracker, 'display_issue') as mock_display,
        ):
            main([])
            self.assertTrue(mock_list.called)
            self.assertFalse(mock_display.called)

    def test_main_display_issue(self) -> None:
        """Tests running main() with -i flag."""
        with (
            patch.object(DemoIssueTracker, 'list_issues') as mock_list,
            patch.object(DemoIssueTracker, 'display_issue') as mock_display,
        ):
            main(['-i', '123456789'])
            self.assertFalse(mock_list.called)
            mock_display.assert_called_once_with(123456789)


if __name__ == '__main__':
    unittest.main()
