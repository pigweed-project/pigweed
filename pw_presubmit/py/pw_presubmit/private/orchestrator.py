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
"""Orchestrator for running presubmit checks."""

from __future__ import annotations

import logging
import json
import os
import shutil
from pathlib import Path
import re
import contextlib
import collections
import enum
import tempfile
import time
from typing import Iterable, Sequence, Mapping, Callable

from pw_cli import git_repo
from pw_cli.file_filter import FileFilter
from pw_cli.tool_runner import BasicSubprocessRunner
from pw_presubmit.private.events import PresubmitEvents
from pw_presubmit.private.result import (
    PresubmitFailure,
    PresubmitResult,
    ProgramResult,
)
from pw_presubmit.private.step import Context, Step, FilteredStep
from pw_presubmit.private.tools import relative_paths

_LOG = logging.getLogger("pw_presubmit")


class Mode(enum.Enum):
    STOP = 'stop'  # Stop on errors
    CONTINUE = 'continue'  # Continue past errors
    FIX = 'fix'  # Attempt to fix errors
    AUTO = 'auto'  # Run on all commits, fixing errors, and amend

    def __str__(self) -> str:
        return self.value


class AutoModeError(Exception):
    """Exception raised for errors during auto mode execution."""


class Orchestrator:
    """Manages the execution of presubmit checks.

    This class coordinates running a set of checks against a set of files,
    handling filtering, output directory management, and reporting results
    via events.

    It also supports automatically applying fixes if the check provides them
    and the user requested it.

    This class is a streamlined alternative to the older `Presubmit` runner.
    It simplifies the execution flow and avoids heavy dependencies, while
    maintaining compatibility with existing check logic.
    """

    def __init__(
        self,
        root: Path,
        output_dir: Path,
        paths: Sequence[Path],
        all_paths: Sequence[Path],
        events: PresubmitEvents,
    ) -> None:
        """Initializes the orchestrator.

        Args:
            root: The root directory of the project.
            output_dir: The directory where output files will be written.
            paths: Selected / modified files.
            all_paths: All files that the presubmit checks apply to.
            events: Event handler for reporting progress and results.
        """
        self._root = root.resolve()
        self._output_dir = output_dir.resolve()
        self._paths = tuple(paths)
        self._all_paths = tuple(all_paths)
        self._relative_paths = tuple(relative_paths(self._paths, self._root))
        self._events = events

    def run(
        self,
        name: str,
        steps: Iterable[Step],
        *,
        mode: Mode = Mode.STOP,
    ) -> ProgramResult:
        """Executes a series of presubmit checks."""
        # Group steps by their FileFilter to avoid redundant evaluations.
        filter_to_steps = collections.defaultdict(list)
        for s in steps:
            filter_to_steps[s.filter].append(s)

        checks_to_paths = {}
        for filt, steps_with_filt in filter_to_steps.items():
            rel_paths = filt.filter(self._relative_paths)
            abs_paths = tuple(self._root / p for p in rel_paths)
            for s in steps_with_filt:
                checks_to_paths[s] = abs_paths

        # Reconstruct all_steps in original order.
        all_steps = [FilteredStep(s, checks_to_paths[s]) for s in steps]

        filtered_steps = tuple(s for s in all_steps if s.paths)

        _LOG.debug(
            'Running %d of %d steps', len(filtered_steps), len(all_steps)
        )

        self._events.program_start(
            name,
            [s.name for s in all_steps],
            [s.name for s in filtered_steps],
            self._relative_paths,
        )

        passed: list[FilteredStep] = []
        failed: list[FilteredStep] = []
        fixed: list[FilteredStep] = []
        skipped: list[FilteredStep] = []

        start_time = time.time()

        for i, step in enumerate(filtered_steps, 1):
            try:
                result = self._run_step(step, i, mode=mode)
            except RuntimeError as e:
                _LOG.critical('%s', e)
                failed.append(step)
                skipped = list(filtered_steps[i:])
                break

            if result is PresubmitResult.PASS:
                passed.append(step)
            elif result is PresubmitResult.FIXED:
                fixed.append(step)
            elif result is PresubmitResult.CANCEL:
                skipped = list(filtered_steps[i:])
                break
            else:
                failed.append(step)
                if mode is not Mode.CONTINUE:
                    skipped = list(filtered_steps[i:])
                    break

        duration = time.time() - start_time

        program_result = ProgramResult(
            passed=[s.name for s in passed],
            failed=[s.name for s in failed],
            skipped=[s.name for s in skipped],
            fixed=[s.name for s in fixed],
            success=not failed and (not fixed or mode is Mode.AUTO),
        )
        self._events.program_completed(program_result, duration)

        return program_result

    def _run_step(
        self, step: FilteredStep, index: int, mode: Mode
    ) -> PresubmitResult:
        """Runs a single check step, applying fixes if supported."""
        self._events.step_start(step.name, index, step.paths)
        start_time = time.time()

        result = self._execute_action(step, 'run', step.step.run)
        if result is PresubmitResult.FAIL:
            result = self._attempt_fix(step, fix=mode in (Mode.FIX, Mode.AUTO))
            if result is PresubmitResult.FIXED and mode is Mode.AUTO:
                _LOG.debug('Amending HEAD commit with fixes')
                repo = git_repo.GitRepo(self._root, BasicSubprocessRunner())
                repo.modify().amend_commit_with_updated_files()

        duration = time.time() - start_time
        self._events.step_end(step.name, index, result, duration)

        return result

    @contextlib.contextmanager
    def _context(self, step: FilteredStep):
        """Creates a context and manages its output directory."""
        sanitized_name = re.sub(r'[\W_]+', '_', step.name).lower()
        step_output_dir = self._output_dir / sanitized_name

        shutil.rmtree(step_output_dir, ignore_errors=True)
        os.makedirs(step_output_dir)

        ctx = Context(
            root=self._root,
            output_dir=step_output_dir,
            paths=tuple(step.paths),
            all_paths=self._all_paths,
        )

        try:
            yield ctx
        finally:
            if not ctx.failed:
                shutil.rmtree(step_output_dir, ignore_errors=True)

    def _execute_action(
        self, step: FilteredStep, name: str, action: Callable[[Context], None]
    ) -> PresubmitResult:
        """Executes Step.run or Step.fix with a Context and handles
        exceptions.
        """
        with self._context(step) as ctx:
            try:
                action(ctx)
                if ctx.failed:
                    return PresubmitResult.FAIL
                return PresubmitResult.PASS
            except NotImplementedError:
                return PresubmitResult.FAIL
            except PresubmitFailure as failure:
                if str(failure):
                    _LOG.warning('%s', failure)
                return PresubmitResult.FAIL
            except Exception as e:  # pylint: disable=broad-except
                ctx.fail(
                    f'Step {name} failed unexpectedly with an exception',
                    exception=e,
                )
                return PresubmitResult.FAIL
            except KeyboardInterrupt:
                print()
                return PresubmitResult.CANCEL

    def _attempt_fix(
        self,
        step: FilteredStep,
        fix: bool,
    ) -> PresubmitResult:
        """Attempts to fix failures."""
        if not fix:
            if type(step.step).fix is not Step.fix:
                _LOG.debug('Automatic fix available for %s', step.name)
                self._events.fix_available(step.name)
            return PresubmitResult.FAIL

        if type(step.step).fix is Step.fix:
            return PresubmitResult.FAIL

        _LOG.debug('Applying fix for %s', step.name)

        result = self._execute_action(step, 'fix', step.step.fix)
        if result is PresubmitResult.FAIL:
            return PresubmitResult.FAIL

        result = self._execute_action(step, 'run', step.step.run)

        if result is PresubmitResult.PASS:
            _LOG.debug('Applied automatic fix for %s', step.name)
            self._events.fix_applied(step.name)
            return PresubmitResult.FIXED

        raise RuntimeError(
            f'{step.name} failed after running its fix() implementation! '
            f'This is invalid; the fix implementation is broken. '
            f'Please review the fix() method for {step.name} and ensure it '
            f'correctly addresses the failures detected by run().'
        )


def run(
    name: str,
    steps: Iterable[Step],
    root: Path,
    events: PresubmitEvents,
    base: str,
    *,
    repos: Iterable[Path] | None = None,
    pathspecs: Iterable[str] = (),
    output_dir: Path | None = None,
    file_filter: FileFilter = FileFilter(),
    mode: Mode = Mode.STOP,
) -> ProgramResult:
    """Simplifies running presubmit checks on one or more repos.

    This collects files from the provided git repositorites, instantiates an
    Orchestrator, and runs the presubmit.
    """
    if repos is None:
        repos = [root]

    with contextlib.ExitStack() as stack:
        if output_dir is None:
            output_dir = Path(
                stack.enter_context(tempfile.TemporaryDirectory())
            )

        repo_files = git_repo.collect_files(
            repos=repos,
            pathspecs=pathspecs,
            base=base,
            file_filter=file_filter,
            root=root,
            tool_runner=BasicSubprocessRunner(),
        )

        events.collect_files(
            repos=tuple(repos),
            base=base,
            pathspecs=tuple(pathspecs),
            exclude=tuple(file_filter.exclude),
            root=root,
            repo_files=repo_files,
        )

        orchestrator = Orchestrator(
            root=root,
            output_dir=output_dir,
            paths=repo_files.modified_paths,
            all_paths=repo_files.paths,
            events=events,
        )

        return orchestrator.run(name, steps, mode=mode)


def _auto_loop(
    name: str,
    steps: Iterable[Step],
    root: Path,
    events: PresubmitEvents,
    repo: git_repo.GitRepo,
    base: str,
    pathspecs: Iterable[str],
    output_dir: Path,
    file_filter: FileFilter,
) -> tuple[bool, bool]:
    """Internal loop for auto mode."""
    fixes_occurred = False
    steps = tuple(steps)
    step_names = [s.name for s in steps]

    while True:
        result = run(
            name=name,
            steps=steps,
            root=root,
            events=events,
            base='HEAD~',
            pathspecs=pathspecs,
            output_dir=output_dir,
            file_filter=file_filter,
            mode=Mode.AUTO,
        )

        if result.fixed:
            fixes_occurred = True

        if not result.success:
            _LOG.debug('Presubmit failed on current commit; stopping rebase')

            resume_file = output_dir / 'resume.json'
            info = repo.rebase_info()
            if not info:
                raise AutoModeError(
                    'Failed to read rebase information. '
                    'Please ensure you are in a valid git rebase state. '
                    'You may need to abort the rebase with '
                    '`git rebase --abort` and try again.'
                )
            state = {
                'name': name,
                'root': str(root),
                'base': base,
                'pathspecs': list(pathspecs),
                'output_dir': str(output_dir),
                'step_names': step_names,
                'rebase_onto': info.onto,
                'rebase_orig_head': info.orig_head,
                'event_handler': events.name,
            }
            try:
                with resume_file.open('w') as f:
                    json.dump(state, f, indent=2)
            except OSError as e:
                _LOG.error('Failed to write resume file: %s', e)

            events.auto_mode_failed(resume_file)

            return False, fixes_occurred

        if repo.has_uncommitted_changes():
            raise AutoModeError(
                'Working tree is not clean after successful run! '
                'This implies the presubmit modified files without amending '
                'or committing them. Please commit or stash these changes '
                'before continuing.'
            )

        try:
            repo.modify().rebase_continue()
            if not repo.is_in_rebase():
                _LOG.debug('Rebase completed successfully!')
                events.auto_mode_completed()
                break
        except git_repo.GitError as e:
            if repo.is_in_rebase():
                _LOG.debug('git rebase --continue failed due to conflicts')
                _LOG.debug(
                    'Resolve the merge conflicts, run '
                    '`git rebase --continue`, then restart the presubmit run'
                )
                events.auto_mode_conflict()
                return False, fixes_occurred

            raise AutoModeError(
                '`git rebase --continue` failed for an unknown reason. '
                'Please check the output of `git status` and resolve any '
                'issues manually.'
            ) from e

    return True, fixes_occurred


def auto(
    name: str,
    steps: Iterable[Step],
    root: Path,
    events: PresubmitEvents,
    base: str,
    *,
    pathspecs: Iterable[str] = (),
    output_dir: Path | None = None,
    file_filter: FileFilter = FileFilter(),
) -> bool:
    """Runs presubmit in auto mode (rebase and fix)."""
    repo = git_repo.GitRepo(root, BasicSubprocessRunner())
    if repo.is_in_rebase():
        raise AutoModeError(
            "You can't start auto mode during a rebase. "
            "If you want to run on a higher commit in the stack, "
            "create a new branch."
        )

    if repo.has_uncommitted_changes():
        raise AutoModeError(
            "Git working tree has uncommitted changes. "
            "Auto mode requires a clean working tree to safely rebase "
            "and amend commits. "
            "Please commit or stash your changes before running."
        )

    if output_dir is None:
        output_dir = Path(tempfile.mkdtemp(prefix='pw_presubmit_auto_'))
        _LOG.debug('Created output directory: %s', output_dir)

    _LOG.debug('Starting interactive rebase on %s', base)
    events.auto_mode_started(base)
    try:
        repo.modify().rebase_interactive(base)
    except git_repo.GitError as e:
        raise AutoModeError(
            f'Failed to start interactive rebase onto {base}. '
            f'Please ensure the base commit exists and you have no '
            f'uncommitted changes.'
        ) from e

    success, fixes_occurred = _auto_loop(
        name=name,
        steps=steps,
        root=root,
        events=events,
        repo=repo,
        base=base,
        pathspecs=pathspecs,
        output_dir=output_dir,
        file_filter=file_filter,
    )

    if success and fixes_occurred:
        _LOG.debug("Fixes occurred during run. Restarting from beginning.")
        events.auto_mode_restarting()
        return auto(
            name=name,
            steps=steps,
            root=root,
            events=events,
            base=base,
            pathspecs=pathspecs,
            output_dir=output_dir,
            file_filter=file_filter,
        )

    return success


def resume(
    all_steps: Mapping[str, Step],
    resume_file: Path,
    file_filter: FileFilter = FileFilter(),
) -> bool:
    """Resumes a failed auto mode run."""
    try:
        with resume_file.open() as f:
            state = json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        raise AutoModeError(
            f'Failed to load resume file: {e}. '
            f'The file might be corrupted. You can delete it and restart '
            f'the presubmit run.'
        ) from e

    try:
        name = state['name']
        root = Path(state['root'])
        base = state['base']
        pathspecs = state['pathspecs']
        output_dir = Path(state['output_dir'])
        saved_step_names = state['step_names']
        events = PresubmitEvents.get(state['event_handler'])
    except KeyError as e:
        raise AutoModeError(
            f'Resume file is missing required key: {e}. '
            f'The file format may be incompatible. You can delete it and '
            f'restart the presubmit run.'
        ) from e

    try:
        steps = [all_steps[name] for name in saved_step_names]
    except KeyError as e:
        raise AutoModeError(
            f'Unknown step in resume file: {e}. '
            f'The step might have been removed or renamed. You can delete '
            f'the resume file and restart.'
        ) from e

    repo = git_repo.GitRepo(root, BasicSubprocessRunner())
    if not repo.is_in_rebase():
        raise AutoModeError(
            "--resume requires the git repository to be in a rebase state, but "
            "it is not. This command should only be used to resume a failed "
            "auto mode run that left the repo in a rebase state."
        )

    info = repo.rebase_info()
    if not info:
        raise AutoModeError(
            'Failed to read rebase information. '
            'Please ensure you are in a valid git rebase state. '
            'You may need to abort the rebase with `git rebase --abort` '
            'and try again.'
        )
    current_onto = info.onto
    current_orig_head = info.orig_head

    if current_onto != state.get(
        'rebase_onto', ''
    ) or current_orig_head != state.get('rebase_orig_head', ''):
        raise AutoModeError(
            "The current rebase state does not match the saved state.\n"
            f"  Expected: onto={state.get('rebase_onto')}, "
            f"orig_head={state.get('rebase_orig_head')}\n"
            f"  Actual:   onto={current_onto}, "
            f"orig_head={current_orig_head}\n"
            "Please ensure you are resuming the correct rebase or delete the "
            "resume file to start over."
        )

    if repo.has_uncommitted_changes():
        raise AutoModeError(
            "Git working tree has uncommitted changes. "
            "Resuming auto mode requires a clean working tree to safely rebase "
            "and amend commits. "
            "Please amend or otherwise resolve changes before running."
        )

    _LOG.debug('Resuming presubmit run on current commit.')
    events.auto_mode_resuming()

    success, _ = _auto_loop(
        name=name,
        steps=steps,
        root=root,
        events=events,
        repo=repo,
        base=base,
        pathspecs=pathspecs,
        output_dir=output_dir,
        file_filter=file_filter,
    )

    if success:
        _LOG.debug('Resume completed successfully. Restarting from beginning.')
        events.auto_mode_restarting()
        return auto(
            name=name,
            steps=steps,
            root=root,
            events=events,
            base=base,
            pathspecs=pathspecs,
            output_dir=output_dir,
            file_filter=file_filter,
        )

    return False
