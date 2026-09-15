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
"""Tests for the IssueTracker, IssueReader, and IssueWriter classes."""

# pylint: disable=protected-access

import asyncio
import unittest
from unittest.mock import patch, MagicMock

from pyfakefs.fake_filesystem_unittest import TestCaseMixin
from pw_fortifier.async_path import AsyncPath
from pw_fortifier.issue import Issue
from pw_fortifier.issue_tracker import (
    IssueReader,
    IssueWriter,
    IssueTrackerStub,
)
from pw_fortifier.pipeline_stage import PipelineSink
from pw_fortifier.scanner import configure_stage_for_test


class TestIssueTracker(unittest.IsolatedAsyncioTestCase):
    """Unit tests for IssueTracker base class and IssueTrackerStub."""

    async def test_tracker_stub(self) -> None:
        """Tests IssueTrackerStub add_issue, create, read, and read_hotlist."""
        stub = IssueTrackerStub()

        # Test properties
        self.assertEqual(stub.component_id, 0)
        self.assertEqual(stub.ccs, [])
        self.assertIsNone(stub.primary_hotlist_id)
        self.assertEqual(stub.extra_hotlist_ids, [])
        self.assertEqual(stub.hotlist_ids, [])
        self.assertIsNone(stub.default_assignee)

        stub.component_id = 1234
        stub.ccs = ['user@google.com']
        stub.primary_hotlist_id = 1000
        stub.extra_hotlist_ids = [2000, 3000]
        stub.default_assignee = 'default@google.com'

        self.assertEqual(stub.component_id, 1234)
        self.assertEqual(stub.ccs, ['user@google.com'])
        self.assertEqual(stub.primary_hotlist_id, 1000)
        self.assertEqual(stub.extra_hotlist_ids, [2000, 3000])
        self.assertEqual(stub.hotlist_ids, [1000, 2000, 3000])
        self.assertEqual(stub.default_assignee, 'default@google.com')

        # Test add_issue and read
        issue1 = Issue(issue_id=100, title='Issue 100')
        stub.add_issue(issue1, hotlist_ids=[5])
        read_res = await stub.read(100)
        self.assertEqual(read_res.title, 'Issue 100')

        # Test read_hotlist
        hotlist_issues = [i async for i in stub.read_hotlist(5)]
        self.assertEqual(len(hotlist_issues), 1)
        self.assertEqual(hotlist_issues[0].issue_id, 100)

        # Test create attaches configured hotlist_ids
        new_issue = Issue(title='New Issue')
        created = await stub.create(new_issue)
        self.assertEqual(created.issue_id, 8675309)
        self.assertEqual(stub.next_issue_id, 8675310)

        primary_issues = [i async for i in stub.read_hotlist(1000)]
        self.assertEqual(len(primary_issues), 1)
        self.assertEqual(primary_issues[0].issue_id, 8675309)


class FakeIssueReader(IssueReader):
    """Concrete IssueReader subclass for testing."""

    @classmethod
    def _convert(cls, issue: Issue) -> Issue | None:
        return issue


class FakeIssueWriter(IssueWriter):
    """Concrete IssueWriter subclass for testing."""

    async def _load(self, path: AsyncPath) -> Issue:
        return await Issue.load(path)


class TestIssueReader(unittest.IsolatedAsyncioTestCase, TestCaseMixin):
    """Unit tests for IssueReader with IssueTrackerStub."""

    def setUp(self) -> None:
        self.setUpPyfakefs()
        self.working_dir = AsyncPath('/working')
        self.fs.create_dir(self.working_dir.path)

    async def test_run_reads_and_forwards_issues(self) -> None:
        """Tests running IssueReader with issue and hotlist IDs."""
        tracker_stub = IssueTrackerStub()
        tracker_stub.add_issue(Issue(issue_id=100, title='Issue 100'))
        tracker_stub.add_issue(Issue(issue_id=200, title='Issue 200'))
        tracker_stub.add_issue(
            Issue(issue_id=51, title='Hotlist 5 Issue 1'), hotlist_ids=[5]
        )
        tracker_stub.add_issue(
            Issue(issue_id=52, title='Hotlist 5 Issue 2'), hotlist_ids=[5]
        )

        reader = FakeIssueReader(tracker_stub)
        next_stage = PipelineSink()
        reader.connect(next_stage)

        await configure_stage_for_test(
            reader,  # type: ignore[arg-type]
            working_dir=str(self.working_dir),
            issues=[100, 200],
            hotlists=[5],
        )

        await reader.run()

        results = []
        while not next_stage.input_queue.empty():
            path = await next_stage.input_queue.get()
            if path is None:
                break
            issue = await Issue.load(path)
            results.append(issue.issue_id)

        self.assertEqual(results, [100, 200, 51, 52])


class TestIssueWriter(unittest.IsolatedAsyncioTestCase, TestCaseMixin):
    """Unit tests for IssueWriter with IssueTrackerStub."""

    def setUp(self) -> None:
        self.setUpPyfakefs()
        self.working_dir = AsyncPath('/working')
        self.fs.create_dir(self.working_dir.path)

    @patch('builtins.print')
    async def test_process_one_dry_run_non_verbose(
        self, mock_print: MagicMock
    ) -> None:
        """Tests non-verbose dry run does not print."""
        initial_issue = Issue(
            issue_id=None,
            title='Dry Run Issue',
            description='Test description',
        )

        tracker_stub = IssueTrackerStub()
        writer = FakeIssueWriter(tracker_stub)
        next_stage = PipelineSink()
        writer.connect(next_stage)

        await configure_stage_for_test(
            writer,
            working_dir=str(self.working_dir),
            create_bugs=False,
            verbose=False,
        )

        issue_file = AsyncPath(self.working_dir, 'dry_run_issue.json')
        await initial_issue.save(issue_file)

        run_task = asyncio.create_task(writer.run())
        await writer.input_queue.put(issue_file)
        await writer.input_queue.put(None)
        await run_task

        mock_print.assert_not_called()

    @patch('builtins.print')
    async def test_process_one_dry_run_verbose(
        self, mock_print: MagicMock
    ) -> None:
        """Tests verbose dry run prints issue title and description."""
        initial_issue = Issue(
            issue_id=None,
            title='Dry Run Issue',
            description='Test description',
        )

        tracker_stub = IssueTrackerStub()
        writer = FakeIssueWriter(tracker_stub)
        next_stage = PipelineSink()
        writer.connect(next_stage)

        await configure_stage_for_test(
            writer,
            working_dir=str(self.working_dir),
            create_bugs=False,
            verbose=True,
        )

        issue_file = AsyncPath(self.working_dir, 'dry_run_issue.json')
        await initial_issue.save(issue_file)

        run_task = asyncio.create_task(writer.run())
        await writer.input_queue.put(issue_file)
        await writer.input_queue.put(None)
        await run_task

        mock_print.assert_any_call('Title: Dry Run Issue')
        mock_print.assert_any_call('Description: Test description')

    async def test_process_one_creates_issue(self) -> None:
        """Tests issue processing when filing a bug."""
        initial_issue = Issue(
            issue_id=None,
            title='Unfiled Issue',
            description='An issue that has not been filed yet.',
        )

        tracker_stub = IssueTrackerStub()
        writer = FakeIssueWriter(tracker_stub)
        next_stage = PipelineSink()
        writer.connect(next_stage)

        await configure_stage_for_test(
            writer,
            working_dir=str(self.working_dir),
            create_bugs=True,
        )

        issue_file = AsyncPath(self.working_dir, 'unfiled_issue.json')
        await initial_issue.save(issue_file)

        run_task = asyncio.create_task(writer.run())
        await writer.input_queue.put(issue_file)
        await writer.input_queue.put(None)
        await run_task

        forwarded_path = await next_stage.input_queue.get()
        assert forwarded_path is not None
        updated_issue = await Issue.load(forwarded_path)
        self.assertEqual(updated_issue.title, 'Unfiled Issue')
        self.assertEqual(updated_issue.issue_id, 8675309)

    @patch('builtins.print')
    async def test_process_one_create_bugs_verbose(
        self, mock_print: MagicMock
    ) -> None:
        """Tests that verbose mode prints when filing bugs too."""
        initial_issue = Issue(
            issue_id=None,
            title='Verbose Created Issue',
            description='Test verbose creation.',
        )

        tracker_stub = IssueTrackerStub()
        writer = FakeIssueWriter(tracker_stub)
        next_stage = PipelineSink()
        writer.connect(next_stage)

        await configure_stage_for_test(
            writer,
            working_dir=str(self.working_dir),
            create_bugs=True,
            verbose=True,
        )

        issue_file = AsyncPath(self.working_dir, 'verbose_issue.json')
        await initial_issue.save(issue_file)

        run_task = asyncio.create_task(writer.run())
        await writer.input_queue.put(issue_file)
        await writer.input_queue.put(None)
        await run_task

        mock_print.assert_any_call('Title: Verbose Created Issue')
        mock_print.assert_any_call('Description: Test verbose creation.')

    async def test_process_one_applies_default_assignee(self) -> None:
        """Tests that default_assignee is applied when issue has no assignee."""
        initial_issue = Issue(
            issue_id=None,
            title='Unassigned Issue',
            description='An issue without an assignee.',
            assignee=None,
        )

        tracker_stub = IssueTrackerStub()
        tracker_stub.default_assignee = 'default-assignee@google.com'
        writer = FakeIssueWriter(tracker_stub)
        next_stage = PipelineSink()
        writer.connect(next_stage)

        await configure_stage_for_test(
            writer,
            working_dir=str(self.working_dir),
            create_bugs=False,
        )

        issue_file = AsyncPath(self.working_dir, 'unassigned_issue.json')
        await initial_issue.save(issue_file)

        run_task = asyncio.create_task(writer.run())
        await writer.input_queue.put(issue_file)
        await writer.input_queue.put(None)
        await run_task

        forwarded_path = await next_stage.input_queue.get()
        assert forwarded_path is not None
        updated_issue = await Issue.load(forwarded_path)
        self.assertEqual(updated_issue.assignee, 'default-assignee@google.com')

    async def test_process_one_preserves_existing_assignee(self) -> None:
        """Tests that existing assignee is not overridden by default."""
        initial_issue = Issue(
            issue_id=None,
            title='Assigned Issue',
            description='An issue with an assignee.',
            assignee='core-owner@google.com',
        )

        tracker_stub = IssueTrackerStub()
        tracker_stub.default_assignee = 'default-assignee@google.com'
        writer = FakeIssueWriter(tracker_stub)
        next_stage = PipelineSink()
        writer.connect(next_stage)

        await configure_stage_for_test(
            writer,
            working_dir=str(self.working_dir),
            create_bugs=False,
        )

        issue_file = AsyncPath(self.working_dir, 'assigned_issue.json')
        await initial_issue.save(issue_file)

        run_task = asyncio.create_task(writer.run())
        await writer.input_queue.put(issue_file)
        await writer.input_queue.put(None)
        await run_task

        forwarded_path = await next_stage.input_queue.get()
        assert forwarded_path is not None
        updated_issue = await Issue.load(forwarded_path)
        self.assertEqual(updated_issue.assignee, 'core-owner@google.com')

    async def test_process_one_invalid_issue_raises(self) -> None:
        """Tests that loading an invalid/empty issue raises ValueError."""
        tracker_stub = IssueTrackerStub()
        writer = FakeIssueWriter(tracker_stub)
        await configure_stage_for_test(
            writer,
            working_dir=str(self.working_dir),
            create_bugs=False,
        )

        empty_file = AsyncPath(self.working_dir, 'empty_issue.json')
        await empty_file.write_text('')

        with patch.object(writer, '_load', return_value=None):
            with self.assertRaises(ValueError) as ctx:
                await writer._process_one(empty_file)
        self.assertIn('Failed to load issue', str(ctx.exception))


if __name__ == '__main__':
    unittest.main()
