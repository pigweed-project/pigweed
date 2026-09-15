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
"""Tests for npm."""
# pylint: disable=protected-access

from datetime import date
import json
import os
import subprocess
import unittest
from unittest.mock import patch

from pyfakefs.fake_filesystem_unittest import TestCaseMixin
from pw_fortifier.async_path import AsyncPath
from pw_fortifier.npm import NpmAnalyzer
from pw_fortifier.freshness_result import PackageVersion, FreshnessResult
from pw_fortifier.pipeline_stage import PipelineSink
from pw_fortifier.scanner import configure_stage_for_test
from pw_fortifier.semver import SemVer


class TestNpmAnalyzer(unittest.IsolatedAsyncioTestCase, TestCaseMixin):
    """Tests for NpmAnalyzer."""

    def setUp(self):
        """Set up test environment."""
        self.setUpPyfakefs()
        self.test_dir = '/test'
        self.working_dir = '/working'
        self._date_patcher = None
        self.package_json_path = os.path.join(self.test_dir, 'package.json')

        self.package_json_content = (
            '{\n'
            '  "name": "test-project",\n'
            '  "dependencies": {\n'
            '    "foo-pkg": "^1.0.0"\n'
            '  },\n'
            '  "devDependencies": {\n'
            '    "bar-pkg": "^2.0.0"\n'
            '  }\n'
            '}\n'
        )
        self.fs.create_file(
            self.package_json_path, contents=self.package_json_content
        )

        self.package_lock_json_path = os.path.join(
            self.test_dir, 'package-lock.json'
        )
        self.package_lock_json_content = (
            '{\n'
            '  "packages": {\n'
            '    "": {\n'
            '      "dependencies": {\n'
            '        "foo-pkg": "^1.0.0"\n'
            '      },\n'
            '      "devDependencies": {\n'
            '        "bar-pkg": "^2.0.0"\n'
            '      }\n'
            '    }\n'
            '  }\n'
            '}\n'
        )
        self.fs.create_file(
            self.package_lock_json_path, contents=self.package_lock_json_content
        )

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
    def _mock_subprocess_run(cmd, **_kwargs):
        """Mock git and npm commands."""
        # Handle npm commands
        if cmd[0] == 'npm':
            if cmd[1] == 'view':
                pkg = cmd[2]
                if cmd[3] == 'time':
                    if pkg == 'foo-pkg':
                        time_data = {
                            'modified': '2026-06-05T12:00:00.000Z',
                            'created': '2026-01-01T12:00:00.000Z',
                            '1.0.0': '2026-01-02T12:00:00.000Z',
                            '1.1.0': '2026-02-01T12:00:00.000Z',
                            '2.0.0': '2026-06-05T12:00:00.000Z',
                        }
                    elif pkg == 'bar-pkg':
                        time_data = {
                            'modified': '2026-06-05T12:00:00.000Z',
                            'created': '2026-02-01T12:00:00.000Z',
                            '2.0.0': '2026-02-02T12:00:00.000Z',
                            '2.1.0': '2026-06-05T12:00:00.000Z',
                        }
                    else:
                        time_data = {}
                    return subprocess.CompletedProcess(
                        args=cmd,
                        returncode=0,
                        stdout=json.dumps(time_data),
                        stderr='',
                    )

        # Handle git commands
        if cmd[0] == 'git':
            if len(cmd) > 2 and cmd[1] == '-C':
                cmd = [cmd[0]] + cmd[3:]
            if cmd[1] == 'log':
                return subprocess.CompletedProcess(
                    args=cmd,
                    returncode=0,
                    stdout='hash1 Fake commit\n',
                    stderr='',
                )
            if cmd[1] == 'show':
                return subprocess.CompletedProcess(
                    args=cmd, returncode=0, stdout='package.json\n', stderr=''
                )
            if cmd[1] == 'blame':
                has_l_arg = '-L' in cmd
                if has_l_arg:
                    l_val = cmd[cmd.index('-L') + 1]
                    if l_val == '4,4':
                        stdout = (
                            'hash1 (<assignee-foo@google.com> '
                            '2026-06-05 10:00:00 +0000 4)   '
                            "\"foo-pkg\": \"^1.0.0\"\n"
                        )
                    elif l_val == '7,7':
                        stdout = (
                            'hash1 (<assignee-bar@google.com> '
                            '2026-06-05 10:00:00 +0000 7)   '
                            "\"bar-pkg\": \"^2.0.0\"\n"
                        )
                    else:
                        stdout = ''
                else:
                    stdout = ''
                return subprocess.CompletedProcess(
                    args=cmd, returncode=0, stdout=stdout, stderr=''
                )

        raise ValueError(f'Unexpected command: {cmd}')

    @patch('subprocess.run')
    @patch('pw_fortifier.find_core_owners.CoreOwnerFinder.core_members')
    async def test_run(self, mock_cores, mock_run):
        """Test scanning standard npm packages."""
        mock_run.side_effect = self._mock_subprocess_run
        mock_cores.return_value = {
            'assignee-foo@google.com',
            'assignee-bar@google.com',
        }

        analyzer = NpmAnalyzer()
        self.set_date(date(2026, 6, 10))

        consumer = PipelineSink()
        analyzer.connect(consumer)

        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            max_retries=0,
        )

        await analyzer.input_queue.put(
            AsyncPath(self.test_dir) / 'package.json'
        )
        await analyzer.input_queue.put(None)

        await analyzer.run()

        # Collect results
        results = []
        while True:
            result_path = await consumer.input_queue.get()
            if result_path is None:
                break
            result = await FreshnessResult.load(result_path)
            results.append(result)

        self.assertEqual(len(results), 2)

        foo_res = next(r for r in results if r.package == 'foo-pkg')
        self.assertEqual(foo_res.source, 'package.json')
        self.assertEqual(foo_res.location.file, 'package.json')
        self.assertEqual(foo_res.location.lines, (4, 4))
        self.assertEqual(foo_res.pkg_type, 'npm')
        self.assertEqual(
            foo_res.current, PackageVersion('1.0.0', date(2026, 1, 2))
        )
        self.assertEqual(
            foo_res.earliest, PackageVersion('1.1.0', date(2026, 2, 1))
        )
        self.assertEqual(foo_res.tier, 2)
        self.assertIsNone(foo_res.assignee)

        bar_res = next(r for r in results if r.package == 'bar-pkg')
        self.assertEqual(bar_res.source, 'package.json')
        self.assertEqual(bar_res.location.file, 'package.json')
        self.assertEqual(bar_res.location.lines, (7, 7))
        self.assertEqual(bar_res.pkg_type, 'npm')
        self.assertEqual(
            bar_res.current, PackageVersion('2.0.0', date(2026, 2, 2))
        )
        self.assertEqual(
            bar_res.earliest, PackageVersion('2.0.0', date(2026, 2, 2))
        )
        self.assertEqual(bar_res.tier, 2)
        self.assertIsNone(bar_res.assignee)

    @patch('subprocess.run')
    @patch('pw_fortifier.find_core_owners.CoreOwnerFinder.core_members')
    async def test_run_pw_web(self, mock_cores, mock_run):
        """Test scanning package.json under pw_web (tier 0)."""
        mock_run.side_effect = self._mock_subprocess_run
        mock_cores.return_value = {
            'assignee-foo@google.com',
            'assignee-bar@google.com',
        }

        # Create package.json under pw_web directory
        pw_web_dir = os.path.join(self.test_dir, 'pw_web')
        os.makedirs(pw_web_dir, exist_ok=True)
        pw_web_package_json = os.path.join(pw_web_dir, 'package.json')
        with open(pw_web_package_json, 'w') as f:
            f.write(self.package_json_content)

        pw_web_package_lock = os.path.join(pw_web_dir, 'package-lock.json')
        with open(pw_web_package_lock, 'w') as f:
            f.write(self.package_lock_json_content)

        analyzer = NpmAnalyzer()
        analyzer.add_on_device_module('pw_web')

        consumer = PipelineSink()
        analyzer.connect(consumer)

        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            max_retries=0,
        )

        await analyzer.input_queue.put(
            AsyncPath(self.test_dir) / 'pw_web/package.json'
        )
        await analyzer.input_queue.put(None)

        await analyzer.run()

        # Collect results
        results = []
        while True:
            result_path = await consumer.input_queue.get()
            if result_path is None:
                break
            result = await FreshnessResult.load(result_path)
            results.append(result)

        self.assertEqual(len(results), 2)

        foo_res = next(r for r in results if r.package == 'foo-pkg')
        # For pw_web, dependencies should be TIER0_ON_DEVICE (0)
        self.assertEqual(foo_res.tier, 0)

    @patch('subprocess.run')
    async def test_npm_not_found(self, mock_run):
        """Test scan when npm command is not found."""
        mock_run.side_effect = FileNotFoundError()
        analyzer = NpmAnalyzer()

        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            max_retries=0,
        )

        with self.assertRaises(FileNotFoundError) as ctx:
            await analyzer._process_one(
                AsyncPath(self.test_dir) / 'package.json'
            )
        self.assertIn('`npm` command not found', str(ctx.exception))
        self.assertIn('activate the Pigweed environment', str(ctx.exception))

    async def test_node_modules_skipped(self):
        """Test that packages inside node_modules are skipped."""
        node_modules_path = os.path.join(
            self.test_dir, 'node_modules', 'some-pkg', 'package.json'
        )
        os.makedirs(os.path.dirname(node_modules_path), exist_ok=True)
        with open(node_modules_path, 'w') as f:
            f.write(self.package_json_content)

        analyzer = NpmAnalyzer()

        consumer = PipelineSink()
        analyzer.connect(consumer)

        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            max_retries=0,
        )

        await analyzer._process_one(AsyncPath(node_modules_path))

        # It should not have sent anything, so consumer queue should be empty.
        self.assertTrue(consumer.input_queue.empty())

    async def test_invalid_semver_raises(self) -> None:
        """Test unparseable semver in package-lock raises ValueError."""
        analyzer = NpmAnalyzer()
        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            max_retries=0,
        )
        bad_lock = os.path.join(self.test_dir, 'bad-lock.json')
        self.fs.create_file(
            bad_lock,
            contents=json.dumps(
                {
                    'packages': {
                        '': {'dependencies': {'bad-pkg': 'not-a-semver'}}
                    }
                }
            ),
        )
        with self.assertRaises(ValueError) as ctx:
            await analyzer._get_resolved_versions(AsyncPath(bad_lock))
        self.assertIn(
            "Failed to parse semver for 'bad-pkg': 'not-a-semver'",
            str(ctx.exception),
        )

    async def test_npm_view_no_timestamps_raises(self) -> None:
        """Test empty timestamps from npm view raises RuntimeError."""
        analyzer = NpmAnalyzer()
        analyzer._versions['foo-pkg'] = (SemVer(1, 0, 0), 2)
        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            max_retries=0,
        )

        with patch.object(NpmAnalyzer, '_get_resolved_versions'):
            with patch.object(
                NpmAnalyzer, '_get_package_times', return_value={}
            ):
                with self.assertRaises(RuntimeError) as ctx:
                    await analyzer._process_one(
                        AsyncPath(self.test_dir) / 'package.json'
                    )
        self.assertIn('No timestamps returned', str(ctx.exception))

    async def test_no_valid_version_dates_raises(self) -> None:
        """Test no valid versions in time_info raises RuntimeError."""
        analyzer = NpmAnalyzer()
        analyzer._versions['foo-pkg'] = (SemVer(1, 0, 0), 2)
        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            max_retries=0,
        )

        with patch.object(NpmAnalyzer, '_get_resolved_versions'):
            with patch.object(
                NpmAnalyzer,
                '_get_package_times',
                return_value={'created': '2026-01-01T00:00:00Z'},
            ):
                with self.assertRaises(RuntimeError) as ctx:
                    await analyzer._process_one(
                        AsyncPath(self.test_dir) / 'package.json'
                    )
        self.assertIn('No valid version dates found', str(ctx.exception))

    async def test_current_version_missing_from_registry_raises(self) -> None:
        """Test current version missing in npm registry raises RuntimeError."""
        analyzer = NpmAnalyzer()
        analyzer._versions['foo-pkg'] = (SemVer(1, 0, 0), 2)
        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            max_retries=0,
        )

        with patch.object(NpmAnalyzer, '_get_resolved_versions'):
            with patch.object(
                NpmAnalyzer,
                '_get_package_times',
                return_value={'2.0.0': '2026-01-01T00:00:00Z'},
            ):
                with self.assertRaises(RuntimeError) as ctx:
                    await analyzer._process_one(
                        AsyncPath(self.test_dir) / 'package.json'
                    )
        self.assertIn('Timestamp for current version', str(ctx.exception))

    @patch('subprocess.run')
    @patch('pw_fortifier.find_core_owners.CoreOwnerFinder.core_members')
    async def test_run_prerelease_only(self, mock_cores, mock_run):
        """Test scanning package where all versions are pre-releases."""
        mock_cores.return_value = {'assignee-foo@google.com'}

        def _mock_run(cmd, **kwargs):
            if cmd[0] == 'npm' and cmd[1] == 'view' and cmd[3] == 'time':
                time_data = {
                    'modified': '2026-06-05T12:00:00.000Z',
                    'created': '2026-01-01T12:00:00.000Z',
                    '4.0.0-alpha.60': '2026-01-02T12:00:00.000Z',
                    '4.0.0-alpha.61': '2026-02-01T12:00:00.000Z',
                }
                return subprocess.CompletedProcess(
                    args=cmd,
                    returncode=0,
                    stdout=json.dumps(time_data),
                    stderr='',
                )
            if cmd[0] == 'git' and 'blame' in cmd:
                return subprocess.CompletedProcess(
                    args=cmd,
                    returncode=0,
                    stdout=(
                        'hash1 (<assignee-foo@google.com> '
                        '2026-06-05 10:00:00 +0000 3)   '
                        '"@material-ui/lab": "4.0.0-alpha.60"\n'
                    ),
                    stderr='',
                )
            return self._mock_subprocess_run(cmd, **kwargs)

        mock_run.side_effect = _mock_run

        pkg_dir = os.path.join(self.test_dir, 'mui_pkg')
        os.makedirs(pkg_dir, exist_ok=True)
        pkg_json = os.path.join(pkg_dir, 'package.json')
        with open(pkg_json, 'w') as f:
            f.write(
                '{\n'
                '  "dependencies": {\n'
                '    "@material-ui/lab": "4.0.0-alpha.60"\n'
                '  }\n'
                '}\n'
            )
        lock_json = os.path.join(pkg_dir, 'package-lock.json')
        with open(lock_json, 'w') as f:
            f.write(
                '{\n'
                '  "packages": {\n'
                '    "": {\n'
                '      "dependencies": {\n'
                '        "@material-ui/lab": "4.0.0-alpha.60"\n'
                '      }\n'
                '    }\n'
                '  }\n'
                '}\n'
            )

        analyzer = NpmAnalyzer()
        self.set_date(date(2026, 6, 10))

        consumer = PipelineSink()
        analyzer.connect(consumer)

        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            max_retries=0,
        )

        await analyzer.input_queue.put(AsyncPath(pkg_dir) / 'package.json')
        await analyzer.input_queue.put(None)

        await analyzer.run()

        result_path = await consumer.input_queue.get()
        self.assertIsNotNone(result_path)
        res = await FreshnessResult.load(result_path)
        self.assertEqual(res.package, '@material-ui/lab')
        self.assertEqual(
            res.current,
            PackageVersion('4.0.0-alpha.60', date(2026, 1, 2)),
        )
        self.assertEqual(
            res.earliest,
            PackageVersion('4.0.0-alpha.61', date(2026, 2, 1)),
        )

    async def test_missing_package_lock_json_returns(self) -> None:
        """Test missing package-lock.json causes _process_one to return."""
        analyzer = NpmAnalyzer()
        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            max_retries=0,
        )
        self.fs.create_file('/other/package.json', contents='{}')
        await analyzer._process_one(AsyncPath('/other/package.json'))
        self.assertEqual(len(analyzer._versions), 0)


if __name__ == '__main__':
    unittest.main()
