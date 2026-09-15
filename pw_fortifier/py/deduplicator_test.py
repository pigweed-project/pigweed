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
"""Tests for the Deduplicator class in pw_fortifier."""

# pylint: disable=protected-access

import asyncio
import unittest

from pyfakefs.fake_filesystem_unittest import TestCaseMixin
from pw_fortifier.async_path import AsyncPath
from pw_fortifier.deduplicator import DeduplicatorStub
from pw_fortifier.pipeline_stage import PipelineSink
from pw_fortifier.issue import Issue
from pw_fortifier.scanner import configure_stage_for_test


class TestDeduplicator(unittest.IsolatedAsyncioTestCase, TestCaseMixin):
    """Unit tests for the Deduplicator class using a stub."""

    def setUp(self) -> None:
        self.setUpPyfakefs()
        self.working_dir = AsyncPath('/working')
        self.fs.create_dir(self.working_dir.path)

    async def test_process_one_forwards_non_duplicate(self) -> None:
        """Tests that a non-duplicate file is forwarded to the next stage."""
        # is_duplicate_val = False means NOT duplicate (should forward)
        dedup = DeduplicatorStub(is_duplicate_val=False)

        next_stage = PipelineSink()
        dedup.connect(next_stage)

        await configure_stage_for_test(dedup, working_dir=str(self.working_dir))

        # Put a fake issue in the input queue and run the stage
        test_file = AsyncPath(self.working_dir, 'new_issue.json')
        issue = Issue(
            issue_id=None,
            title='Test Issue',
            description='issue info',
        )
        await issue.save(test_file)

        # Start stage execution
        run_task = asyncio.create_task(dedup.run())

        # Send file and then sentinel
        await dedup.input_queue.put(test_file)
        await dedup.input_queue.put(None)
        await run_task

        # It should have checked the issue
        self.assertEqual(len(dedup.checked_issues), 1)
        self.assertEqual(dedup.checked_issues[0].description, 'issue info')

        # Since it's not a duplicate, it should be forwarded
        self.assertEqual(
            next_stage.input_queue.qsize(), 2
        )  # 1 forwarded path + 1 sentinel (None)
        forwarded_path = await next_stage.input_queue.get()
        assert forwarded_path is not None
        self.assertEqual(forwarded_path.name, 'new_issue.json')
        self.assertTrue(
            await forwarded_path.exists()
        )  # Should be moved to stage_out

    async def test_process_one_drops_duplicate(self) -> None:
        """Tests that a duplicate file is dropped (deleted, not forwarded)."""
        # is_duplicate_val = True means duplicate (should drop)
        dedup = DeduplicatorStub(is_duplicate_val=True)

        next_stage = PipelineSink()
        dedup.connect(next_stage)

        await configure_stage_for_test(dedup, working_dir=str(self.working_dir))

        test_file = AsyncPath(self.working_dir, 'duplicate_issue.json')
        issue = Issue(
            issue_id=None,
            title='Test Issue',
            description='duplicate issue info',
        )
        await issue.save(test_file)

        run_task = asyncio.create_task(dedup.run())

        await dedup.input_queue.put(test_file)
        await dedup.input_queue.put(None)
        await run_task

        # It should have checked the issue
        self.assertEqual(len(dedup.checked_issues), 1)
        self.assertEqual(
            dedup.checked_issues[0].description, 'duplicate issue info'
        )

        # Since it's a duplicate, it should be deleted from stage_in and NOT
        # forwarded
        self.assertFalse(
            await AsyncPath(
                self.working_dir, 'deduplicator_stub_in/duplicate_defect.json'
            ).exists()
        )
        self.assertFalse(
            await AsyncPath(
                self.working_dir, 'deduplicator_stub_out/duplicate_defect.json'
            ).exists()
        )

        # It should be moved to duplicates directory
        duplicates_dir = self.working_dir / 'duplicates'
        dup_files = [f async for f in duplicates_dir.iterdir()]
        self.assertEqual(len(dup_files), 1)
        self.assertTrue(dup_files[0].name.startswith('b12345-'))

        # The out_queue should only receive the None sentinel
        self.assertEqual(next_stage.input_queue.qsize(), 1)
        sentinel = await next_stage.input_queue.get()
        self.assertIsNone(sentinel)


if __name__ == '__main__':
    unittest.main()
