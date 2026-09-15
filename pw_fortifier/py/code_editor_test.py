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
"""Tests for the CodeEditor pipeline stage."""

# pylint: disable=protected-access

import asyncio
from typing import Iterator
import unittest
from unittest.mock import AsyncMock, MagicMock

from pyfakefs.fake_filesystem_unittest import TestCaseMixin

from pw_fortifier.async_path import AsyncPath
from pw_fortifier.code_editor import CodeEditor
from pw_fortifier.git_utils import GitBranch, WritableGitWorkspace
from pw_fortifier.issue import Issue
from pw_fortifier.pipeline_stage import PipelineSink
from pw_fortifier.scanner import configure_stage_for_test


class TestCodeEditorStage(CodeEditor):
    """Test implementation of CodeEditor."""

    def __init__(
        self,
        generate_result: bool = True,
        validate_result: bool = True,
        commit_msg: tuple[str, ...] = ('Test Title', 'Test Description'),
        name: str = 'test_code_editor',
    ) -> None:
        super().__init__(name)
        self.generate_result = generate_result
        self.validate_result = validate_result
        self.commit_msg = commit_msg
        self.generated_issues: list[Issue] = []

    async def _load(self, path: AsyncPath) -> Issue:
        return await Issue.load(path)

    async def _generate(self, issue: Issue, branch: GitBranch) -> bool:
        self.generated_issues.append(issue)
        return self.generate_result

    async def _validate(self, issue: Issue, branch: GitBranch) -> bool:
        return self.validate_result

    async def _make_git_commit_msg(
        self, issue: Issue, branch: GitBranch
    ) -> Iterator[str]:
        return iter(self.commit_msg)


class TestCodeEditor(unittest.IsolatedAsyncioTestCase, TestCaseMixin):
    """Unit tests for CodeEditor."""

    def setUp(self) -> None:
        self.setUpPyfakefs()
        self.working_dir = AsyncPath('/working')
        self.fs.create_dir(self.working_dir.path)

        self.mock_branch = MagicMock(spec=GitBranch)
        self.mock_branch.setup = AsyncMock(return_value=self.mock_branch)
        self.mock_branch.teardown = AsyncMock()
        self.mock_branch.reset = AsyncMock()
        self.mock_branch.add = AsyncMock()
        self.mock_branch.commit = AsyncMock()
        self.mock_branch.push = AsyncMock(return_value=12345)

        self.mock_dst_repo = MagicMock(spec=WritableGitWorkspace)
        self.mock_dst_repo.branch.return_value = self.mock_branch
        self.mock_dst_repo.project_dir = '/working/dst'

    async def test_edits_disabled_forwards_without_branch(self) -> None:
        """Tests disabled allow_edits skips edits and forwards."""
        editor = TestCodeEditorStage()
        next_stage = PipelineSink()
        editor.connect(next_stage)

        await configure_stage_for_test(
            editor,
            working_dir=str(self.working_dir),
            dst_repo=self.mock_dst_repo,
        )

        test_file = AsyncPath(self.working_dir, 'issue-1.json')
        issue = Issue(issue_id=101, title='Issue 1', description='Desc 1')
        await issue.save(test_file)

        run_task = asyncio.create_task(editor.run())
        await editor.input_queue.put(test_file)
        await editor.input_queue.put(None)
        await run_task

        self.mock_dst_repo.branch.assert_not_called()
        self.assertEqual(len(editor.generated_issues), 0)
        self.assertEqual(next_stage.input_queue.qsize(), 2)

    async def test_allow_edits_without_uploads(self) -> None:
        """Tests that allow_edits creates branch, commits, but does not push."""
        editor = TestCodeEditorStage()
        next_stage = PipelineSink()
        editor.connect(next_stage)

        await configure_stage_for_test(
            editor,
            working_dir=str(self.working_dir),
            dst_repo=self.mock_dst_repo,
            allow_edits=True,
            allow_uploads=False,
        )

        test_file = AsyncPath(self.working_dir, 'issue-1.json')
        issue = Issue(issue_id=101, title='Issue 1', description='Desc 1')
        await issue.save(test_file)

        run_task = asyncio.create_task(editor.run())
        await editor.input_queue.put(test_file)
        await editor.input_queue.put(None)
        await run_task

        self.mock_dst_repo.branch.assert_called_once_with(
            name='b101', keep=True
        )
        self.mock_branch.setup.assert_awaited_once()
        self.mock_branch.add.assert_awaited_once()
        self.mock_branch.commit.assert_awaited_once()
        self.mock_branch.push.assert_not_called()
        self.mock_branch.teardown.assert_awaited_once()

        forwarded = await next_stage.input_queue.get()
        assert forwarded is not None
        saved_issue = await Issue.load(forwarded)
        self.assertIsNone(saved_issue.cl_num)

    async def test_allow_uploads_pushes_cl(self) -> None:
        """Tests that allow_uploads pushes CL and updates cl_num."""
        editor = TestCodeEditorStage()
        next_stage = PipelineSink()
        editor.connect(next_stage)

        await configure_stage_for_test(
            editor,
            working_dir=str(self.working_dir),
            dst_repo=self.mock_dst_repo,
            allow_edits=True,
            allow_uploads=True,
        )

        test_file = AsyncPath(self.working_dir, 'issue-1.json')
        issue = Issue(issue_id=101, title='Issue 1', description='Desc 1')
        await issue.save(test_file)

        run_task = asyncio.create_task(editor.run())
        await editor.input_queue.put(test_file)
        await editor.input_queue.put(None)
        await run_task

        self.mock_dst_repo.branch.assert_called_once_with(
            name='b101', keep=False
        )
        self.mock_branch.push.assert_awaited_once()

        forwarded = await next_stage.input_queue.get()
        assert forwarded is not None
        saved_issue = await Issue.load(forwarded)
        self.assertEqual(saved_issue.cl_num, 12345)

    async def test_generate_failure_resets_branch(self) -> None:
        """Tests that generate failure resets branch and skips commit/push."""
        editor = TestCodeEditorStage(generate_result=False)
        next_stage = PipelineSink()
        editor.connect(next_stage)

        await configure_stage_for_test(
            editor,
            working_dir=str(self.working_dir),
            dst_repo=self.mock_dst_repo,
            allow_edits=True,
            allow_uploads=True,
        )

        test_file = AsyncPath(self.working_dir, 'issue-1.json')
        issue = Issue(issue_id=101, title='Issue 1', description='Desc 1')
        await issue.save(test_file)

        run_task = asyncio.create_task(editor.run())
        await editor.input_queue.put(test_file)
        await editor.input_queue.put(None)
        await run_task

        self.mock_branch.reset.assert_awaited_once_with(staged=True)
        self.mock_branch.commit.assert_not_called()
        self.mock_branch.push.assert_not_called()


if __name__ == '__main__':
    unittest.main()
