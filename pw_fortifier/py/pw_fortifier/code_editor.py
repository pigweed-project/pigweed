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
"""Defines the CodeEditor base class for pipeline stages modifying code."""

from abc import abstractmethod
import argparse
from typing import Iterator

from pw_fortifier.async_path import AsyncPath
from pw_fortifier.build_utils import run_unit_tests
from pw_fortifier.git_utils import GitBranch
from pw_fortifier.issue import Issue
from pw_fortifier.pipeline_stage import PipelineStage


class CodeEditor(PipelineStage):
    """Base class for pipeline stages that generate and upload code edits."""

    def __init__(self, name: str = 'code_editor') -> None:
        super().__init__(name)
        self._allow_edits: bool = False
        self._allow_uploads: bool = False
        self._combine_cls: bool = False
        self._branch: GitBranch | None = None

    async def configure(self, args: argparse.Namespace) -> None:
        """Configures the stage with command-line arguments.

        Args:
            args: Command-line arguments namespace object.
        """
        await super().configure(args)
        self._allow_edits = bool(args.allow_edits or args.allow_uploads)
        self._allow_uploads = bool(args.allow_uploads)

    async def _tear_down(self) -> None:
        """Cleans up any open branch when stage finishes."""
        await super()._tear_down()
        if self._branch is not None:
            await self._branch.teardown()
            self._branch = None

    async def _process_one(self, path: AsyncPath) -> None:
        """Processes an issue, generating edits and uploading CLs if enabled."""
        try:
            if not self._allow_edits:
                return

            issue = await self._load(path)
            if self._branch is None:
                assert self._dst_repo is not None
                branch_name = f'b{issue.issue_id}' if issue.issue_id else 'tmp'
                self._branch = self._dst_repo.branch(
                    name=branch_name,
                    keep=not self._allow_uploads,
                )
                await self._branch.setup()

            if not await self._generate(issue, self._branch):
                await self._branch.reset(staged=not self._combine_cls)
                return

            if not await self._validate(issue, self._branch):
                await self._branch.reset(staged=not self._combine_cls)
                return

            await self._branch.add()
            commit_msg = await self._make_git_commit_msg(issue, self._branch)
            await self._branch.commit('\n'.join(list(commit_msg)), amend=True)

            if self._allow_uploads:
                cl_num = await self._branch.push()
                if cl_num is not None:
                    issue = issue._replace(cl_num=cl_num)

            await issue.save(path)
        finally:
            if self._branch is not None and not self._combine_cls:
                await self._branch.teardown()
                self._branch = None
            await self._forward_one(path)

    async def _validate(self, issue: Issue, branch: GitBranch) -> bool:
        """Validates generated changes. Default implementation runs unit tests.

        Args:
            issue: The issue finding being resolved.
            branch: The GitBranch containing the candidate changes.

        Returns:
            True if validation succeeded; False otherwise.
        """
        del issue, branch
        assert self._dst_repo is not None
        failures = [
            t async for t in run_unit_tests(cwd=self._dst_repo.project_dir)
        ]
        return len(failures) == 0

    @abstractmethod
    async def _load(self, path: AsyncPath) -> Issue:
        """Loads and returns the concrete Issue subtype from path.

        Args:
            path: Path to the JSON issue file.

        Returns:
            The loaded Issue subclass instance.
        """
        raise NotImplementedError

    @abstractmethod
    async def _generate(self, issue: Issue, branch: GitBranch) -> bool:
        """Generates code modifications for the issue.

        Args:
            issue: The issue finding being resolved.
            branch: The active GitBranch where modifications are made.

        Returns:
            True if code modifications were generated; False otherwise.
        """
        raise NotImplementedError

    @abstractmethod
    async def _make_git_commit_msg(
        self, issue: Issue, branch: GitBranch
    ) -> Iterator[str]:
        """Generates the commit message lines for the changes.

        Args:
            issue: The issue finding being resolved.
            branch: The active GitBranch containing the modifications.

        Returns:
            An iterator of commit message string lines.
        """
        raise NotImplementedError
