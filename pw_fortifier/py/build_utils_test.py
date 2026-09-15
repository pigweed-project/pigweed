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
"""Unit tests for build_utils."""

import subprocess
import unittest
from unittest.mock import patch

from pw_fortifier.build_utils import run_presubmit, run_unit_tests


class TestBuildUtils(unittest.IsolatedAsyncioTestCase):
    """Tests for build_utils functions."""

    @patch('pw_fortifier.build_utils.run_bazelisk')
    async def test_run_unit_tests_default_target(
        self, mock_run_bazelisk
    ) -> None:
        """Tests run_unit_tests uses '//...' by default."""
        mock_run_bazelisk.return_value = subprocess.CompletedProcess(
            args=['bazelisk', 'test', '//...'],
            returncode=0,
            stdout='//pw_foo:bar_test PASSED in 0.5s\n',
            stderr='',
        )

        failed_tests = [t async for t in run_unit_tests(cwd='/path/to/repo')]
        self.assertEqual(failed_tests, [])
        mock_run_bazelisk.assert_called_once_with(
            ['test', '//...'], cwd='/path/to/repo'
        )

    @patch('pw_fortifier.build_utils.run_bazelisk')
    async def test_run_unit_tests_custom_target(
        self, mock_run_bazelisk
    ) -> None:
        """Tests run_unit_tests uses specified target."""
        mock_run_bazelisk.return_value = subprocess.CompletedProcess(
            args=['bazelisk', 'test', '//pw_fortifier/py:defect_test'],
            returncode=0,
            stdout='//pw_fortifier/py:defect_test PASSED in 0.2s\n',
            stderr='',
        )

        failed_tests = [
            t
            async for t in run_unit_tests(
                cwd='/path/to/repo', target='//pw_fortifier/py:defect_test'
            )
        ]
        self.assertEqual(failed_tests, [])
        mock_run_bazelisk.assert_called_once_with(
            ['test', '//pw_fortifier/py:defect_test'], cwd='/path/to/repo'
        )

    @patch('pw_fortifier.build_utils.run_bazelisk')
    async def test_run_unit_tests_yields_failed_tests(
        self, mock_run_bazelisk
    ) -> None:
        """Tests run_unit_tests extracts failed test targets deduplicated."""
        mock_run_bazelisk.return_value = subprocess.CompletedProcess(
            args=['bazelisk', 'test', '//...'],
            returncode=3,
            stdout=(
                'FAIL: //pw_fortifier/py:poc_test (Exit 1) (see /log)\n'
                'INFO: Found 6 targets and 32 test targets...\n'
                '//pw_fortifier/py:collector_test (cached) PASSED in 0.5s\n'
                '//pw_fortifier/py:defect_test FAILED in 0.4s\n'
                '//pw_fortifier/py:poc_test FAILED in 0.4s\n'
                '//pw_fortifier/py:timeout_test TIMEOUT in 30.0s\n'
            ),
            stderr='',
        )

        failed_tests = [t async for t in run_unit_tests(cwd='/path/to/repo')]
        self.assertEqual(
            failed_tests,
            [
                '//pw_fortifier/py:poc_test',
                '//pw_fortifier/py:defect_test',
                '//pw_fortifier/py:timeout_test',
            ],
        )

    @patch('pw_fortifier.build_utils.run_bazelisk')
    async def test_run_presubmit_success(self, mock_run_bazelisk) -> None:
        """Tests run_presubmit returns True when command succeeds."""
        mock_run_bazelisk.return_value = subprocess.CompletedProcess(
            args=['bazelisk', 'run', '//:pw', '--', 'presubmit'],
            returncode=0,
            stdout='Presubmit passed\n',
            stderr='',
        )

        result = await run_presubmit(cwd='/path/to/repo')
        self.assertTrue(result)
        mock_run_bazelisk.assert_called_once_with(
            ['run', '//:pw', '--', 'presubmit'], cwd='/path/to/repo'
        )

    @patch('pw_fortifier.build_utils.run_bazelisk')
    async def test_run_presubmit_failure(self, mock_run_bazelisk) -> None:
        """Tests run_presubmit returns False when command fails."""
        mock_run_bazelisk.return_value = subprocess.CompletedProcess(
            args=['bazelisk', 'run', '//:pw', '--', 'presubmit'],
            returncode=1,
            stdout='Presubmit failed\n',
            stderr='',
        )

        result = await run_presubmit(cwd='/path/to/repo')
        self.assertFalse(result)


if __name__ == '__main__':
    unittest.main()
