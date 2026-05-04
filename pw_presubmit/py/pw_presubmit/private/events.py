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
"""Event interfaces for pw_presubmit."""

from __future__ import annotations

import abc
from collections.abc import Sequence
from pathlib import Path
from re import Pattern

import pw_cli.color
from pw_cli.plural import plural
from pw_cli.collect_files import file_summary
from pw_cli.git_repo import GitRepo, GitError, describe_git_pattern, RepoFiles
from pw_cli.tool_runner import BasicSubprocessRunner
from pw_presubmit.private import tools
from pw_presubmit.private.result import (
    PresubmitResult,
    ProgramResult,
)

_COLOR = pw_cli.color.colors()


def _truncate(text: str, width: int) -> str:
    if len(text) <= width:
        return text
    return text[: width - 1] + '…'


_RESOLVE_CONFLICTS = (
    'Resolve the conflicts, run `git rebase --continue` until '
    'the rebase is complete, then restart the presubmit run.'
)


_REBASE_WARNING = 'IMPORTANT: You are currently editing a commit during rebase!'


class PresubmitEvents(abc.ABC):
    """Abstract base class for presubmit event handlers."""

    @property
    @abc.abstractmethod
    def name(self) -> str:
        """Name of the UI."""

    @staticmethod
    def get(name: str) -> PresubmitEvents:
        """Looks up a PresubmitEvents implementation by name."""
        if name == 'boxes':
            return HumanUI()
        if name == 'minimal':
            return MinimalUI()
        raise KeyError(
            f'Unknown PresubmitEvents name: "{name}". '
            f'Valid options are "boxes" and "minimal".'
        )

    @abc.abstractmethod
    def program_start(
        self,
        program: str,
        all_checks: Sequence[str],
        selected_checks: Sequence[str],
        paths: Sequence[Path],
    ) -> None:
        """Called at the start of a presubmit program."""

    @abc.abstractmethod
    def collect_files(
        self,
        repos: Sequence[Path],
        base: str | None,
        pathspecs: Sequence[str],
        exclude: Sequence[Pattern[str]],
        root: Path,
        repo_files: RepoFiles,
    ) -> None:
        """Called after files are collected."""

    @abc.abstractmethod
    def warning(self, message: str) -> None:
        """Called with a warning message."""

    @abc.abstractmethod
    def auto_mode_started(self, base: str) -> None:
        """Called when starting auto mode."""

    @abc.abstractmethod
    def auto_mode_completed(self) -> None:
        """Called when auto mode completes successfully."""

    @abc.abstractmethod
    def auto_mode_conflict(self) -> None:
        """Called when auto mode hits a rebase conflict."""

    @abc.abstractmethod
    def auto_mode_failed(self, resume_file: Path) -> None:
        """Called when auto mode fails and writes a resume file."""

    @abc.abstractmethod
    def auto_mode_resuming(self) -> None:
        """Called when resuming an auto mode run."""

    @abc.abstractmethod
    def auto_mode_restarting(self) -> None:
        """Called when auto mode restarts due to fixes."""

    @abc.abstractmethod
    def fix_available(self, step_name: str) -> None:
        """Called when a fix is available for a step."""

    @abc.abstractmethod
    def fix_applied(self, step_name: str) -> None:
        """Called when a fix was successfully applied."""

    @abc.abstractmethod
    def step_start(
        self, check: str, step_count: int, paths: Sequence[Path]
    ) -> None:
        """Called at the start of a presubmit step."""

    @abc.abstractmethod
    def step_end(
        self,
        check: str,
        step_count: int,
        result: PresubmitResult,
        duration_s: float,
    ) -> None:
        """Called at the end of a presubmit step."""

    @abc.abstractmethod
    def program_completed(
        self,
        result: ProgramResult,
        duration_s: float,
    ) -> None:
        """Called with the final summary of the presubmit run."""


class HumanUI(PresubmitEvents):
    """The default terminal-based UI."""

    @property
    def name(self) -> str:
        return 'boxes'

    _SUMMARY_BOX = '══╦╗ ║║══╩╝'
    _CHECK_UPPER = '━━━┓       '
    _CHECK_LOWER = '       ━━━┛'
    _LEFT = 7
    _RIGHT = 11

    def __init__(self, width: int = 80):
        self.width = width
        self._center = self.width - self._LEFT - self._RIGHT - 4
        self.program: str | None = None
        self.checks: Sequence[str] = []
        self.paths: Sequence[Path] = []

    @staticmethod
    def _print(*args) -> None:
        print(*args, flush=True)

    def _box(self, style: str, left: str, middle: str, right: str) -> str:
        box = tools.make_box('><>')
        return box.format(
            *style,
            section1=left + ('' if left.endswith(' ') else ' '),
            width1=self._LEFT,
            section2=' ' + middle,
            width2=self._center,
            section3=right + ' ',
            width3=self._RIGHT,
        )

    def _padded_line(
        self, plain_text: str, colorized: str | None = None
    ) -> str:
        if colorized is None:
            colorized = plain_text

        content_width = self.width - 1
        padding = content_width - len(plain_text)
        left_padding = padding // 2
        right_padding = padding - left_padding
        return ' ' * left_padding + colorized + ' ' * right_padding + '║'

    def program_start(
        self,
        program: str,
        all_checks: Sequence[str],
        selected_checks: Sequence[str],
        paths: Sequence[Path],
    ) -> None:
        self.program = program
        self.checks = selected_checks
        self.paths = paths

        self._print()
        for line in file_summary(paths):
            self._print(line)
        self._print()

        content_width = self.width - 1
        try:
            repo = GitRepo(Path.cwd(), BasicSubprocessRunner())
            commit_name = _truncate(repo.name_rev(), content_width)
            commit_description = _truncate(
                repo.commit_message().split('\n', 1)[0], content_width
            )

            self._print('═' * content_width + '╗')
            self._print(self._padded_line(f' {program} presubmit checks '))
            self._print('─' * content_width + '╢')
            self._print(
                self._padded_line(commit_name, _COLOR.cyan(commit_name))
            )
            self._print(self._padded_line(''))
            self._print(self._padded_line(commit_description))
            self._print('═' * content_width + '╝')
        except GitError:
            self._print('═' * content_width + '╗')
            self._print(self._padded_line(f' {program} presubmit checks '))
            self._print('═' * content_width + '╝')

    def collect_files(
        self,
        repos: Sequence[Path],
        base: str | None,
        pathspecs: Sequence[str],
        exclude: Sequence[Pattern[str]],
        root: Path,
        repo_files: RepoFiles,
    ) -> None:
        for repo in repos:
            pattern_desc = describe_git_pattern(
                repo, base, pathspecs, exclude, BasicSubprocessRunner(), root
            )
            self._print(f'Checking {pattern_desc}')

    def warning(self, message: str) -> None:
        self._print(_COLOR.yellow(message))

    def auto_mode_started(self, base: str) -> None:
        self._print(f'🚀 Starting auto mode on {base}')

    def auto_mode_completed(self) -> None:
        self._print('✨ Auto mode completed successfully! ✨')

    def auto_mode_conflict(self) -> None:
        self._print(_COLOR.red('💥 Git rebase failed due to conflicts!'))
        self._print(_RESOLVE_CONFLICTS)
        self._print(_REBASE_WARNING)

    def auto_mode_failed(self, resume_file: Path) -> None:
        self._print(
            _COLOR.red(
                '❌ Presubmit failed on current commit. Stopping rebase.'
            )
        )
        self._print(
            f'Fix the issue, amend the commit, and call --resume {resume_file}'
        )
        self._print(_REBASE_WARNING)

    def auto_mode_resuming(self) -> None:
        self._print('🔄 Resuming presubmit run on current commit.')

    def auto_mode_restarting(self) -> None:
        self._print('🔁 Fixes occurred during run; restarting run.')

    def fix_available(self, step_name: str) -> None:
        self._print(
            f'💡 Automatic fix available for {_COLOR.cyan(step_name)}; '
            f'run with --fix to apply'
        )

    def fix_applied(self, step_name: str) -> None:
        self._print(f'🤖 Applied automatic fix for {step_name} 🛠️')

    def step_start(
        self, check: str, step_count: int, paths: Sequence[Path]
    ) -> None:
        total = len(self.checks) if self.checks else 0
        num_paths = len(paths)
        middle_text = f'{step_count}/{total}'
        self._print(
            self._box(
                self._CHECK_UPPER,
                middle_text,
                check,
                plural(num_paths, 'file'),
            )
        )

    def step_end(
        self,
        check: str,
        step_count: int,
        result: PresubmitResult,
        duration_s: float,
    ) -> None:
        time_str = tools.format_time(duration_s)
        self._print(
            self._box(
                self._CHECK_LOWER,
                result.colorized(self._LEFT),
                check,
                time_str,
            )
        )

    def program_completed(
        self,
        result: ProgramResult,
        duration_s: float,
    ) -> None:
        self._print(
            self._box(
                self._SUMMARY_BOX,
                result.result.colorized(self._LEFT, invert=True),
                f'{result.total} checks on '
                f'{plural(len(self.paths), "file")}: {result.message()}',
                tools.format_time(duration_s),
            )
        )


class MinimalUI(PresubmitEvents):
    """A minimal, Markdown-inspired UI optimized for LLMs."""

    @property
    def name(self) -> str:
        return 'minimal'

    def __init__(self):
        self.checks: Sequence[str] = []
        self.paths: Sequence[Path] = []
        self._program_run_count = 0
        self._total_commits = 0
        self._in_auto_mode = False

    def program_start(
        self,
        program: str,
        all_checks: Sequence[str],
        selected_checks: Sequence[str],
        paths: Sequence[Path],
    ) -> None:
        self.checks = selected_checks
        self.paths = paths

        self._program_run_count += 1
        if self._in_auto_mode:
            count = f'[{self._program_run_count}/{self._total_commits}] '
        else:
            count = ''

        print(f'## {count}Presubmit program')
        print(f'- Name: {program}')
        print(f'- Files: {len(paths)}')
        print(f'- Steps: {len(selected_checks)}')
        print()
        print('### Steps')
        print()

    def collect_files(
        self,
        repos: Sequence[Path],
        base: str | None,
        pathspecs: Sequence[str],
        exclude: Sequence[Pattern[str]],
        root: Path,
        repo_files: RepoFiles,
    ) -> None:
        pass

    def warning(self, message: str) -> None:
        print('> [!WARNING]')
        print(f'> {message}')
        print()

    def auto_mode_started(self, base: str) -> None:
        self._in_auto_mode = True
        repo = GitRepo(Path.cwd(), BasicSubprocessRunner())
        self._total_commits = repo.commit_count(base, 'HEAD')
        print('# Auto presubmit start')
        print(
            f'- Running on {plural(self._total_commits, "commit")} '
            'commits since {base}'
        )
        print('- Fixes are automatically applied when possible.')
        print('- The presubmit may be resumed if manual fixes are required.')
        print()

    def auto_mode_completed(self) -> None:
        print('# Auto presubmit completed')
        print('- Result: PASS')

    def auto_mode_conflict(self) -> None:
        print('## Auto presubmit rebase conflict')
        print(_RESOLVE_CONFLICTS)
        print(_REBASE_WARNING)

    def auto_mode_failed(self, resume_file: Path) -> None:
        print('## Auto presubmit failed')
        print(
            '- Simple issues: Fix, amend the commit, and resume the presubmit '
            f'checks by running this script with `--resume {resume_file}`'
        )
        print('- Complex issues: Seek guidance about how to proceed.')
        print()
        print(_REBASE_WARNING)

    def auto_mode_resuming(self) -> None:
        pass

    def auto_mode_restarting(self) -> None:
        pass

    def fix_available(self, step_name: str) -> None:
        print(f'- **Fix available** for `{step_name}`')

    def fix_applied(self, step_name: str) -> None:
        print(f'- Applied automatic fix for `{step_name}`')

    def step_start(
        self, check: str, step_count: int, paths: Sequence[Path]
    ) -> None:
        print(f'#### ({step_count}/{len(self.checks)}) {check}')

    def step_end(
        self,
        check: str,
        step_count: int,
        result: PresubmitResult,
        duration_s: float,
    ) -> None:
        print(f'- Result: {result.name}')
        print()

    def program_completed(
        self,
        result: ProgramResult,
        duration_s: float,
    ) -> None:
        print('### Program completed')
        print(f'- Result: {result.result.name}')
        print(f'- Passed: {len(result.passed)}')
        print()
