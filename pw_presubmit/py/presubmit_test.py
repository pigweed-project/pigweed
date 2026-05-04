#!/usr/bin/env python3
# Copyright 2020 The Pigweed Authors
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
"""Tests for presubmit tools."""

import contextlib
import io
import tempfile
from pathlib import Path
import unittest
from unittest import mock

from pw_cli.git_repo import GitError
from pw_presubmit import presubmit
from pw_presubmit.private.events import PresubmitEvents, HumanUI
from pw_presubmit import Program, Programs, PresubmitResult
from pw_presubmit.private.result import ProgramResult


def _fake_function_1(_):
    """Fake presubmit function."""


def _fake_function_2(_):
    """Fake presubmit function."""


def _all_substeps(program):
    substeps = {}
    for step in program:
        # pylint: disable=protected-access
        for sub in step.substeps():
            substeps[sub.name or step.name] = sub._func
        # pylint: enable=protected-access
    return substeps


def _test_presubmit(
    test_case: unittest.TestCase, events=None
) -> presubmit.Presubmit:
    if events is None:
        events = mock.Mock(spec=PresubmitEvents)
    tmp_dir = tempfile.TemporaryDirectory()
    test_case.addCleanup(tmp_dir.cleanup)
    tmp_path = Path(tmp_dir.name)
    return presubmit.Presubmit(
        root=Path('.'),
        repos=[Path('.')],
        output_directory=tmp_path / 'out',
        paths=[Path('file.cc')],
        all_paths=[Path('file.cc')],
        package_root=tmp_path / 'packages',
        override_gn_args={},
        continue_after_build_error=False,
        rng_seed=1,
        full=False,
        events=events,
    )


class ProgramsTest(unittest.TestCase):
    """Tests the presubmit Programs abstraction."""

    def setUp(self):
        self._programs = Programs(
            first=[_fake_function_1, (), [(_fake_function_2,)]],
            second=[_fake_function_2],
        )

    def test_empty(self) -> None:
        self.assertEqual({}, Programs())

    def test_access_present_members_first(self) -> None:
        self.assertEqual('first', self._programs['first'].name)
        self.assertEqual(
            ('_fake_function_1', '_fake_function_2'),
            tuple(x.name for x in self._programs['first']),
        )

        self.assertEqual(2, len(self._programs['first']))
        substeps = _all_substeps(
            self._programs['first']  # pylint: disable=protected-access
        ).values()
        self.assertEqual(2, len(substeps))
        self.assertEqual((_fake_function_1, _fake_function_2), tuple(substeps))

    def test_access_present_members_second(self) -> None:
        self.assertEqual('second', self._programs['second'].name)
        self.assertEqual(
            ('_fake_function_2',),
            tuple(x.name for x in self._programs['second']),
        )

        self.assertEqual(1, len(self._programs['second']))
        substeps = _all_substeps(
            self._programs['second']  # pylint: disable=protected-access
        ).values()
        self.assertEqual(1, len(substeps))
        self.assertEqual((_fake_function_2,), tuple(substeps))

    def test_access_missing_member(self) -> None:
        with self.assertRaises(KeyError):
            _ = self._programs['not_there']

    def test_all_steps(self) -> None:
        all_steps = self._programs.all_steps()
        self.assertEqual(len(all_steps), 2)
        all_substeps = _all_substeps(all_steps.values())
        self.assertEqual(len(all_substeps), 2)

        # pylint: disable=protected-access
        self.assertEqual(all_substeps['_fake_function_1'], _fake_function_1)
        self.assertEqual(all_substeps['_fake_function_2'], _fake_function_2)
        # pylint: enable=protected-access


class PresubmitEventsTest(unittest.TestCase):
    """Tests for PresubmitEvents."""

    def test_run_calls_events(self) -> None:  # pylint: disable=no-self-use
        """Test that Presubmit.run calls event methods."""
        mock_events = mock.Mock(spec=PresubmitEvents)
        pre = _test_presubmit(self, events=mock_events)

        program = Program('test_program', [_fake_function_1])
        pre.run(program)

        # Verify calls
        mock_events.program_start.assert_called_once()
        args, _ = mock_events.program_start.call_args
        self.assertEqual(args[0], program.name)
        self.assertEqual(args[1], [c.name for c in program])
        self.assertEqual(args[2], [c.name for c in program])
        self.assertEqual(args[3], (Path('file.cc'),))

        mock_events.step_start.assert_called_once_with(
            program[0].name, 1, (Path('file.cc'),)
        )

        mock_events.step_end.assert_called_once_with(
            program[0].name, 1, PresubmitResult.PASS, mock.ANY
        )

        mock_events.program_completed.assert_called_once_with(
            ProgramResult(
                passed=['_fake_function_1'],
                failed=[],
                skipped=[],
                fixed=[],
                success=True,
            ),
            mock.ANY,
        )


class HumanUITest(unittest.TestCase):
    """Tests for HumanUI."""

    def test_title(self) -> None:
        """Test title rendering."""
        ui = HumanUI(width=40)
        output = io.StringIO()
        program = Program('test_program', [])
        with mock.patch('pw_presubmit.private.events.GitRepo') as mock_git_repo:
            mock_repo_inst = mock_git_repo.return_value
            mock_repo_inst.name_rev.return_value = 'main'
            mock_repo_inst.commit_message.return_value = 'Commit message\n'
            with contextlib.redirect_stdout(output):
                ui.program_start(program.name, [], [], [])

        self.assertIn('test_program', output.getvalue())
        self.assertIn('═', output.getvalue())

    def test_title_git_error(self) -> None:
        """Test title rendering when outside of a git repo."""
        ui = HumanUI(width=40)
        output = io.StringIO()
        program = Program('test_program', [])
        with mock.patch('pw_presubmit.private.events.GitRepo') as mock_git_repo:
            mock_repo_inst = mock_git_repo.return_value
            mock_repo_inst.name_rev.side_effect = GitError(
                ['name-rev'], 'not a git repo', 128
            )
            with contextlib.redirect_stdout(output):
                ui.program_start(program.name, [], [], [])

        self.assertIn('test_program', output.getvalue())
        self.assertNotIn('Commit message', output.getvalue())

    def test_fix_summary_ui(self) -> None:
        """Test that HumanUI summary reports FIXED correctly."""
        ui = HumanUI(width=80)
        output = io.StringIO()
        with mock.patch('pw_presubmit.private.events.GitRepo') as mock_git_repo:
            mock_repo_inst = mock_git_repo.return_value
            mock_repo_inst.name_rev.return_value = 'main'
            mock_repo_inst.commit_message.return_value = 'Commit message\n'
            with contextlib.redirect_stdout(output):
                ui.program_start('test_program', [], [], [Path('a'), Path('b')])

        result = ProgramResult(
            passed=['step'] * 8,
            failed=[],
            skipped=[],
            fixed=['fixed_step'],
            success=True,
        )
        with contextlib.redirect_stdout(output):
            ui.program_completed(result, 1.0)

        output_str = output.getvalue()
        self.assertIn('FIXED', output_str)
        self.assertIn('8 passed, 1 fixed', output_str)


if __name__ == '__main__':
    unittest.main()
