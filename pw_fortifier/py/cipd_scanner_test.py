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
"""Tests for the CipdScanner class."""
# pylint: disable=protected-access

import subprocess
import unittest
from unittest.mock import MagicMock

from pw_fortifier.cipd_scanner import CipdScanner


class FakeCipdScanner(CipdScanner):
    """Fake CipdScanner for testing base class methods."""

    def target(self):
        return 'fake'


class TestCipdScanner(unittest.TestCase):
    """Tests for CipdScanner."""

    def setUp(self):
        """Set up test environment."""
        self.scanner = FakeCipdScanner()

    def test_run_cipd_describe_parsing(self):
        """Test parsing cipd describe output."""
        mock_output = (
            "Package:       pigweed/third_party/doxygen/linux-amd64\n"
            "Instance:      pigweed/third_party/doxygen/linux-amd64:"
            "git_revision:5d15657a55555e6181a7830a5c723af75e7577e2\n"
            "Registered by: user@google.com\n"
            "Registered at: 2026-06-01 12:00:00 -0700 MST\n"
            "Refs:\n"
            "  latest\n"
            "  prod\n"
            "Tags:\n"
            "  git_repository:https://github.com/doxygen/doxygen\n"
            "  git_revision:5d15657a55555e6181a7830a5c723af75e7577e2\n"
            "  version:1.9.4\n"
            "Metadata:\n"
            "  version:1.9.4\n"
        )
        self.scanner._run_cipd = MagicMock(
            return_value=subprocess.CompletedProcess(
                args=['describe', 'pkg', '--version', 'ref'],
                returncode=0,
                stdout=mock_output,
                stderr='',
            )
        )

        result = self.scanner.run_cipd_describe('pkg', 'ref')

        expected = {
            'Package': 'pigweed/third_party/doxygen/linux-amd64',
            'Instance': (
                'pigweed/third_party/doxygen/linux-amd64:'
                'git_revision:5d15657a55555e6181a7830a5c723af75e7577e2'
            ),
            'Registered by': 'user@google.com',
            'Registered at': '2026-06-01 12:00:00 -0700 MST',
            'Refs': {'latest': '', 'prod': ''},
            'Tags': {
                'git_repository': 'https://github.com/doxygen/doxygen',
                'git_revision': '5d15657a55555e6181a7830a5c723af75e7577e2',
                'version': '1.9.4',
            },
            'Metadata': {'version': '1.9.4'},
        }

        self.assertEqual(result, expected)

    def test_run_cipd_describe_failure(self):
        """Test run_cipd_describe when cipd command fails."""
        self.scanner._run_cipd = MagicMock(
            side_effect=RuntimeError('cipd failed')
        )

        result = self.scanner.run_cipd_describe('pkg', 'ref')
        self.assertEqual(result, {})

    def test_run_cipd_instances_parsing(self):
        """Test parsing cipd instances output."""
        mock_output = (
            "Instance ID                                   "
            "Registered by        Registered at\n"
            "----------------------------------------"
            "-----------------------------------------\n"
            "git_revision:ninja_latest_hash                "
            "user@google.com      2026-06-08 12:00:00 -0700 MST\n"
            "git_revision:ninja_mid_hash                   "
            "user@google.com      2026-06-06 12:00:00 -0700 MST\n"
            "git_revision:ninja123                        "
            "user@google.com      2026-06-05 12:00:00 -0700 MST\n"
        )
        self.scanner._run_cipd = MagicMock(
            return_value=subprocess.CompletedProcess(
                args=['instances', 'pkg'],
                returncode=0,
                stdout=mock_output,
                stderr='',
            )
        )

        result = list(self.scanner.run_cipd_instances('pkg'))

        expected = [
            'git_revision:ninja_latest_hash',
            'git_revision:ninja_mid_hash',
            'git_revision:ninja123',
        ]
        self.assertEqual(result, expected)

    def test_run_cipd_instances_failure(self):
        """Test run_cipd_instances when cipd command fails."""
        self.scanner._run_cipd = MagicMock(
            side_effect=RuntimeError('cipd failed')
        )

        result = list(self.scanner.run_cipd_instances('pkg'))
        self.assertEqual(result, [])

    def test_get_package_version_unexpected_format_raises(self):
        """Test _get_package_version when 'Registered at' format is invalid."""
        self.scanner.run_cipd_describe = MagicMock(
            return_value={'Registered at': {'foo': 'bar'}}
        )
        with self.assertRaises(AttributeError):
            self.scanner._get_package_version('pkg', 'ref')

    def test_get_package_version_missing_registered_at_raises(self):
        """Test _get_package_version when 'Registered at' is missing."""
        self.scanner.run_cipd_describe = MagicMock(
            return_value={'Package': 'some-pkg'}
        )
        with self.assertRaises(KeyError):
            self.scanner._get_package_version('pkg', 'ref')


if __name__ == '__main__':
    unittest.main()
