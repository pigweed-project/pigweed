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
"""Tests for bazel_dep_scanner."""
# pylint: disable=protected-access

from datetime import date
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch, MagicMock

from pw_fortifier.bazel_dep_scanner import BazelDepScanner
from pw_fortifier.package_scanner import PackageVersion


class TestBazelDepScanner(unittest.TestCase):
    """Tests for BazelDepScanner."""

    def setUp(self):
        """Set up test environment."""
        self.test_dir = tempfile.mkdtemp()
        self.module_bazel = os.path.join(self.test_dir, 'MODULE.bazel')

        # Line 2: foo-mod
        # Line 3: bar-tool
        # Line 4: dev-mod
        self.module_bazel_content = (
            '# Fake MODULE.bazel\n'
            'bazel_dep(name = "foo-mod", version = "1.0.0")\n'
            'bazel_dep(name = "bar-tool", version = "2.0.0")\n'
            'bazel_dep('
            'name = "dev-mod", version = "3.0.0", dev_dependency = True'
            ')\n'
        )
        with open(self.module_bazel, 'w') as f:
            f.write(self.module_bazel_content)

        # Write a fake OWNERS file
        self.owners_path = os.path.join(self.test_dir, 'OWNERS')
        with open(self.owners_path, 'w') as f:
            f.write(
                'owner-foo@google.com\n'
                'owner-bar@google.com\n'
                'owner-dev@google.com\n'
            )

    def tearDown(self):
        """Tear down test environment."""
        shutil.rmtree(self.test_dir)

    @staticmethod
    def _mock_git_log(cmd, **_kwargs):
        if '--format=%H,%cd' in cmd:
            scope = cmd[-1]
            parts = scope.split('/')
            if len(parts) == 3 and parts[0] == 'modules':
                module = parts[1]
                version = parts[2]
                date_str = '2026-01-01 12:00:00 +0000'
                if module == 'foo-mod':
                    if version == '1.0.0':
                        date_str = '2026-01-01 12:00:00 +0000'
                    elif version == '1.1.0':
                        date_str = '2026-02-01 12:00:00 +0000'
                    elif version == '2.0.0':
                        date_str = '2026-06-05 12:00:00 +0000'
                elif module == 'bar-tool':
                    if version == '2.0.0':
                        date_str = '2026-02-02 12:00:00 +0000'
                    elif version == '2.1.0':
                        date_str = '2026-06-05 12:00:00 +0000'
                elif module == 'dev-mod':
                    if version == '3.0.0':
                        date_str = '2026-03-03 12:00:00 +0000'
                return subprocess.CompletedProcess(
                    args=cmd,
                    returncode=0,
                    stdout=f'hash_{module}_{version},{date_str}\n',
                    stderr='',
                )
        return subprocess.CompletedProcess(
            args=cmd,
            returncode=0,
            stdout='hash1 Fake commit\n',
            stderr='',
        )

    @staticmethod
    def _mock_git_show(cmd, **_kwargs):
        return subprocess.CompletedProcess(
            args=cmd, returncode=0, stdout='MODULE.bazel\n', stderr=''
        )

    def _mock_git_blame(self, cmd, **_kwargs):
        has_l_arg = '-L' in cmd
        if has_l_arg:
            l_val = cmd[cmd.index('-L') + 1]
            filepath = cmd[-1]
            if os.path.isabs(filepath):
                rel_path = os.path.relpath(filepath, self.test_dir)
            else:
                rel_path = filepath

            if rel_path == 'MODULE.bazel':
                if l_val == '2,2':
                    stdout = (
                        'hash1 (<owner-foo@google.com> '
                        '2026-06-01 10:00:00 +0000 2) '
                        'bazel_dep(name = "foo-mod", '
                        'version = "1.0.0")\n'
                    )
                elif l_val == '3,3':
                    stdout = (
                        'hash1 (<owner-bar@google.com> '
                        '2026-06-01 10:00:00 +0000 3) '
                        'bazel_dep(name = "bar-tool", '
                        'version = "2.0.0")\n'
                    )
                elif l_val == '4,4':
                    stdout = (
                        'hash1 (<owner-dev@google.com> '
                        '2026-06-01 10:00:00 +0000 4) '
                        'bazel_dep(name = "dev-mod", '
                        'version = "3.0.0", dev_dependency = True)\n'
                    )
                else:
                    stdout = ''
            else:
                stdout = ''
        else:
            stdout = ''
        return subprocess.CompletedProcess(
            args=cmd, returncode=0, stdout=stdout, stderr=''
        )

    @staticmethod
    def _mock_git_clone(cmd, **kwargs):
        cwd = kwargs.get('cwd')
        if cwd:
            os.makedirs(os.path.join(cwd, 'src'), exist_ok=True)
        return subprocess.CompletedProcess(
            args=cmd, returncode=0, stdout='', stderr=''
        )

    def _mock_subprocess_run(self, cmd, **kwargs):
        """Mock git commands."""
        if cmd[0] == 'git':
            subcommands = {
                'log': self._mock_git_log,
                'show': self._mock_git_show,
                'blame': self._mock_git_blame,
                'clone': self._mock_git_clone,
            }
            subcmd = subcommands.get(cmd[1])
            if subcmd:
                return subcmd(cmd, **kwargs)

        raise ValueError(f'Unexpected command: {cmd}')

    @staticmethod
    def _mock_run_bazelisk(args, _cwd):
        """Mock bazelisk commands."""
        if args == ['mod', 'graph', '--ignore_dev_dependency', '--output=json']:
            data = {
                'dependencies': [
                    {'name': 'foo-mod', 'version': '1.0.0'},
                    {'name': 'bar-tool', 'version': '2.0.0'},
                ]
            }
            return subprocess.CompletedProcess(
                args=args, returncode=0, stdout=json.dumps(data), stderr=''
            )
        if args == ['mod', 'graph', '--output=json']:
            data = {
                'dependencies': [
                    {'name': 'foo-mod', 'version': '1.0.0'},
                    {'name': 'bar-tool', 'version': '2.0.0'},
                    {'name': 'dev-mod', 'version': '3.0.0'},
                ]
            }
            return subprocess.CompletedProcess(
                args=args, returncode=0, stdout=json.dumps(data), stderr=''
            )
        raise ValueError(f'Unexpected bazelisk args: {args}')

    @patch('requests.get')
    @patch('subprocess.run')
    def test_scan(self, mock_run, mock_get):
        """Test scanning bazel dependencies in MODULE.bazel."""
        mock_run.side_effect = self._mock_subprocess_run

        # Mock requests.get for BCR and Github
        def mock_requests_get(url, **_kwargs):
            mock_resp = MagicMock()
            mock_resp.status_code = 200

            # BCR Metadata
            if 'bcr.bazel.build' in url:
                if 'foo-mod' in url:
                    data = {'versions': ['1.0.0', '1.1.0', '2.0.0']}
                elif 'bar-tool' in url:
                    data = {'versions': ['2.0.0', '2.1.0']}
                elif 'dev-mod' in url:
                    data = {'versions': ['3.0.0']}
                else:
                    data = {}
                mock_resp.json.return_value = data

            else:
                mock_resp.json.return_value = {}

            return mock_resp

        mock_get.side_effect = mock_requests_get

        scanner = BazelDepScanner(self.test_dir)
        scanner._run_bazelisk = self._mock_run_bazelisk
        scanner.date = date(2026, 6, 10)

        results = list(scanner.pre_scan())

        self.assertEqual(len(results), 3)

        foo_res = next(r for r in results if r.package == 'foo-mod')
        self.assertEqual(foo_res.source, 'MODULE.bazel')
        self.assertEqual(foo_res.pkg_type, 'bazel_dep')
        self.assertEqual(
            foo_res.current, PackageVersion('1.0.0', date(2026, 1, 1))
        )
        self.assertEqual(
            foo_res.earliest, PackageVersion('1.1.0', date(2026, 2, 1))
        )
        self.assertEqual(foo_res.tier, 1)
        self.assertEqual(foo_res.owner, 'owner-foo@google.com')

        bar_res = next(r for r in results if r.package == 'bar-tool')
        self.assertEqual(bar_res.source, 'MODULE.bazel')
        self.assertEqual(bar_res.pkg_type, 'bazel_dep')
        self.assertEqual(
            bar_res.current, PackageVersion('2.0.0', date(2026, 2, 2))
        )
        self.assertEqual(
            bar_res.earliest, PackageVersion('2.0.0', date(2026, 2, 2))
        )
        self.assertEqual(bar_res.tier, 2)
        self.assertEqual(bar_res.owner, 'owner-bar@google.com')

        dev_res = next(r for r in results if r.package == 'dev-mod')
        self.assertEqual(dev_res.source, 'MODULE.bazel')
        self.assertEqual(dev_res.pkg_type, 'bazel_dep')
        self.assertEqual(
            dev_res.current, PackageVersion('3.0.0', date(2026, 3, 3))
        )
        self.assertEqual(
            dev_res.earliest, PackageVersion('3.0.0', date(2026, 3, 3))
        )
        self.assertEqual(dev_res.tier, 3)
        self.assertEqual(dev_res.owner, 'owner-dev@google.com')


if __name__ == '__main__':
    unittest.main()
