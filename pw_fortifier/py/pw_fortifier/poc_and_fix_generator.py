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
"""Defines the PocAndFixGenerator base class and stub."""

from abc import abstractmethod
from typing import Iterator

from pw_fortifier.async_path import AsyncPath
from pw_fortifier.build_utils import run_unit_tests
from pw_fortifier.code_editor import CodeEditor
from pw_fortifier.defect import Defect
from pw_fortifier.git_utils import GitBranch, make_git_commit_msg
from pw_fortifier.issue import Issue


class PocAndFixGenerator(CodeEditor):
    """Base class for stages generating Proof of Concept tests and fixes."""

    def __init__(self, name: str = 'poc_and_fix_generator') -> None:
        super().__init__(name)
        self._current_poc_test: str | None = None
        self._current_fix_ok: bool = False

    async def _load(self, path: AsyncPath) -> Defect:
        """Loads a Defect from the JSON path."""
        return await Defect.load(path)

    async def _generate(self, issue: Issue, branch: GitBranch) -> bool:
        """Generates PoC test and code fix for the defect."""
        assert isinstance(issue, Defect)
        self._current_poc_test = await self._generate_poc(issue)
        self._current_fix_ok = await self._generate_fix(issue)
        if self._current_poc_test is None and not self._current_fix_ok:
            return False
        return True

    async def _validate(self, issue: Issue, branch: GitBranch) -> bool:
        """Validates unit tests according to whether fix was generated."""
        assert self._dst_repo is not None
        failures = [
            t async for t in run_unit_tests(cwd=self._dst_repo.project_dir)
        ]
        if not self._current_fix_ok:
            return self._current_poc_test is not None and failures == [
                self._current_poc_test
            ]
        return len(failures) == 0

    async def _make_git_commit_msg(
        self, issue: Issue, branch: GitBranch
    ) -> Iterator[str]:
        """Summarizes staged diffs into a git commit message."""
        assert isinstance(issue, Defect)
        diffs = await branch.diff(staged=True)
        (title, desc) = await self._summarize(issue, diffs)
        return make_git_commit_msg(title, desc, issue.issue_id)

    @abstractmethod
    async def _generate_poc(self, defect: Defect) -> str | None:
        """Generates a PoC unit test triggering the defect.

        Args:
            defect: The defect finding.

        Returns:
            The name of the generated PoC test target, or None on failure.
        """
        raise NotImplementedError

    @abstractmethod
    async def _generate_fix(self, defect: Defect) -> bool:
        """Generates a code fix resolving the defect.

        Args:
            defect: The defect finding.

        Returns:
            True if fix was generated; False otherwise.
        """
        raise NotImplementedError

    @abstractmethod
    async def _summarize(
        self, defect: Defect, diffs: list[str]
    ) -> tuple[str, str]:
        """Summarizes the fix into a commit title and description."""
        raise NotImplementedError


################################################################################
# Test support


class PocAndFixGeneratorStub(PocAndFixGenerator):
    """A stub implementation of PocAndFixGenerator for testing."""

    def __init__(
        self,
        poc_result: str | bool | None = '//pw_defect:poc_test',
        fix_result: bool = True,
        summary: tuple[str, str] = ('Commit Title', 'Commit Description'),
        name: str = 'poc_and_fix_generator_stub',
    ) -> None:
        super().__init__(name)
        if poc_result is True:
            self.poc_result: str | None = '//pw_defect:poc_test'
        elif poc_result is False:
            self.poc_result = None
        else:
            self.poc_result = poc_result
        self.fix_result = fix_result
        self.summary = summary
        self.passed_defects: list[Defect] = []

    async def _generate_poc(self, defect: Defect) -> str | None:
        """Simulates PoC generation."""
        self.passed_defects.append(defect)
        return self.poc_result

    async def _generate_fix(self, defect: Defect) -> bool:
        """Simulates fix generation."""
        return self.fix_result

    async def _validate(self, issue: Issue, branch: GitBranch) -> bool:
        """Simulates validation for testing."""
        return True

    async def _summarize(
        self, defect: Defect, diffs: list[str]
    ) -> tuple[str, str]:
        """Returns configured commit title and description."""
        return self.summary
