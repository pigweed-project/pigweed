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
"""Tests for orchestrator."""

import tempfile
import json
from pathlib import Path
import unittest
import subprocess
from unittest import mock

from pw_presubmit.private.events import PresubmitEvents
from pw_presubmit.private import orchestrator
from pw_presubmit.private.orchestrator import (
    Orchestrator,
    run,
    Mode,
    auto,
    AutoModeError,
    resume,
)
from pw_presubmit.private.result import PresubmitResult
from pw_presubmit.private.step import Step, Context
from pw_presubmit.private.result import ProgramResult

from pw_cli.file_filter import FileFilter
from pw_cli import git_repo
from pw_cli.git_repo import RepoFiles, GitError


class PassStep(Step):
    def run(self, ctx: Context) -> None:
        pass


class FailStep(Step):
    def run(self, ctx: Context) -> None:
        ctx.fail('failure')


class FixableStep(Step):
    def __init__(self) -> None:
        super().__init__()
        self.run_count = 0

    def run(self, ctx: Context) -> None:
        self.run_count += 1
        if self.run_count == 1:
            ctx.fail('needs fix')

    def fix(self, ctx: Context) -> None:
        pass


class OrchestratorTest(unittest.TestCase):
    """Tests for Orchestrator."""

    def setUp(self) -> None:
        self.tmp_dir = tempfile.TemporaryDirectory()
        self.output_dir = Path(self.tmp_dir.name)
        self.events = mock.Mock(spec=PresubmitEvents)
        self.events.name = 'boxes'

    def tearDown(self) -> None:
        self.tmp_dir.cleanup()

    def test_run_zero_checks(self) -> None:
        orch = Orchestrator(
            root=Path('.'),
            output_dir=self.output_dir,
            paths=(),
            all_paths=(),
            events=self.events,
        )
        result = orch.run('Presubmit', [])
        self.assertTrue(result.success)

    def test_run_multiple_checks(self) -> None:
        orch = Orchestrator(
            root=Path('.'),
            output_dir=self.output_dir,
            paths=(Path('file.cc'),),
            all_paths=(Path('file.cc'),),
            events=self.events,
        )
        result = orch.run('Presubmit', [PassStep(), PassStep()])
        self.assertTrue(result.success)

    def test_run_failure(self) -> None:
        orch = Orchestrator(
            root=Path('.'),
            output_dir=self.output_dir,
            paths=(Path('file.cc'),),
            all_paths=(Path('file.cc'),),
            events=self.events,
        )
        result = orch.run('Presubmit', [FailStep()])
        self.assertFalse(result.success)

    def test_fix_and_rerun(self) -> None:
        fixable = FixableStep()
        orch = Orchestrator(
            root=Path('.'),
            output_dir=self.output_dir,
            paths=(Path('file.cc'),),
            all_paths=(Path('file.cc'),),
            events=self.events,
        )
        result = orch.run('Presubmit', [fixable], mode=Mode.FIX)
        self.assertFalse(result.success)
        self.assertEqual(result.result, PresubmitResult.FIXED)
        self.assertEqual(fixable.run_count, 2)
        self.events.fix_applied.assert_called_once_with('fixable_step')

    def test_filtering(self) -> None:
        class PyStep(Step):
            def __init__(self) -> None:
                super().__init__(file_filter=FileFilter(endswith='.py'))
                self.run_called = False

            def run(self, ctx: Context) -> None:
                self.run_called = True

        pystep = PyStep()
        orch = Orchestrator(
            root=Path('.'),
            output_dir=self.output_dir,
            paths=(Path('file.cc'),),
            all_paths=(Path('file.cc'),),
            events=self.events,
        )
        result = orch.run('Presubmit', [pystep])
        self.assertTrue(result.success)
        self.assertFalse(pystep.run_called)

    def test_run_calls_events(self) -> None:
        """Test that Orchestrator.run calls event methods."""
        orch = Orchestrator(
            root=Path('.'),
            output_dir=self.output_dir,
            paths=(Path('file.cc'),),
            all_paths=(Path('file.cc'),),
            events=self.events,
        )
        step = PassStep()
        orch.run('Presubmit', [step])

        self.events.program_start.assert_called_once()
        self.events.step_start.assert_called_once()
        self.events.step_end.assert_called_once()
        self.events.program_completed.assert_called_once()

    def test_run_takes_name(self) -> None:
        """Test that Orchestrator.run takes a name and passes it to events."""
        orch = Orchestrator(
            root=Path('.'),
            output_dir=self.output_dir,
            paths=(Path('file.cc'),),
            all_paths=(Path('file.cc'),),
            events=self.events,
        )
        step = PassStep()
        orch.run('MyCustomRun', [step])

        self.events.program_start.assert_called_once()
        args, _ = self.events.program_start.call_args
        self.assertEqual(args[0], 'MyCustomRun')

    def test_fix_fails_aborts(self) -> None:
        """Test that presubmit aborts if fix fails to fix the issue."""

        class FixFailsStep(FixableStep):
            def run(self, ctx: Context) -> None:
                self.run_count += 1
                ctx.fail('needs fix')

        step = FixFailsStep()
        orch = Orchestrator(
            root=Path('.'),
            output_dir=self.output_dir,
            paths=(Path('file.cc'),),
            all_paths=(Path('file.cc'),),
            events=self.events,
        )
        result = orch.run('Presubmit', [step], mode=Mode.FIX)

        self.assertEqual(step.run_count, 2)
        self.assertFalse(result.success)

    def test_fix_fails(self) -> None:
        """Test that if fix() fails (calls fail), the check fails normally."""

        class FixFailsStep(FixableStep):
            def fix(self, _ctx: Context) -> None:
                _ctx.fail('error')

        step = FixFailsStep()
        orch = Orchestrator(
            root=Path('.'),
            output_dir=self.output_dir,
            paths=(Path('file.cc'),),
            all_paths=(Path('file.cc'),),
            events=self.events,
        )
        result = orch.run('Presubmit', [step], mode=Mode.FIX)

        self.assertEqual(step.run_count, 1)
        self.assertFalse(result.success)

    def test_fix_not_called_when_fix_false(self) -> None:
        step = FixableStep()
        orch = Orchestrator(
            root=Path('.'),
            output_dir=self.output_dir,
            paths=(Path('file.cc'),),
            all_paths=(Path('file.cc'),),
            events=self.events,
        )
        orch.run('Presubmit', [step])

        self.assertEqual(step.run_count, 1)

    def test_fix_not_called_on_passing_check(self) -> None:
        """Test that fix is not called if the check passes."""

        class PassingStepWithFix(Step):
            def __init__(self) -> None:
                super().__init__()
                self.fix_called = False
                self.run_count = 0

            def run(self, _ctx: Context) -> None:
                self.run_count += 1

            def fix(self, _ctx: Context) -> None:
                self.fix_called = True

        step = PassingStepWithFix()
        orch = Orchestrator(
            root=Path('.'),
            output_dir=self.output_dir,
            paths=(Path('file.cc'),),
            all_paths=(Path('file.cc'),),
            events=self.events,
        )
        orch.run('Presubmit', [step], mode=Mode.FIX)

        self.assertFalse(step.fix_called)
        self.assertEqual(step.run_count, 1)

    def test_fix_crashes(self) -> None:
        """Test that an exception in fix() fails the check."""

        class FixCrashesStep(FixableStep):
            def fix(self, _ctx: Context) -> None:
                raise Exception('Fix crashed!')

        step = FixCrashesStep()
        orch = Orchestrator(
            root=Path('.'),
            output_dir=self.output_dir,
            paths=(Path('file.cc'),),
            all_paths=(Path('file.cc'),),
            events=self.events,
        )
        result = orch.run('Presubmit', [step], mode=Mode.FIX)

        self.assertEqual(step.run_count, 1)
        self.assertFalse(result.success)


class ModuleRunTest(unittest.TestCase):
    """Tests for the module-level run function."""

    def setUp(self) -> None:
        self.tmp_dir = tempfile.TemporaryDirectory()
        self.output_dir = Path(self.tmp_dir.name)

    def tearDown(self) -> None:
        self.tmp_dir.cleanup()

    def test_run_success(self) -> None:
        with mock.patch.object(git_repo, 'collect_files') as mock_collect:
            mock_collect.return_value = RepoFiles(
                paths=[Path('file.cc')], modified_paths=[Path('file.cc')]
            )
            mock_events = mock.Mock(spec=PresubmitEvents)

            step = PassStep()
            result = run(
                name='MyRun',
                steps=[step],
                root=Path('.'),
                events=mock_events,
                output_dir=self.output_dir,
                base='HEAD',
            )

            self.assertTrue(result.success)
            mock_collect.assert_called_once()

    def test_run_failure(self) -> None:
        with mock.patch.object(git_repo, 'collect_files') as mock_collect:
            mock_collect.return_value = RepoFiles(
                paths=[Path('file.cc')], modified_paths=[Path('file.cc')]
            )
            mock_events = mock.Mock(spec=PresubmitEvents)

            step = FailStep()
            result = run(
                name='MyRun',
                steps=[step],
                root=Path('.'),
                events=mock_events,
                output_dir=self.output_dir,
                base='HEAD',
            )

            self.assertFalse(result.success)


class AutoTest(unittest.TestCase):
    """Tests for auto mode."""

    def setUp(self) -> None:
        self.tmp_dir = tempfile.TemporaryDirectory()
        self.output_dir = Path(self.tmp_dir.name)
        self.events = mock.Mock(spec=PresubmitEvents)
        self.events.name = 'boxes'

    def tearDown(self) -> None:
        self.tmp_dir.cleanup()

    def _run_auto_test(  # pylint: disable=too-many-arguments
        self,
        num_commits,
        run_results,
        expected_return=None,
        expected_run_calls=0,
        expected_continue_calls=0,
        uncommitted_changes=False,
        git_rebase_fail=False,
        continue_fail=False,
        expect_exception=None,
        changes_after_run=False,
    ) -> None:
        with (
            mock.patch.object(orchestrator, 'run') as mock_run,
            mock.patch.object(subprocess, 'run') as mock_subproc,
        ):

            git_dir = self.output_dir / 'git_dir'
            git_dir.mkdir(exist_ok=True)
            rebase_merge = git_dir / 'rebase-merge'
            if num_commits > 0:
                rebase_merge.mkdir(exist_ok=True)
                (rebase_merge / 'onto').write_text('1234')
                (rebase_merge / 'orig-head').write_text('5678')

            continue_calls = 0

            diff_index_results = [uncommitted_changes] + [
                changes_after_run
            ] * 10
            diff_index_iter = iter(diff_index_results)

            def subproc_side_effect(args, **_kwargs):
                nonlocal continue_calls
                args_str = ' '.join(str(a) for a in args)

                if 'rev-parse' in args_str and '--absolute-git-dir' in args_str:
                    return mock.Mock(stdout=str(git_dir).encode(), returncode=0)

                if 'diff-index' in args_str:
                    try:
                        has_changes = next(diff_index_iter)
                    except StopIteration:
                        has_changes = False
                    return mock.Mock(returncode=1 if has_changes else 0)

                if 'rebase' in args_str and '-i' in args_str:
                    if git_rebase_fail:
                        return mock.Mock(returncode=1, stderr=b'failed')
                    return mock.Mock(returncode=0)

                if 'rebase' in args_str and '--continue' in args_str:
                    if continue_fail:
                        return mock.Mock(returncode=1, stderr=b'failed')
                    continue_calls += 1
                    if continue_calls >= num_commits:
                        if rebase_merge.exists():
                            (rebase_merge / 'onto').unlink(missing_ok=True)
                            (rebase_merge / 'orig-head').unlink(missing_ok=True)
                            rebase_merge.rmdir()
                    return mock.Mock(returncode=0)

                return mock.Mock(returncode=0, stdout=b'')

            mock_subproc.side_effect = subproc_side_effect

            mock_run.side_effect = [
                ProgramResult(
                    passed=[], failed=[], skipped=[], fixed=[], success=s
                )
                for s in run_results
            ]

            if expect_exception:
                with self.assertRaises(expect_exception):
                    auto('Presubmit', [], self.output_dir, self.events, 'HEAD')
                return

            result = auto('Presubmit', [], self.output_dir, self.events, 'HEAD')

            self.assertEqual(result, expected_return)
            self.assertEqual(mock_run.call_count, expected_run_calls)
            self.assertEqual(continue_calls, expected_continue_calls)

            if expected_run_calls > 0:
                for call in mock_run.call_args_list:
                    self.assertEqual(call.kwargs.get('mode'), Mode.AUTO)

    def test_auto_one_commit_succeeds_after_fixes(self) -> None:
        self._run_auto_test(
            num_commits=1,
            run_results=[True],
            expected_return=True,
            expected_run_calls=1,
            expected_continue_calls=1,
        )

    def test_auto_one_commit_fails(self) -> None:
        self._run_auto_test(
            num_commits=1,
            run_results=[False],
            expected_return=False,
            expected_run_calls=1,
            expected_continue_calls=0,
        )

    def test_auto_three_commits_succeeds(self) -> None:
        self._run_auto_test(
            num_commits=3,
            run_results=[True, True, True],
            expected_return=True,
            expected_run_calls=3,
            expected_continue_calls=3,
        )

    def test_auto_three_commits_succeeds_after_fixes(self) -> None:
        self._run_auto_test(
            num_commits=3,
            run_results=[True, True, True],
            expected_return=True,
            expected_run_calls=3,
            expected_continue_calls=3,
        )

    def test_auto_fixes_trigger_restart(self) -> None:
        """Test that auto mode restarts if fixes occurred."""
        with (
            mock.patch.object(orchestrator, 'run') as mock_run,
            mock.patch.object(subprocess, 'run') as mock_subproc,
        ):
            git_dir = self.output_dir / 'git_dir'
            git_dir.mkdir(exist_ok=True)
            rebase_merge = git_dir / 'rebase-merge'
            rebase_merge.mkdir(exist_ok=True)
            (rebase_merge / 'onto').write_text('1234')
            (rebase_merge / 'orig-head').write_text('5678')

            # 1 commit, first run fixed, second run clean
            mock_run.side_effect = [
                ProgramResult(
                    passed=[],
                    failed=[],
                    skipped=[],
                    fixed=['step1'],
                    success=True,
                ),
                ProgramResult(
                    passed=[], failed=[], skipped=[], fixed=[], success=True
                ),
            ]

            continue_calls = 0

            def subproc_side_effect(args, **_kwargs):
                nonlocal continue_calls
                args_str = ' '.join(str(a) for a in args)
                if 'rev-parse' in args_str and '--absolute-git-dir' in args_str:
                    return mock.Mock(stdout=str(git_dir).encode(), returncode=0)
                if 'diff-index' in args_str:
                    return mock.Mock(returncode=0)
                if 'rebase' in args_str and '-i' in args_str:
                    rebase_merge.mkdir(exist_ok=True)
                    (rebase_merge / 'onto').write_text('1234')
                    (rebase_merge / 'orig-head').write_text('5678')
                    return mock.Mock(returncode=0)
                if 'rebase' in args_str and '--continue' in args_str:
                    continue_calls += 1
                    if rebase_merge.exists():
                        (rebase_merge / 'onto').unlink(missing_ok=True)
                        (rebase_merge / 'orig-head').unlink(missing_ok=True)
                        rebase_merge.rmdir()
                    return mock.Mock(returncode=0)
                return mock.Mock(returncode=0, stdout=b'')

            mock_subproc.side_effect = subproc_side_effect

            result = auto('Presubmit', [], self.output_dir, self.events, 'HEAD')

            self.assertTrue(result)
            self.assertEqual(mock_run.call_count, 2)
            self.events.auto_mode_restarting.assert_called_once()

            # Verify rebase was called with 'HEAD' and not 'HEAD~'
            rebase_calls = []
            for c in mock_subproc.call_args_list:
                args_list = c[0][0]
                args_str = ' '.join(str(a) for a in args_list)
                if 'rebase' in args_str and '-i' in args_str:
                    rebase_calls.append(args_list)

            self.assertTrue(rebase_calls)
            self.assertTrue(str(rebase_calls[0][-1]).endswith('HEAD'))

    def test_auto_three_commits_fails_on_first(self) -> None:
        self._run_auto_test(
            num_commits=3,
            run_results=[False],
            expected_return=False,
            expected_run_calls=1,
            expected_continue_calls=0,
        )

    def test_auto_three_commits_fails_on_third(self) -> None:
        self._run_auto_test(
            num_commits=3,
            run_results=[True, True, False],
            expected_return=False,
            expected_run_calls=3,
            expected_continue_calls=2,
        )

    def test_auto_uncommitted_changes_initially(self) -> None:
        self._run_auto_test(
            num_commits=3,
            run_results=[],
            uncommitted_changes=True,
            expect_exception=AutoModeError,
        )

    def test_auto_git_rebase_fails(self) -> None:
        self._run_auto_test(
            num_commits=3,
            run_results=[],
            git_rebase_fail=True,
            expect_exception=AutoModeError,
        )

    def test_auto_uncommitted_changes_after_run(self) -> None:
        self._run_auto_test(
            num_commits=3,
            run_results=[True],
            expected_run_calls=1,
            changes_after_run=True,
            expect_exception=AutoModeError,
        )

    def test_auto_continue_fails(self) -> None:
        self._run_auto_test(
            num_commits=3,
            run_results=[True],
            continue_fail=True,
            expected_run_calls=1,
            expected_return=False,
        )

    def test_auto_continue_fails_but_completed(self) -> None:
        """Test auto fails if rebase continue fails even if completed."""
        with (
            mock.patch.object(orchestrator, 'run') as mock_run,
            mock.patch.object(git_repo, 'GitRepo') as mock_git_repo,
            mock.patch.object(subprocess, 'run') as mock_subproc,
        ):

            mock_git_repo.return_value.has_uncommitted_changes.return_value = (
                False
            )
            mock_git_repo.return_value.modify.return_value = (
                mock_git_repo.return_value
            )

            def mock_is_in_rebase():
                return rebase_merge.exists()

            mock_git_repo.return_value.is_in_rebase.side_effect = (
                mock_is_in_rebase
            )

            def mock_rebase_continue():
                (rebase_merge / 'onto').unlink(missing_ok=True)
                (rebase_merge / 'orig-head').unlink(missing_ok=True)
                rebase_merge.rmdir()
                raise GitError(['git', 'rebase', '--continue'], 'failed', 1)

            mock_git_repo.return_value.rebase_continue.side_effect = (
                mock_rebase_continue
            )

            git_dir = self.output_dir / 'git_dir'
            git_dir.mkdir(exist_ok=True)
            rebase_merge = git_dir / 'rebase-merge'
            rebase_merge.mkdir(exist_ok=True)
            (rebase_merge / 'onto').write_text('1234')
            (rebase_merge / 'orig-head').write_text('5678')

            def subproc_side_effect(args, **_kwargs):
                if args == ['git', 'rev-parse', '--git-dir']:
                    return mock.Mock(stdout=str(git_dir))
                if args == ['git', 'rebase', '--continue']:
                    (rebase_merge / 'onto').unlink(missing_ok=True)
                    (rebase_merge / 'orig-head').unlink(missing_ok=True)
                    rebase_merge.rmdir()
                    raise subprocess.CalledProcessError(1, args)
                return mock.Mock()

            mock_subproc.side_effect = subproc_side_effect

            mock_run.return_value = ProgramResult(
                passed=[], failed=[], skipped=[], fixed=[], success=True
            )

            with self.assertRaises(AutoModeError):
                auto('Presubmit', [], self.output_dir, self.events, 'HEAD')


class ResumeTest(unittest.TestCase):
    """Tests for resume function."""

    def setUp(self) -> None:
        self.tmp_dir = tempfile.TemporaryDirectory()
        self.output_dir = Path(self.tmp_dir.name)
        self.events = mock.Mock(spec=PresubmitEvents)
        self.events.name = 'boxes'

    def tearDown(self) -> None:
        self.tmp_dir.cleanup()

    def test_resume_success(self) -> None:
        """Test that resume successfully continues a rebase."""
        resume_file = self.output_dir / 'resume.json'
        state = {
            'name': 'Presubmit',
            'root': str(self.output_dir),
            'base': 'HEAD',
            'pathspecs': [],
            'output_dir': str(self.output_dir),
            'step_names': ['pass_step'],
            'event_handler': 'boxes',
            'rebase_onto': '1234',
            'rebase_orig_head': '5678',
        }
        with open(resume_file, 'w') as f:
            json.dump(state, f)

        with (
            mock.patch.object(orchestrator, '_auto_loop') as mock_loop,
            mock.patch.object(subprocess, 'run') as mock_subproc,
            mock.patch.object(orchestrator, 'auto') as mock_auto,
        ):

            mock_loop.return_value = (True, False)  # success, fixes_occurred

            git_dir = self.output_dir / 'git_dir'
            git_dir.mkdir()
            rebase_merge = git_dir / 'rebase-merge'
            rebase_merge.mkdir()
            (rebase_merge / 'onto').write_text('1234')
            (rebase_merge / 'orig-head').write_text('5678')

            def subproc_side_effect(args, **_kwargs):
                args_str = ' '.join(str(a) for a in args)
                if 'rev-parse' in args_str and '--absolute-git-dir' in args_str:
                    return mock.Mock(stdout=str(git_dir).encode(), returncode=0)
                return mock.Mock(stdout=b'', returncode=0)

            mock_subproc.side_effect = subproc_side_effect

            step = PassStep()

            result = resume({step.name: step}, resume_file)

            self.assertTrue(result)
            mock_loop.assert_called_once()
            mock_auto.assert_called_once()

    def test_resume_mismatched_rebase_state(self) -> None:
        """Test that resume fails if rebase state does not match."""
        resume_file = self.output_dir / 'resume.json'
        state = {
            'name': 'Presubmit',
            'root': str(self.output_dir),
            'base': 'HEAD',
            'pathspecs': [],
            'output_dir': str(self.output_dir),
            'step_names': ['pass_step'],
            'event_handler': 'boxes',
            'rebase_onto': '1234',
            'rebase_orig_head': '5678',
        }
        with open(resume_file, 'w') as f:
            json.dump(state, f)

        with mock.patch.object(subprocess, 'run') as mock_subproc:
            git_dir = self.output_dir / 'git_dir'
            git_dir.mkdir()
            rebase_merge = git_dir / 'rebase-merge'
            rebase_merge.mkdir()
            (rebase_merge / 'onto').write_text('aaaa')
            (rebase_merge / 'orig-head').write_text('bbbb')

            def subproc_side_effect(args, **_kwargs):
                args_str = ' '.join(str(a) for a in args)
                if 'rev-parse' in args_str and '--absolute-git-dir' in args_str:
                    return mock.Mock(stdout=str(git_dir).encode(), returncode=0)
                return mock.Mock(stdout=b'', returncode=0)

            mock_subproc.side_effect = subproc_side_effect

            step = PassStep()

            with self.assertRaises(AutoModeError):
                resume({step.name: step}, resume_file)

    def test_resume_mismatched_steps(self) -> None:
        """Test that resume fails if steps do not match."""
        resume_file = self.output_dir / 'resume.json'
        state = {
            'name': 'Presubmit',
            'root': str(self.output_dir),
            'base': 'HEAD',
            'pathspecs': [],
            'output_dir': str(self.output_dir),
            'step_names': ['other_step'],
            'event_handler': 'boxes',
        }
        with open(resume_file, 'w') as f:
            json.dump(state, f)

        step = PassStep()

        with self.assertRaises(AutoModeError):
            resume({step.name: step}, resume_file)

    def test_resume_not_in_rebase(self) -> None:
        """Test that resume fails if not in a rebase."""
        resume_file = self.output_dir / 'resume.json'
        state = {
            'name': 'Presubmit',
            'root': str(self.output_dir),
            'base': 'HEAD',
            'pathspecs': [],
            'output_dir': str(self.output_dir),
            'step_names': ['pass_step'],
            'event_handler': 'boxes',
        }
        with open(resume_file, 'w') as f:
            json.dump(state, f)

        with mock.patch.object(subprocess, 'run') as mock_subproc:
            git_dir = self.output_dir / 'git_dir'
            git_dir.mkdir()

            def subproc_side_effect(args, **_kwargs):
                args_str = ' '.join(str(a) for a in args)
                if 'rev-parse' in args_str and '--absolute-git-dir' in args_str:
                    return mock.Mock(stdout=str(git_dir).encode(), returncode=0)
                return mock.Mock(stdout=b'', returncode=0)

            mock_subproc.side_effect = subproc_side_effect

            step = PassStep()

            with self.assertRaises(AutoModeError):
                resume({step.name: step}, resume_file)

    def test_resume_invalid_json(self) -> None:
        """Test that resume fails on invalid JSON."""
        resume_file = self.output_dir / 'resume.json'
        with open(resume_file, 'w') as f:
            f.write('invalid json')

        step = PassStep()
        with self.assertRaises(AutoModeError):
            resume({step.name: step}, resume_file)

    def test_resume_missing_key(self) -> None:
        """Test that resume fails if a required key is missing."""
        resume_file = self.output_dir / 'resume.json'
        state = {
            'name': 'Presubmit',
            # 'root' is missing
            'base': 'HEAD',
            'pathspecs': [],
            'output_dir': str(self.output_dir),
            'step_names': ['pass_step'],
            'event_handler': 'boxes',
        }
        with open(resume_file, 'w') as f:
            json.dump(state, f)

        step = PassStep()
        with self.assertRaises(AutoModeError):
            resume({step.name: step}, resume_file)

    def test_resume_uncommitted_changes(self) -> None:
        """Test that resume fails if there are uncommitted changes."""
        resume_file = self.output_dir / 'resume.json'
        state = {
            'name': 'Presubmit',
            'root': str(self.output_dir),
            'base': 'HEAD',
            'pathspecs': [],
            'output_dir': str(self.output_dir),
            'step_names': ['pass_step'],
            'event_handler': 'boxes',
            'rebase_onto': '1234',
            'rebase_orig_head': '5678',
        }
        with open(resume_file, 'w') as f:
            json.dump(state, f)

        with mock.patch.object(subprocess, 'run') as mock_subproc:
            git_dir = self.output_dir / 'git_dir'
            git_dir.mkdir()
            rebase_merge = git_dir / 'rebase-merge'
            rebase_merge.mkdir()
            (rebase_merge / 'onto').write_text('1234')
            (rebase_merge / 'orig-head').write_text('5678')

            def subproc_side_effect(args, **_kwargs):
                args_str = ' '.join(str(a) for a in args)
                if 'rev-parse' in args_str and '--absolute-git-dir' in args_str:
                    return mock.Mock(stdout=str(git_dir).encode(), returncode=0)
                if 'diff-index' in args_str:
                    return mock.Mock(returncode=1)
                return mock.Mock(stdout=b'', returncode=0)

            mock_subproc.side_effect = subproc_side_effect

            step = PassStep()
            with self.assertRaises(AutoModeError):
                resume({step.name: step}, resume_file)

    def test_resume_fails(self) -> None:
        """Test that resume returns False if auto loop fails."""
        resume_file = self.output_dir / 'resume.json'
        state = {
            'name': 'Presubmit',
            'root': str(self.output_dir),
            'base': 'HEAD',
            'pathspecs': [],
            'output_dir': str(self.output_dir),
            'step_names': ['pass_step'],
            'event_handler': 'boxes',
            'rebase_onto': '1234',
            'rebase_orig_head': '5678',
        }
        with open(resume_file, 'w') as f:
            json.dump(state, f)

        with (
            mock.patch.object(orchestrator, '_auto_loop') as mock_loop,
            mock.patch.object(subprocess, 'run') as mock_subproc,
        ):

            mock_loop.return_value = (False, False)  # success, fixes_occurred

            git_dir = self.output_dir / 'git_dir'
            git_dir.mkdir()
            rebase_merge = git_dir / 'rebase-merge'
            rebase_merge.mkdir()
            (rebase_merge / 'onto').write_text('1234')
            (rebase_merge / 'orig-head').write_text('5678')

            def subproc_side_effect(args, **_kwargs):
                args_str = ' '.join(str(a) for a in args)
                if 'rev-parse' in args_str and '--absolute-git-dir' in args_str:
                    return mock.Mock(stdout=str(git_dir).encode(), returncode=0)
                return mock.Mock(stdout=b'', returncode=0)

            mock_subproc.side_effect = subproc_side_effect

            step = PassStep()

            result = resume({step.name: step}, resume_file)

            self.assertFalse(result)
            mock_loop.assert_called_once()


if __name__ == '__main__':
    unittest.main()
