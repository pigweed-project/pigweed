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
"""Tests for the Triager class in pw_fortifier."""

# pylint: disable=protected-access

import asyncio
import unittest
from unittest.mock import patch

from pyfakefs.fake_filesystem_unittest import TestCaseMixin
from pw_fortifier.async_path import AsyncPath
from pw_fortifier.code_snippet import CodeSnippet
from pw_fortifier.issue import Issue
from pw_fortifier.pipeline_stage import PipelineSink
from pw_fortifier.scanner import configure_stage_for_test
from pw_fortifier.triager import Triager, TriagerStub


class TestTriager(unittest.IsolatedAsyncioTestCase, TestCaseMixin):
    """Unit tests for the Triager class using a stub."""

    def setUp(self) -> None:
        self.setUpPyfakefs()
        self.working_dir = AsyncPath('/working')
        self.fs.create_dir(self.working_dir.path)

    async def test_process_one_triages_and_forwards_issue(self) -> None:
        """Tests that an issue is triaged, saved, and forwarded."""
        # Create a base issue
        base_issue = Issue(
            issue_id=None,
            title='Triaged Issue',
            description='An issue that has been triaged.',
        )

        triager = TriagerStub(base_issue)

        next_stage = PipelineSink()
        triager.connect(next_stage)

        await configure_stage_for_test(
            triager,
            working_dir=str(self.working_dir),
        )

        # Input is already an issue JSON file
        test_file = AsyncPath(self.working_dir, 'issue-12345.json')
        initial_issue = Issue(
            issue_id=None,
            title='Untriaged Issue',
            description='An issue.',
        )
        await initial_issue.save(test_file)

        # Start stage execution
        run_task = asyncio.create_task(triager.run())

        # Send file and then sentinel
        await triager.input_queue.put(test_file)
        await triager.input_queue.put(None)
        await run_task

        # It should have triaged the file
        self.assertEqual(len(triager.triaged_issues), 1)

        # It should have forwarded the issue (and None sentinel)
        self.assertEqual(next_stage.input_queue.qsize(), 2)

        forwarded_path = await next_stage.input_queue.get()
        assert forwarded_path is not None

        # The issue filename should match the input filename
        self.assertEqual(forwarded_path.name, 'issue-12345.json')
        self.assertTrue(await forwarded_path.exists())

        # Load and verify the forwarded issue (should be the stub_issue)
        issue = await Issue.load(forwarded_path)
        self.assertEqual(issue.title, 'Triaged Issue')

    @patch(
        'pw_fortifier.triager.CoreOwnerFinder.find',
        return_value='resolved_owner@google.com',
    )
    async def test_triager_resolves_assignee_from_location(
        self, _mock_find
    ) -> None:
        """Tests Triager._process_one resolves assignee using finder."""

        class ConcreteTriager(Triager):
            async def _triage(self, issue: Issue) -> None:
                pass

        triager = ConcreteTriager()
        next_stage = PipelineSink()
        triager.connect(next_stage)

        await configure_stage_for_test(
            triager,
            working_dir=str(self.working_dir),
        )

        test_file = AsyncPath(self.working_dir, 'unassigned.json')
        initial_issue = Issue(
            location=CodeSnippet(file='pw_foo/foo.cc', lines=(10, 20)),
            issue_id=None,
            title='Issue with Location',
            description='Location details',
        )
        await initial_issue.save(test_file)

        run_task = asyncio.create_task(triager.run())
        await triager.input_queue.put(test_file)
        await triager.input_queue.put(None)
        await run_task

        forwarded_path = await next_stage.input_queue.get()
        assert forwarded_path is not None
        saved_issue = await Issue.load(forwarded_path)
        self.assertEqual(saved_issue.assignee, 'resolved_owner@google.com')


if __name__ == '__main__':
    unittest.main()
