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
"""Tests for PocAndFixGenerator stage."""

import asyncio
import unittest
from unittest.mock import AsyncMock, MagicMock, patch

from pyfakefs.fake_filesystem_unittest import TestCaseMixin
from pw_fortifier.async_path import AsyncPath
from pw_fortifier.code_snippet import CodeSnippet
from pw_fortifier.defect import Defect
from pw_fortifier.git_utils import GitBranch, WritableGitWorkspace
from pw_fortifier.pipeline_stage import PipelineSink
from pw_fortifier.poc_and_fix_generator import PocAndFixGeneratorStub
from pw_fortifier.scanner import configure_stage_for_test


class TestPocAndFixGenerator(unittest.IsolatedAsyncioTestCase, TestCaseMixin):
    """Unit tests for PocAndFixGenerator functionality."""

    def setUp(self) -> None:
        self.setUpPyfakefs()
        self.working_dir = AsyncPath('/working')
        self.fs.create_dir(self.working_dir.path)

        # Mock WritableGitWorkspace and GitBranch
        self.mock_branch = MagicMock(spec=GitBranch)
        self.mock_branch.setup = AsyncMock(return_value=self.mock_branch)
        self.mock_branch.teardown = AsyncMock()
        self.mock_branch.reset = AsyncMock()
        self.mock_branch.add = AsyncMock()
        self.mock_branch.diff = AsyncMock(
            return_value=['diff line 1', 'diff line 2']
        )
        self.mock_branch.commit = AsyncMock()
        self.mock_branch.push = AsyncMock(return_value=654321)

        self.mock_dst_repo = MagicMock(spec=WritableGitWorkspace)
        self.mock_dst_repo.branch.return_value = self.mock_branch
        self.mock_dst_repo.project_dir = '/working/dst'

    @patch('pw_fortifier.poc_and_fix_generator.run_unit_tests')
    async def test_process_one_modified(self, mock_unit_tests) -> None:
        """Tests processing defect when PoC/fix generates and uploads CL."""

        async def fake_unit_tests(_cwd='.'):
            # When fix is generated, no tests fail
            return
            yield

        mock_unit_tests.side_effect = fake_unit_tests

        generator = PocAndFixGeneratorStub(
            poc_result='//pw_foo:poc_test',
            fix_result=True,
            summary=('Custom Title', 'Custom Description'),
        )
        next_stage = PipelineSink()
        generator.connect(next_stage)

        await configure_stage_for_test(
            generator,
            working_dir=str(self.working_dir),
            dst_repo=self.mock_dst_repo,
            allow_edits=True,
            allow_uploads=True,
        )

        defect = Defect(
            location=CodeSnippet(file='foo/bar.cc'),
            title='Sample Defect',
            description='Defect description',
            issue_id=789,
        )

        path = AsyncPath(self.working_dir, 'defect.json')
        await defect.save(path)

        run_task = asyncio.create_task(generator.run())
        await generator.input_queue.put(path)
        await generator.input_queue.put(None)
        await run_task

        # Verify stub executed
        self.assertEqual(len(generator.passed_defects), 1)

        # Verify branch actions
        self.mock_dst_repo.branch.assert_called_once_with(
            name='b789', keep=False
        )
        self.mock_branch.setup.assert_awaited_once()
        self.mock_branch.add.assert_called_once()
        self.mock_branch.diff.assert_called_once_with(staged=True)
        self.mock_branch.commit.assert_called_once_with(
            'Custom Title\n\nCustom Description\n\nBug: 789', amend=True
        )
        self.mock_branch.push.assert_called_once()
        self.mock_branch.teardown.assert_awaited_once()

        # Verify forwarded defect updated with CL number
        fwd = await next_stage.input_queue.get()
        assert fwd is not None

        out_defect = await Defect.load(fwd)
        self.assertEqual(out_defect.cl_num, 654321)

    async def test_process_one_not_modified(self) -> None:
        """Tests processing defect when no changes are generated."""
        generator = PocAndFixGeneratorStub(
            poc_result=None,
            fix_result=False,
        )
        next_stage = PipelineSink()
        generator.connect(next_stage)

        await configure_stage_for_test(
            generator,
            working_dir=str(self.working_dir),
            dst_repo=self.mock_dst_repo,
            allow_edits=True,
            allow_uploads=True,
        )

        defect = Defect(
            location=CodeSnippet(file='foo/bar.cc'),
            title='Sample Defect',
            description='Defect description',
            issue_id=789,
        )

        path = AsyncPath(self.working_dir, 'defect.json')
        await defect.save(path)

        run_task = asyncio.create_task(generator.run())
        await generator.input_queue.put(path)
        await generator.input_queue.put(None)
        await run_task

        self.mock_branch.reset.assert_awaited_once_with(staged=True)
        self.mock_branch.commit.assert_not_called()
        self.mock_branch.push.assert_not_called()

        fwd = await next_stage.input_queue.get()
        assert fwd is not None

        out_defect = await Defect.load(fwd)
        self.assertIsNone(out_defect.cl_num)

    @patch('pw_fortifier.poc_and_fix_generator.run_unit_tests')
    async def test_process_one_allow_edits_without_uploads(
        self, mock_unit_tests
    ) -> None:
        """Tests allow_edits without uploads keeps branch and skips push."""

        async def fake_unit_tests(_cwd='.'):
            return
            yield

        mock_unit_tests.side_effect = fake_unit_tests

        generator = PocAndFixGeneratorStub(
            poc_result='//pw_foo:poc_test',
            fix_result=True,
            summary=('Custom Title', 'Custom Description'),
        )
        next_stage = PipelineSink()
        generator.connect(next_stage)

        await configure_stage_for_test(
            generator,
            working_dir=str(self.working_dir),
            dst_repo=self.mock_dst_repo,
            allow_edits=True,
            allow_uploads=False,
        )

        defect = Defect(
            location=CodeSnippet(file='foo/bar.cc'),
            title='Sample Defect',
            description='Defect description',
            issue_id=789,
        )

        path = AsyncPath(self.working_dir, 'defect.json')
        await defect.save(path)

        run_task = asyncio.create_task(generator.run())
        await generator.input_queue.put(path)
        await generator.input_queue.put(None)
        await run_task

        # Verify branch created with keep=True and push was not called
        self.mock_dst_repo.branch.assert_called_once_with(
            name='b789', keep=True
        )
        self.mock_branch.add.assert_called_once()
        self.mock_branch.commit.assert_called_once()
        self.mock_branch.push.assert_not_called()

        fwd = await next_stage.input_queue.get()
        assert fwd is not None

        out_defect = await Defect.load(fwd)
        self.assertIsNone(out_defect.cl_num)


if __name__ == '__main__':
    unittest.main()
