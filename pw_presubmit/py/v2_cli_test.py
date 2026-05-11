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
"""Tests for v2 CLI."""

import argparse
from pathlib import Path
import unittest
from unittest import mock

from pw_presubmit.private import orchestrator
from pw_presubmit.private import v2_cli
from pw_presubmit.private.step import Step, Context


class Step1(Step):
    def run(self, ctx: Context) -> None:
        pass


class Step2(Step):
    def run(self, ctx: Context) -> None:
        pass


class V2CliTest(unittest.TestCase):
    """Tests for v2 CLI argument parsing and processing."""

    def setUp(self) -> None:
        self.step1 = Step1()
        self.step2 = Step2()
        self.programs: dict[str, list[Step]] = {
            "prog1": [self.step1],
            "prog2": [self.step2],
        }

    def test_add_arguments(self) -> None:
        parser = argparse.ArgumentParser()
        programs: dict[str, list[Step]] = {
            'prog1': [self.step1],
            'prog2': [self.step2],
        }
        v2_cli.add_arguments(parser, programs)

        args = parser.parse_args(
            ['-p', 'prog1', '--mode', 'continue', '--output-dir', 'out']
        )
        self.assertEqual(args.program, 'prog1')
        self.assertEqual(args.mode, orchestrator.Mode.CONTINUE)
        self.assertEqual(args.output_dir, Path('out'))

    @mock.patch('pw_presubmit.git_repo.find_git_repo')
    @mock.patch('pw_presubmit.private.orchestrator.run')
    def test_main(self, mock_run, mock_find_git_repo) -> None:
        mock_run.return_value = mock.Mock(success=True)
        mock_repo = mock.Mock()
        mock_repo.root.return_value = Path('/mock/root')
        mock_find_git_repo.return_value = mock_repo

        result = v2_cli.main(self.programs, argv=['-p', 'prog1'])

        self.assertEqual(result, 0)
        mock_run.assert_called_once()

    @mock.patch('pw_presubmit.private.v2_cli._main')
    def test_default_program(self, mock_main) -> None:
        mock_main.return_value = 0

        v2_cli.main(
            self.programs, default_program="prog1", argv=["--output-dir", "out"]
        )

        mock_main.assert_called_once()
        kwargs = mock_main.call_args.kwargs
        self.assertEqual(kwargs['program'], 'prog1')
        self.assertEqual(kwargs['steps'], self.programs['prog1'])

    @mock.patch('pw_presubmit.private.v2_cli._main')
    def test_program_override(self, mock_main) -> None:
        mock_main.return_value = 0

        v2_cli.main(
            self.programs,
            default_program="prog1",
            argv=["--program", "prog2", "--output-dir", "out"],
        )

        mock_main.assert_called_once()
        kwargs = mock_main.call_args.kwargs
        self.assertEqual(kwargs['program'], 'prog2')
        self.assertEqual(kwargs['steps'], self.programs['prog2'])

    @mock.patch('pw_presubmit.private.v2_cli._main')
    def test_step_override(self, mock_main) -> None:
        mock_main.return_value = 0

        v2_cli.main(
            self.programs,
            default_program="prog1",
            argv=["--step", "step1", "--output-dir", "out"],
        )

        mock_main.assert_called_once()
        kwargs = mock_main.call_args.kwargs
        self.assertEqual(kwargs['program'], 'custom')
        self.assertEqual(kwargs['steps'], [self.step1])

    @mock.patch('pw_presubmit.private.v2_cli._main')
    def test_multiple_steps(self, mock_main) -> None:
        mock_main.return_value = 0

        v2_cli.main(
            self.programs,
            default_program="prog1",
            argv=[
                "--step",
                "step1",
                "--step",
                "step2",
                "--output-dir",
                "out",
            ],
        )

        mock_main.assert_called_once()
        kwargs = mock_main.call_args.kwargs
        self.assertEqual(kwargs['program'], 'custom')
        self.assertEqual(kwargs['steps'], [self.step1, self.step2])

    @mock.patch('pw_presubmit.private.v2_cli._main')
    def test_mode_override(self, mock_main) -> None:
        mock_main.return_value = 0

        v2_cli.main(
            self.programs,
            default_program="prog1",
            argv=["--mode", "fix", "--output-dir", "out"],
        )

        mock_main.assert_called_once()
        kwargs = mock_main.call_args.kwargs
        self.assertEqual(kwargs['mode'], orchestrator.Mode.FIX)

    @mock.patch('pw_presubmit.private.v2_cli._main')
    def test_ui_override(self, mock_main) -> None:
        mock_main.return_value = 0

        v2_cli.main(
            self.programs,
            default_program="prog1",
            argv=["--ui", "minimal", "--output-dir", "out"],
        )

        mock_main.assert_called_once()
        kwargs = mock_main.call_args.kwargs
        self.assertEqual(kwargs['ui'], 'minimal')

    def test_no_program_or_step_fails(self) -> None:
        with self.assertRaises(SystemExit):
            v2_cli.main(self.programs, argv=["--output-dir", "out"])

    def test_mutually_exclusive_fails(self) -> None:
        with self.assertRaises(SystemExit):
            v2_cli.main(
                self.programs,
                argv=[
                    "--program",
                    "prog1",
                    "--step",
                    "step1",
                    "--output-dir",
                    "out",
                ],
            )

    def test_parse_args_list_steps_exits(self) -> None:
        with self.assertRaises(SystemExit) as cm:
            v2_cli.main(self.programs, argv=['--list-steps'])
        self.assertEqual(cm.exception.code, 0)

    def test_unknown_step_fails(self) -> None:
        with self.assertRaises(SystemExit):
            v2_cli.main(
                self.programs,
                argv=["--step", "unknown_step", "--output-dir", "out"],
            )

    @mock.patch('pw_presubmit.private.orchestrator.resume')
    @mock.patch(
        'pathlib.Path.open',
        new_callable=mock.mock_open,
        read_data='{"event_handler": "minimal"}',
    )
    def test_resume(self, _mock_open, mock_resume) -> None:
        mock_resume.return_value = True

        result = v2_cli.main(
            self.programs, argv=['--resume', 'path/to/resume.json']
        )

        self.assertEqual(result, 0)
        mock_resume.assert_called_once()
        kwargs = mock_resume.call_args.kwargs
        self.assertEqual(
            kwargs['all_steps'], {'step1': self.step1, 'step2': self.step2}
        )
        self.assertEqual(kwargs['resume_file'], Path('path/to/resume.json'))

    @mock.patch(
        'pathlib.Path.open',
        new_callable=mock.mock_open,
        read_data='invalid json',
    )
    def test_resume_invalid_json(self, mock_open) -> None:
        result = v2_cli.main(
            self.programs, argv=['--resume', 'path/to/resume.json']
        )
        self.assertEqual(result, 1)
        mock_open.assert_called_once()


if __name__ == '__main__':
    unittest.main()
