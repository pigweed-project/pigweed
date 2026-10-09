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
"""Tests for pw_package.git_repo."""

from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

from pw_package.git_repo import GitRepo


class TestGitRepo(unittest.TestCase):
    """Tests for GitRepo checkout behavior."""

    @mock.patch('pw_package.git_repo.git_stdout')
    def test_checkout_full_commit(self, mock_git_stdout: mock.MagicMock):
        repo = GitRepo(
            name='test_pkg',
            url='https://example.com/repo.git',
            commit='deadbeef',
        )
        path = Path('/tmp/test_pkg')
        repo.checkout_full(path)

        self.assertEqual(
            mock_git_stdout.call_args_list,
            [
                mock.call(
                    'clone',
                    '--no-checkout',
                    '--filter=blob:none',
                    'https://example.com/repo.git',
                    path,
                ),
                mock.call('reset', '--hard', 'deadbeef', repo=path),
            ],
        )

    @mock.patch('pw_package.git_repo.git_stdout')
    def test_checkout_full_tag(self, mock_git_stdout: mock.MagicMock):
        repo = GitRepo(
            name='test_pkg',
            url='https://example.com/repo.git',
            tag='v1.2.3',
        )
        path = Path('/tmp/test_pkg')
        repo.checkout_full(path)

        self.assertEqual(
            mock_git_stdout.call_args_list,
            [
                mock.call(
                    'clone',
                    '-b',
                    'v1.2.3',
                    '--no-checkout',
                    '--filter=blob:none',
                    'https://example.com/repo.git',
                    path,
                ),
                mock.call('reset', '--hard', 'v1.2.3', repo=path),
            ],
        )

    @mock.patch('pw_package.git_repo.git_stdout')
    def test_checkout_full_cleans_up_on_error(
        self, mock_git_stdout: mock.MagicMock
    ):
        repo = GitRepo(
            name='test_pkg',
            url='https://example.com/repo.git',
            commit='deadbeef',
        )
        with tempfile.TemporaryDirectory() as tmp_dir:
            pkg_path = Path(tmp_dir) / 'test_pkg'

            def side_effect(*args, **_kwargs):
                if args[0] == 'clone':
                    (pkg_path / '.git').mkdir(parents=True)
                    return ''
                raise subprocess.CalledProcessError(128, ['git', *args])

            mock_git_stdout.side_effect = side_effect

            with self.assertRaises(subprocess.CalledProcessError):
                repo.checkout_full(pkg_path)

            self.assertFalse(pkg_path.exists())


if __name__ == '__main__':
    unittest.main()
