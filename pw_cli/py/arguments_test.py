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
"""Tests for pw_cli.arguments."""

import io
import os
import unittest
from unittest.mock import patch

from pw_cli import arguments
from pw_cli import env


class TestIsInteractiveTerminal(unittest.TestCase):
    """Tests for arguments.is_interactive_terminal."""

    @patch('sys.stdout.isatty', return_value=True)
    @patch('sys.stderr.isatty', return_value=True)
    def test_interactive_human_terminal(self, _mock_stderr, _mock_stdout):
        clean_env = {'TERM': 'xterm-256color'}
        with patch.dict(os.environ, clean_env, clear=True):
            self.assertTrue(arguments.is_interactive_terminal())

    @patch('sys.stdout.isatty', return_value=False)
    @patch('sys.stderr.isatty', return_value=True)
    def test_stdout_not_tty(self, _mock_stderr, _mock_stdout):
        clean_env = {'TERM': 'xterm-256color'}
        with patch.dict(os.environ, clean_env, clear=True):
            self.assertFalse(arguments.is_interactive_terminal())

    @patch('sys.stdout.isatty', return_value=True)
    @patch('sys.stderr.isatty', return_value=False)
    def test_stderr_not_tty(self, _mock_stderr, _mock_stdout):
        clean_env = {'TERM': 'xterm-256color'}
        with patch.dict(os.environ, clean_env, clear=True):
            self.assertFalse(arguments.is_interactive_terminal())

    @patch('sys.stdout.isatty', return_value=True)
    @patch('sys.stderr.isatty', return_value=True)
    def test_dumb_terminal(self, _mock_stderr, _mock_stdout):
        with patch.dict(os.environ, {'TERM': 'dumb'}, clear=True):
            self.assertFalse(arguments.is_interactive_terminal())

    @patch('sys.stdout.isatty', return_value=True)
    @patch('sys.stderr.isatty', return_value=True)
    def test_empty_term(self, _mock_stderr, _mock_stdout):
        with patch.dict(os.environ, {'TERM': ''}, clear=True):
            self.assertFalse(arguments.is_interactive_terminal())

    @patch('sys.stdout.isatty', return_value=True)
    @patch('sys.stderr.isatty', return_value=True)
    def test_agent_and_ci_env_vars(self, _mock_stderr, _mock_stdout):
        agent_vars = [
            'AI_AGENT',
            'ANTIGRAVITY_TRAJECTORY_ID',
            'CI',
            'CLAUDECODE',
            'CONTINUOUS_INTEGRATION',
            'CURSOR_AGENT',
            'INVOKER_INFO_NAME',
            'JETSKI',
        ]
        for var in agent_vars:
            with self.subTest(var=var):
                clean_env = {'TERM': 'xterm-256color', var: '1'}
                with patch.dict(os.environ, clean_env, clear=True):
                    self.assertFalse(arguments.is_interactive_terminal())


class TestBannerPrinting(unittest.TestCase):
    """Tests for arguments.print_banner and arguments.maybe_print_banner."""

    def setUp(self):
        env._memoized_environment = None  # pylint: disable=protected-access

    def tearDown(self):
        env._memoized_environment = None  # pylint: disable=protected-access

    @patch('sys.stderr', new_callable=io.StringIO)
    def test_print_banner(self, mock_stderr):
        arguments.print_banner()
        output = mock_stderr.getvalue()
        self.assertTrue(len(output) > 0)
        self.assertIn('▒█████▄', output)

    @patch('pw_cli.arguments.is_interactive_terminal', return_value=True)
    @patch('sys.stderr', new_callable=io.StringIO)
    def test_maybe_print_banner_interactive(
        self, mock_stderr, _mock_is_interactive
    ):
        with patch.dict(os.environ, {}, clear=True):
            arguments.maybe_print_banner()
            output = mock_stderr.getvalue()
            self.assertTrue(len(output) > 0)
            self.assertIn('▒█████▄', output)

    @patch('pw_cli.arguments.is_interactive_terminal', return_value=False)
    @patch('sys.stderr', new_callable=io.StringIO)
    def test_maybe_print_banner_suppressed_for_non_interactive(
        self, mock_stderr, _mock_is_interactive
    ):
        with patch.dict(os.environ, {}, clear=True):
            arguments.maybe_print_banner()
            self.assertEqual(mock_stderr.getvalue(), '')

    @patch('pw_cli.arguments.is_interactive_terminal', return_value=True)
    @patch('sys.stderr', new_callable=io.StringIO)
    def test_maybe_print_banner_disabled_by_pw_envsetup_no_banner(
        self, mock_stderr, _mock_is_interactive
    ):
        with patch.dict(os.environ, {'PW_ENVSETUP_NO_BANNER': '1'}, clear=True):
            arguments.maybe_print_banner()
            self.assertEqual(mock_stderr.getvalue(), '')

    @patch('pw_cli.arguments.is_interactive_terminal', return_value=True)
    @patch('sys.stderr', new_callable=io.StringIO)
    def test_maybe_print_banner_disabled_by_pw_envsetup_quiet(
        self, mock_stderr, _mock_is_interactive
    ):
        with patch.dict(os.environ, {'PW_ENVSETUP_QUIET': '1'}, clear=True):
            arguments.maybe_print_banner()
            self.assertEqual(mock_stderr.getvalue(), '')


if __name__ == '__main__':
    unittest.main()
