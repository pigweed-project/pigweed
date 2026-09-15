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
"""Tests for bazel_dep."""
# pylint: disable=protected-access

from datetime import date
import json
import os
import subprocess
import unittest
from unittest.mock import patch, MagicMock

from pyfakefs.fake_filesystem_unittest import TestCaseMixin
from pw_fortifier.bazel_dep import BazelDepAnalyzer
from pw_fortifier.freshness_result import PackageVersion, FreshnessResult
from pw_fortifier.pipeline_stage import PipelineSink
from pw_fortifier.scanner import configure_stage_for_test


class TestBazelDepAnalyzer(unittest.IsolatedAsyncioTestCase, TestCaseMixin):
    """Tests for BazelDepAnalyzer."""

    def setUp(self):
        """Set up test environment."""
        self.setUpPyfakefs()
        self.test_dir = '/test'
        self.working_dir = '/working'
        self.module_bazel = os.path.join(self.test_dir, 'MODULE.bazel')
        self._date_patcher = None

        self.module_bazel_content = (
            '# Fake MODULE.bazel\n'
            'bazel_dep(name = "foo-mod", version = "1.0.0")\n'
            'bazel_dep(name = "bar-tool", version = "2.0.0")\n'
            'bazel_dep('
            'name = "dev-mod", version = "3.0.0", dev_dependency = True'
            ')\n'
        )
        self.fs.create_file(
            self.module_bazel, contents=self.module_bazel_content
        )

        # Write a fake OWNERS file
        self.owners_path = os.path.join(self.test_dir, 'OWNERS')
        owners_content = (
            'assignee-foo@google.com\n'
            'assignee-bar@google.com\n'
            'assignee-dev@google.com\n'
        )
        self.fs.create_file(self.owners_path, contents=owners_content)

    def tearDown(self):
        """Tear down test environment."""
        if self._date_patcher is not None:
            self._date_patcher.stop()

    def set_date(self, d):
        """Sets the mocked package analyzer date."""
        if self._date_patcher is not None:
            self._date_patcher.stop()
        self._date_patcher = patch('pw_fortifier.package_analyzer.DATE', d)
        self._date_patcher.start()

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
                        'hash1 (<assignee-foo@google.com> '
                        '2026-06-01 10:00:00 +0000 2) '
                        'bazel_dep(name = "foo-mod", '
                        'version = "1.0.0")\n'
                    )
                elif l_val == '3,3':
                    stdout = (
                        'hash1 (<assignee-bar@google.com> '
                        '2026-06-01 10:00:00 +0000 3) '
                        'bazel_dep(name = "bar-tool", '
                        'version = "2.0.0")\n'
                    )
                elif l_val == '4,4':
                    stdout = (
                        'hash1 (<assignee-dev@google.com> '
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
            args = cmd[1:]
            if args and args[0] == '-C':
                args = args[2:]
            if args:
                subcommands = {
                    'log': self._mock_git_log,
                    'show': self._mock_git_show,
                    'blame': self._mock_git_blame,
                    'clone': self._mock_git_clone,
                }
                subcmd = subcommands.get(args[0])
                if subcmd:
                    return subcmd(cmd, **kwargs)

        raise ValueError(f'Unexpected command: {cmd}')

    @staticmethod
    async def _mock_run_bazelisk(args, _cwd):
        """Mock bazelisk commands."""
        if args == [
            'mod',
            'graph',
            '--lockfile_mode=off',
            '--ignore_dev_dependency',
            '--output=json',
        ]:
            data = {
                'dependencies': [
                    {'name': 'foo-mod', 'version': '1.0.0'},
                    {'name': 'bar-tool', 'version': '2.0.0'},
                ]
            }
            return subprocess.CompletedProcess(
                args=args, returncode=0, stdout=json.dumps(data), stderr=''
            )
        if args == ['mod', 'graph', '--lockfile_mode=off', '--output=json']:
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

    @patch('pw_fortifier.bazel_dep.run_bazelisk')
    @patch('requests.get')
    @patch('subprocess.run')
    async def test_run(self, mock_run, mock_get, mock_bazelisk):
        """Test scanning bazel dependencies in MODULE.bazel."""
        mock_run.side_effect = self._mock_subprocess_run
        mock_bazelisk.side_effect = self._mock_run_bazelisk

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

        analyzer = BazelDepAnalyzer()
        self.set_date(date(2026, 6, 10))

        consumer = PipelineSink()
        analyzer.connect(consumer)

        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            max_retries=0,
        )
        await analyzer.run()

        # Collect results
        results = []
        while True:
            result_path = await consumer.input_queue.get()
            if result_path is None:
                break
            result = await FreshnessResult.load(result_path)
            results.append(result)

        self.assertEqual(len(results), 3)

        foo_res = next(r for r in results if r.package == 'foo-mod')
        self.assertEqual(foo_res.source, 'MODULE.bazel')
        self.assertEqual(foo_res.location.file, 'MODULE.bazel')
        self.assertEqual(foo_res.location.lines, (2, 2))
        self.assertEqual(foo_res.pkg_type, 'bazel_dep')
        self.assertEqual(
            foo_res.current, PackageVersion('1.0.0', date(2026, 1, 1))
        )
        self.assertEqual(
            foo_res.earliest, PackageVersion('1.1.0', date(2026, 2, 1))
        )
        self.assertEqual(foo_res.tier, 0)
        self.assertIsNone(foo_res.assignee)

        bar_res = next(r for r in results if r.package == 'bar-tool')
        self.assertEqual(bar_res.source, 'MODULE.bazel')
        self.assertEqual(bar_res.location.file, 'MODULE.bazel')
        self.assertEqual(bar_res.location.lines, (3, 3))
        self.assertEqual(bar_res.pkg_type, 'bazel_dep')
        self.assertEqual(
            bar_res.current, PackageVersion('2.0.0', date(2026, 2, 2))
        )
        self.assertEqual(
            bar_res.earliest, PackageVersion('2.0.0', date(2026, 2, 2))
        )
        self.assertEqual(bar_res.tier, 2)
        self.assertIsNone(bar_res.assignee)

        dev_res = next(r for r in results if r.package == 'dev-mod')
        self.assertEqual(dev_res.source, 'MODULE.bazel')
        self.assertEqual(dev_res.location.file, 'MODULE.bazel')
        self.assertEqual(dev_res.location.lines, (4, 4))
        self.assertEqual(dev_res.pkg_type, 'bazel_dep')
        self.assertEqual(
            dev_res.current, PackageVersion('3.0.0', date(2026, 3, 3))
        )
        self.assertEqual(
            dev_res.earliest, PackageVersion('3.0.0', date(2026, 3, 3))
        )
        self.assertEqual(dev_res.tier, 3)
        self.assertIsNone(dev_res.assignee)

    async def test_configure_skip_setup(self) -> None:
        """Tests configure sets skip_setup when MODULE.bazel is not in files."""
        analyzer = BazelDepAnalyzer()
        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            files=None,
        )
        self.assertFalse(analyzer.skip_setup)

        analyzer = BazelDepAnalyzer()
        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            files=['MODULE.bazel'],
        )
        self.assertFalse(analyzer.skip_setup)

        analyzer = BazelDepAnalyzer()
        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            files=['other.txt'],
        )
        self.assertTrue(analyzer.skip_setup)
        consumer = PipelineSink()
        analyzer.connect(consumer)
        await analyzer.run()
        self.assertIsNone(await consumer.input_queue.get())


if __name__ == '__main__':
    unittest.main()
