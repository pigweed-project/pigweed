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
"""Tests for the Critic class in pw_fortifier."""

# pylint: disable=protected-access

import asyncio
import unittest

from pyfakefs.fake_filesystem_unittest import TestCaseMixin
from pw_fortifier.async_path import AsyncPath
from pw_fortifier.code_snippet import CodeSnippet
from pw_fortifier.critic import CriticStub
from pw_fortifier.pipeline_stage import PipelineSink
from pw_fortifier.defect import Defect
from pw_fortifier.scanner import configure_stage_for_test


class TestCritic(unittest.IsolatedAsyncioTestCase, TestCaseMixin):
    """Unit tests for the Critic class using a stub."""

    def setUp(self) -> None:
        self.setUpPyfakefs()
        self.working_dir = AsyncPath('/working')
        self.fs.create_dir(self.working_dir.path)

    async def test_process_one_forwards_validated_finding(self) -> None:
        """Tests that a validated finding is forwarded."""
        # criticize_val = True means valid finding (should forward)
        critic = CriticStub(criticize_val=True)

        next_stage = PipelineSink()
        critic.connect(next_stage)

        await configure_stage_for_test(
            critic,
            working_dir=str(self.working_dir),
        )

        test_file = AsyncPath(self.working_dir, 'valid_defect.json')
        defect = Defect(
            location=CodeSnippet(file='valid_defect.json'),
            issue_id=None,
            title='Test Defect',
            description='defect info',
            cl_num=None,
        )
        await defect.save(test_file)

        # Start stage execution
        run_task = asyncio.create_task(critic.run())

        # Send file and then sentinel
        await critic.input_queue.put(test_file)
        await critic.input_queue.put(None)
        await run_task

        # It should have checked the defect
        self.assertEqual(len(critic.checked_defects), 1)
        self.assertEqual(critic.checked_defects[0].description, 'defect info')

        # Since it is valid, it should be forwarded
        self.assertEqual(
            next_stage.input_queue.qsize(), 2
        )  # 1 forwarded path + 1 sentinel (None)
        forwarded_path = await next_stage.input_queue.get()
        assert forwarded_path is not None
        self.assertEqual(forwarded_path.name, 'valid_defect.json')
        self.assertTrue(
            await forwarded_path.exists()
        )  # Should be moved to stage_out

    async def test_process_one_drops_invalid_finding(self) -> None:
        """Tests that an invalid finding is dropped."""
        # criticize_val = False means invalid finding (should drop/delete)
        critic = CriticStub(criticize_val=False)

        next_stage = PipelineSink()
        critic.connect(next_stage)

        await configure_stage_for_test(
            critic,
            working_dir=str(self.working_dir),
        )

        test_file = AsyncPath(self.working_dir, 'invalid_defect.json')
        defect = Defect(
            location=CodeSnippet(file='invalid_defect.json'),
            issue_id=None,
            title='Test Defect',
            description='invalid defect info',
            cl_num=None,
        )
        await defect.save(test_file)

        run_task = asyncio.create_task(critic.run())

        await critic.input_queue.put(test_file)
        await critic.input_queue.put(None)
        await run_task

        # It should have checked the defect
        self.assertEqual(len(critic.checked_defects), 1)
        self.assertEqual(
            critic.checked_defects[0].description, 'invalid defect info'
        )

        # Since it is invalid, it should be deleted from stage_in and NOT
        # forwarded
        self.assertFalse(
            await AsyncPath(
                self.working_dir, 'critic_in/invalid_defect.json'
            ).exists()
        )
        self.assertFalse(
            await AsyncPath(
                self.working_dir, 'critic_out/invalid_defect.json'
            ).exists()
        )

        # The out_queue should only receive the None sentinel
        self.assertEqual(next_stage.input_queue.qsize(), 1)
        sentinel = await next_stage.input_queue.get()
        self.assertIsNone(sentinel)


if __name__ == '__main__':
    unittest.main()
