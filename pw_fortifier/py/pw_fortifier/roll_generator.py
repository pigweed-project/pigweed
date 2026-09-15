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
"""Defines the RollGenerator base class and stub."""

import asyncio
import json
import logging
import os
from pathlib import Path
import subprocess
from typing import Iterator

from pw_fortifier.async_path import AsyncPath
from pw_fortifier.code_editor import CodeEditor
from pw_fortifier.freshness_result import FreshnessResult
from pw_fortifier.git_utils import GitBranch, make_git_commit_msg
from pw_fortifier.issue import Issue
from pw_fortifier.package_updater import PackageUpdater


class RollGenerator(CodeEditor):
    """Base class for stages that generate dependency roll commits."""

    def __init__(self, name: str = 'roll_generator') -> None:
        super().__init__(name)
        self._combine_cls: bool = True
        self._first_commit: bool = True
        self._pkg_registry: dict[str, PackageUpdater] = {}
        self._target_registry: dict[str, PackageUpdater] = {}
        self._env: dict[str, str] | None = None

    def register(self, updater: PackageUpdater) -> None:
        """Registers a package updater by package type or target filename.

        Args:
            updater: PackageUpdater instance to register.
        """
        assert not (updater.PKG_TYPE is None and updater.TARGET is None)
        if updater.PKG_TYPE is not None:
            self._pkg_registry[updater.PKG_TYPE] = updater
        if updater.TARGET is not None:
            self._target_registry[updater.TARGET] = updater

    async def _set_up(self) -> None:
        """Bootstraps the destination repo to initialize the environment."""
        await super()._set_up()
        if self._dst_repo is None:
            return
        cmd = (
            'source ./bootstrap.sh >&2 && '
            'python3 -c "import json, os; print(json.dumps(dict(os.environ)))"'
        )
        proc = await asyncio.create_subprocess_shell(
            cmd,
            cwd=str(self._dst_repo.project_dir),
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
            executable='/bin/bash',
        )
        stdout, stderr = await proc.communicate()
        if proc.returncode != 0:
            assert proc.returncode is not None
            raise subprocess.CalledProcessError(
                proc.returncode, cmd, output=stdout, stderr=stderr
            )
        self._env = json.loads(stdout.decode().strip())

    async def _load(self, path: AsyncPath) -> FreshnessResult:
        """Loads a FreshnessResult from the JSON path."""
        return await FreshnessResult.load(path)

    async def _generate(self, issue: Issue, branch: GitBranch) -> bool:
        """Generates a roll for the freshness finding."""
        assert isinstance(issue, FreshnessResult)
        return await self._generate_roll(issue)

    async def _validate(self, issue: Issue, branch: GitBranch) -> bool:
        """Validation is performed by package updaters during generation."""
        del issue, branch
        return True

    async def _make_git_commit_msg(
        self, issue: Issue, branch: GitBranch
    ) -> Iterator[str]:
        """Generates or amends the roll commit message."""
        assert isinstance(issue, FreshnessResult)
        update = f'{issue.package} -> {issue.earliest.version}'
        issue_id = issue.issue_id
        if self._first_commit:
            self._first_commit = False
            title = 'roll: Update 3p deps'
            return make_git_commit_msg(title, update, issue_id)

        commit_msg = await branch.commit_msg()
        last_blank_idx = None
        for i in range(len(commit_msg) - 1, -1, -1):
            if not commit_msg[i].strip():
                last_blank_idx = i

        # No blank line indicates the ChangeID git-hook is missing.
        assert last_blank_idx is not None
        commit_msg.insert(last_blank_idx, update)
        if issue_id:
            commit_msg.append(f'Bug: {issue_id}')
        return iter(commit_msg)

    async def _generate_roll(self, result: FreshnessResult) -> bool:
        """Generates roll changes for the package dependency.

        Args:
            result: The freshness result finding containing dependency details.

        Returns:
            True if dependency was updated and presubmit succeeded; False
            otherwise.
        """
        assert self._dst_repo is not None

        # Check for a registered updater by pkg_type or target source
        updater = self._pkg_registry.get(result.pkg_type)
        if updater is None:
            updater = self._target_registry.get(result.source)
        if updater is not None:
            return await updater.update(self._dst_repo, result)

        # Fallback to simply updating the 3p reference
        project_dir = self._dst_repo.project_dir
        target_file = Path(project_dir, result.source)
        if not target_file.is_file():
            logging.warning('Target file %s does not exist', target_file)
            return False

        content = target_file.read_text()
        if result.current.version not in content:
            logging.warning(
                'Version %s not found in %s',
                result.current.version,
                target_file,
            )
            return False

        new_content = content.replace(
            result.current.version,
            result.earliest.version,
        )
        target_file.write_text(new_content)

        # Attempt to build
        pw_script = Path(project_dir, 'pw')
        env = self._env if self._env is not None else os.environ.copy()

        candidates: list[list[str]] = []
        if pw_script.is_file():
            candidates.append([str(pw_script), 'presubmit'])
        candidates.append(['pw', 'presubmit'])

        for cmd in candidates:
            try:
                proc = await asyncio.create_subprocess_exec(
                    *cmd,
                    cwd=str(project_dir),
                    env=env,
                    stdout=asyncio.subprocess.PIPE,
                    stderr=asyncio.subprocess.PIPE,
                )
                await proc.communicate()
                return proc.returncode == 0
            except FileNotFoundError:
                if cmd is candidates[-1]:
                    logging.error('No presubmit checks could be run')
                    raise

        return False

    async def _tear_down(self) -> None:
        """Tears down the branch and cleans up stage."""
        if self._branch is not None:
            await self._branch.teardown()
            self._branch = None
        await super()._tear_down()


################################################################################
# Test support


class RollGeneratorStub(RollGenerator):
    """A stub implementation of RollGenerator for testing."""

    def __init__(
        self,
        generate_return_value: bool = True,
        name: str = 'roll_generator_stub',
    ) -> None:
        super().__init__(name)
        self.generate_return_value = generate_return_value
        self.passed_results: list[FreshnessResult] = []

    async def _set_up(self) -> None:
        pass

    async def _generate_roll(self, result: FreshnessResult) -> bool:
        """Simulates roll generation."""
        self.passed_results.append(result)
        return self.generate_return_value
