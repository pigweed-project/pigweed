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
"""Tests for the pw_build.error module."""

import unittest
from unittest.mock import patch, MagicMock
from pathlib import Path

from pw_build.error import main


class TestErrorScript(unittest.TestCase):
    """Tests for the error script."""

    @patch('shutil.which')
    @patch('subprocess.run')
    def test_gn_found_in_path(self, mock_run, mock_which):
        """Test that gn is found in the system PATH."""
        mock_which.side_effect = lambda cmd, path=None: (
            '/usr/bin/gn' if cmd == 'gn' and path is None else None
        )
        mock_run.return_value = MagicMock(
            returncode=0, stdout=b'No non-data paths found'
        )

        result = main(
            message="test error",
            target="//foo:bar",
            root=Path('/project/root'),
            out=Path('/project/out'),
        )

        self.assertEqual(result, 1)
        mock_run.assert_called_once()
        args, _ = mock_run.call_args
        self.assertEqual(args[0][0], '/usr/bin/gn')

    @patch('shutil.which')
    @patch('subprocess.run')
    def test_gn_found_in_fallback(self, mock_run, mock_which):
        """Test that gn is found in fallback paths."""

        def side_effect(cmd, path=None):
            if cmd == 'gn':
                if path is None:
                    return None
                if (
                    str(Path('/project/root/environment/cipd/packages/pigweed'))
                    in path
                ):
                    return '/project/root/environment/cipd/packages/pigweed/gn'
            return None

        mock_which.side_effect = side_effect
        mock_run.return_value = MagicMock(
            returncode=0, stdout=b'No non-data paths found'
        )

        result = main(
            message="test error",
            target="//foo:bar",
            root=Path('/project/root'),
            out=Path('/project/out'),
        )

        self.assertEqual(result, 1)
        mock_run.assert_called_once()
        args, _ = mock_run.call_args
        self.assertEqual(
            args[0][0],
            '/project/root/environment/cipd/packages/pigweed/gn',
        )

    @patch('shutil.which')
    @patch('subprocess.run')
    def test_gn_not_found(self, mock_run, mock_which):
        """Test that gn is not found anywhere."""
        mock_which.return_value = None

        result = main(
            message="test error",
            target="//foo:bar",
            root=Path('/project/root'),
            out=Path('/project/out'),
        )

        self.assertEqual(result, 1)
        mock_run.assert_not_called()


if __name__ == '__main__':
    unittest.main()
