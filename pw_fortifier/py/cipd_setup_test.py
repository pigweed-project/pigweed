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
"""Tests for cipd_setup."""
# pylint: disable=protected-access

from datetime import date
import json
import os
import subprocess
import unittest
from unittest.mock import patch

from pyfakefs.fake_filesystem_unittest import TestCaseMixin
from pw_fortifier.cipd_setup import CipdSetupAnalyzer
from pw_fortifier.freshness_result import PackageVersion, FreshnessResult
from pw_fortifier.pipeline_stage import PipelineSink
from pw_fortifier.scanner import configure_stage_for_test


class TestCipdSetupAnalyzer(unittest.IsolatedAsyncioTestCase, TestCaseMixin):
    """Tests for CipdSetupAnalyzer."""

    def setUp(self):
        """Set up test environment."""
        self.setUpPyfakefs()
        self.test_dir = '/test'
        self.working_dir = '/working'
        self._date_patcher = None
        self.cipd_setup_dir = os.path.join(
            self.test_dir, 'pw_env_setup', 'py', 'pw_env_setup', 'cipd_setup'
        )

        # Create root pigweed.json
        self.root_pigweed_json = os.path.join(self.test_dir, 'pigweed.json')
        root_pigweed_content = {
            'pw': {
                'pw_env_setup': {
                    'cipd_package_files': [
                        'pw_env_setup/py/pw_env_setup/cipd_setup/upstream.json'
                    ]
                }
            }
        }
        self.fs.create_file(
            self.root_pigweed_json, contents=json.dumps(root_pigweed_content)
        )

        # Write inclusions
        # default.json -> pigweed.json, other.json
        # upstream.json -> default.json, dev_tools.json
        # pigweed.json -> rust.json
        default_json = os.path.join(self.cipd_setup_dir, 'default.json')
        self.fs.create_file(
            default_json,
            contents=json.dumps(
                {'included_files': ['pigweed.json', 'other.json']}
            ),
        )

        other_json = os.path.join(self.cipd_setup_dir, 'other.json')
        self.fs.create_file(other_json, contents=json.dumps({'packages': []}))

        upstream_json = os.path.join(self.cipd_setup_dir, 'upstream.json')
        self.fs.create_file(
            upstream_json,
            contents=json.dumps(
                {'included_files': ['default.json', 'dev_tools.json']}
            ),
        )

        pigweed_json = os.path.join(self.cipd_setup_dir, 'pigweed.json')
        pigweed_json_content = {
            'included_files': ['rust.json'],
            'packages': [
                {
                    'path': 'fuchsia/third_party/ninja/${platform}',
                    'platforms': ['linux-amd64', 'mac-amd64'],
                    'tags': ['git_revision:ninja123'],
                },
                {
                    'path': 'fuchsia/third_party/3pp/bloaty/${platform}',
                    'platforms': ['linux-amd64'],
                    'tags': ['git_revision:bloaty123'],
                },
            ],
        }
        self.fs.create_file(
            pigweed_json, contents=json.dumps(pigweed_json_content)
        )

        rust_json = os.path.join(self.cipd_setup_dir, 'rust.json')
        rust_json_content = {
            'packages': [
                {
                    'path': 'fuchsia/third_party/rust/${platform}',
                    'platforms': ['linux-amd64'],
                    'tags': ['git_revision:rust123'],
                }
            ]
        }
        self.fs.create_file(rust_json, contents=json.dumps(rust_json_content))

        dev_tools_json = os.path.join(self.cipd_setup_dir, 'dev_tools.json')
        dev_tools_json_content = {
            'packages': [
                {
                    'path': 'fuchsia/third_party/dev_tool/${platform}',
                    'platforms': ['linux-amd64'],
                    'tags': ['git_revision:dev_tool123'],
                }
            ]
        }
        self.fs.create_file(
            dev_tools_json, contents=json.dumps(dev_tools_json_content)
        )

        ignored_json = os.path.join(self.cipd_setup_dir, 'ignored.json')
        ignored_json_content = {
            'packages': [
                {
                    'path': 'fuchsia/third_party/ignored/${platform}',
                    'platforms': ['linux-amd64'],
                    'tags': ['git_revision:ignored123'],
                }
            ]
        }
        self.fs.create_file(
            ignored_json, contents=json.dumps(ignored_json_content)
        )

        # Write a fake OWNERS file
        owners_path = os.path.join(self.test_dir, 'OWNERS')
        owners_content = (
            'assignee-ninja@google.com\n'
            'assignee-bloaty@google.com\n'
            'assignee-other@google.com\n'
        )
        self.fs.create_file(owners_path, contents=owners_content)

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

    def _mock_subprocess_run(self, cmd, **_kwargs):
        """Mock git commands."""
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
                    args=cmd, returncode=0, stdout='json content\n', stderr=''
                )
            if cmd[1] == 'blame':
                filepath = cmd[-1]
                if os.path.isabs(filepath):
                    rel_path = os.path.relpath(filepath, self.test_dir)
                else:
                    rel_path = filepath

                if 'pigweed.json' in rel_path:
                    stdout = (
                        'hash1 (<assignee-ninja@google.com> '
                        '2026-06-01 10:00:00 +0000 1) { ... }\n'
                    )
                elif 'rust.json' in rel_path:
                    stdout = (
                        'hash1 (<assignee-bloaty@google.com> '
                        '2026-06-01 10:00:00 +0000 1) { ... }\n'
                    )
                else:
                    stdout = (
                        'hash1 (<assignee-other@google.com> '
                        '2026-06-01 10:00:00 +0000 1) { ... }\n'
                    )
                return subprocess.CompletedProcess(
                    args=cmd, returncode=0, stdout=stdout, stderr=''
                )

        raise ValueError(f'Unexpected command: {cmd}')

    @staticmethod
    def _make_describe_output(pkg: str, date_str: str, tag: str) -> str:
        """Helper to format cipd describe output."""
        parts = tag.split(':')
        val = parts[1] if len(parts) > 1 else tag
        return (
            f'Package: {pkg}\n'
            f'Registered at: {date_str} 12:00:00 -0700 MST\n'
            'Tags:\n'
            f'  git_revision:{val}\n'
        )

    @staticmethod
    def _mock_cipd_ls(pkg: str) -> list[str]:
        if 'ninja' in pkg:
            return [
                'fuchsia/third_party/ninja/linux-amd64',
                'fuchsia/third_party/ninja/mac-amd64',
            ]
        if 'bloaty' in pkg:
            return [
                'fuchsia/third_party/3pp/bloaty/linux-amd64',
                'fuchsia/third_party/3pp/bloaty/mac-arm64',
            ]
        if 'rust' in pkg:
            return ['fuchsia/third_party/rust/linux-amd64']
        if 'dev_tool' in pkg:
            return ['fuchsia/third_party/dev_tool/linux-amd64']
        if 'shared' in pkg:
            return ['fuchsia/third_party/shared/linux-amd64']
        return []

    @staticmethod
    def _mock_cipd_instances(pkg: str) -> list[str]:
        parts = pkg.split('/')
        name = parts[-2] if len(parts) >= 2 else 'pkg'
        stdout = (
            'Instance ID                                   '
            'Registered by        Registered at\n'
            '----------------------------------------'
            '-----------------------------------------\n'
            f'git_revision:{name}_latest_hash                '
            'user@google.com      2026-06-08 12:00:00 -0700 MST\n'
            f'git_revision:{name}_mid_hash                   '
            'user@google.com      2026-06-06 12:00:00 -0700 MST\n'
            f'git_revision:{name}123                        '
            'user@google.com      2026-06-05 12:00:00 -0700 MST\n'
        )
        return [l.rstrip() for l in stdout.splitlines() if l.strip()]

    def _mock_cipd_describe(self, pkg: str, tag: str) -> list[str]:
        if '_latest_hash' in tag:
            stdout = self._make_describe_output(pkg, '2026-06-08', tag)
        elif '_mid_hash' in tag:
            stdout = self._make_describe_output(pkg, '2026-06-06', tag)
        else:
            tag_pkg_map = {
                'git_revision:ninja123': {
                    'fuchsia/third_party/ninja/linux-amd64',
                    'fuchsia/third_party/ninja/mac-amd64',
                },
                'git_revision:bloaty123': {
                    'fuchsia/third_party/3pp/bloaty/linux-amd64',
                },
                'git_revision:rust123': {
                    'fuchsia/third_party/rust/linux-amd64',
                },
                'git_revision:dev_tool123': {
                    'fuchsia/third_party/dev_tool/linux-amd64',
                },
                'git_revision:shared123': {
                    'fuchsia/third_party/shared/linux-amd64',
                },
            }
            tag_key = (
                tag
                if tag.startswith('git_revision:')
                else f'git_revision:{tag}'
            )
            if tag_key in tag_pkg_map and pkg in tag_pkg_map[tag_key]:
                stdout = self._make_describe_output(pkg, '2026-06-05', tag)
            else:
                raise ValueError(f'Unexpected tag/pkg combination: {tag}/{pkg}')

        return [l.rstrip() for l in stdout.splitlines() if l.strip()]

    async def _mock_run_cipd(self, args: list[str]) -> list[str]:
        """Mock cipd commands."""
        if args[0] == 'ls':
            return self._mock_cipd_ls(args[1])
        if args[0] == 'instances':
            return self._mock_cipd_instances(args[1])
        if args[0] == 'describe':
            return self._mock_cipd_describe(args[1], args[3])
        raise ValueError(f'Unexpected cipd args: {args}')

    @patch('pw_fortifier.cipd_utils._run_cipd')
    @patch('subprocess.run')
    async def test_run(self, mock_run, mock_cipd):
        """Test scanning starting from pigweed.json."""
        mock_run.side_effect = self._mock_subprocess_run
        mock_cipd.side_effect = self._mock_run_cipd

        analyzer = CipdSetupAnalyzer()
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

        # We expect 4 results:
        # - ninja -> TIER1 (from pigweed.json)
        # - bloaty -> TIER2 (from pigweed.json)
        # - rust -> TIER2 (from rust.json)
        # - dev_tool -> TIER3 (from dev_tools.json)
        self.assertEqual(len(results), 4)

        # 1. Ninja
        ninja = next(
            r for r in results if r.package == 'fuchsia/third_party/ninja'
        )
        self.assertEqual(ninja.tier, 1)
        self.assertIsNone(ninja.assignee)
        self.assertEqual(
            ninja.current,
            PackageVersion('git_revision:ninja123', date(2026, 6, 5)),
        )
        self.assertEqual(ninja.pkg_type, 'cipd_setup')
        self.assertEqual(
            ninja.source,
            os.path.normpath(
                'pw_env_setup/py/pw_env_setup/cipd_setup/pigweed.json'
            ),
        )
        self.assertEqual(ninja.location.file, ninja.source)

        # 2. Bloaty
        bloaty = next(
            r for r in results if r.package == 'fuchsia/third_party/3pp/bloaty'
        )
        self.assertEqual(bloaty.tier, 2)
        self.assertIsNone(bloaty.assignee)
        self.assertEqual(
            bloaty.current,
            PackageVersion('git_revision:bloaty123', date(2026, 6, 5)),
        )
        self.assertEqual(bloaty.pkg_type, 'cipd_setup')
        self.assertEqual(
            bloaty.source,
            os.path.normpath(
                'pw_env_setup/py/pw_env_setup/cipd_setup/pigweed.json'
            ),
        )
        self.assertEqual(bloaty.location.file, bloaty.source)

        # 3. Rust
        rust = next(
            r for r in results if r.package == 'fuchsia/third_party/rust'
        )
        self.assertEqual(rust.tier, 2)
        self.assertIsNone(rust.assignee)
        self.assertEqual(
            rust.current,
            PackageVersion('git_revision:rust123', date(2026, 6, 5)),
        )
        self.assertEqual(rust.pkg_type, 'cipd_setup')
        self.assertEqual(
            rust.source,
            os.path.normpath(
                'pw_env_setup/py/pw_env_setup/cipd_setup/rust.json'
            ),
        )
        self.assertEqual(rust.location.file, rust.source)

        # 4. Dev tool
        dev_tool = next(
            r for r in results if r.package == 'fuchsia/third_party/dev_tool'
        )
        self.assertEqual(dev_tool.tier, 3)
        self.assertIsNone(dev_tool.assignee)
        self.assertEqual(
            dev_tool.current,
            PackageVersion('git_revision:dev_tool123', date(2026, 6, 5)),
        )
        self.assertEqual(dev_tool.pkg_type, 'cipd_setup')
        self.assertEqual(
            dev_tool.source,
            os.path.normpath(
                'pw_env_setup/py/pw_env_setup/cipd_setup/dev_tools.json'
            ),
        )
        self.assertEqual(dev_tool.location.file, dev_tool.source)

    @patch('pw_fortifier.cipd_utils._run_cipd')
    @patch('subprocess.run')
    async def test_tier_override(self, mock_run, mock_cipd):
        """Tests tier 2 overrides tier 3 when reachable via both."""
        mock_run.side_effect = self._mock_subprocess_run

        # default.json -> shared.json AND pigweed.json, other.json
        default_json = os.path.join(self.cipd_setup_dir, 'default.json')
        with open(default_json, 'w') as f:
            json.dump(
                {
                    'included_files': [
                        'pigweed.json',
                        'other.json',
                        'shared.json',
                    ]
                },
                f,
            )

        # upstream.json -> default.json AND dev_tools.json AND shared.json
        upstream_json = os.path.join(self.cipd_setup_dir, 'upstream.json')
        with open(upstream_json, 'w') as f:
            json.dump(
                {
                    'included_files': [
                        'default.json',
                        'dev_tools.json',
                        'shared.json',
                    ]
                },
                f,
            )

        # Create shared.json
        shared_json = os.path.join(self.cipd_setup_dir, 'shared.json')
        shared_json_content = {
            'packages': [
                {
                    'path': 'fuchsia/third_party/shared/${platform}',
                    'platforms': ['linux-amd64'],
                    'tags': ['git_revision:shared123'],
                }
            ]
        }
        with open(shared_json, 'w') as f:
            json.dump(shared_json_content, f)

        mock_cipd.side_effect = self._mock_run_cipd
        analyzer = CipdSetupAnalyzer()
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

        results = []
        while True:
            result_path = await consumer.input_queue.get()
            if result_path is None:
                break
            result = await FreshnessResult.load(result_path)
            results.append(result)

        # Find the result for shared package
        shared_res = next(
            (r for r in results if r.package == 'fuchsia/third_party/shared'),
            None,
        )
        self.assertIsNotNone(shared_res)
        self.assertEqual(shared_res.tier, 2)  # TIER2_DEVHOST

    @patch('pw_fortifier.cipd_utils._run_cipd')
    @patch('subprocess.run')
    async def test_invalid_json_raises_error(self, mock_run, mock_cipd):
        """Test that invalid JSON in a CIPD setup file is fatal."""
        mock_run.side_effect = self._mock_subprocess_run
        mock_cipd.side_effect = self._mock_run_cipd

        invalid_json = os.path.join(self.cipd_setup_dir, 'other.json')
        with open(invalid_json, 'w') as f:
            f.write('{invalid json')

        analyzer = CipdSetupAnalyzer()
        consumer = PipelineSink()
        analyzer.connect(consumer)

        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            max_retries=0,
        )

        with self.assertRaises(json.JSONDecodeError):
            await analyzer.run()

    @patch('pw_fortifier.cipd_utils._run_cipd')
    @patch('subprocess.run')
    async def test_missing_file_raises_error(self, mock_run, mock_cipd):
        """Test that a missing included CIPD setup file is fatal."""
        mock_run.side_effect = self._mock_subprocess_run
        mock_cipd.side_effect = self._mock_run_cipd

        os.remove(os.path.join(self.cipd_setup_dir, 'other.json'))

        analyzer = CipdSetupAnalyzer()
        consumer = PipelineSink()
        analyzer.connect(consumer)

        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            max_retries=0,
        )

        with self.assertRaises(FileNotFoundError):
            await analyzer.run()

    @patch('pw_fortifier.cipd_utils._run_cipd')
    @patch('subprocess.run')
    async def test_invalid_package_structure_raises_error(
        self, mock_run, mock_cipd
    ):
        """Test that malformed package definitions are fatal."""
        mock_run.side_effect = self._mock_subprocess_run
        mock_cipd.side_effect = self._mock_run_cipd

        malformed_json = os.path.join(self.cipd_setup_dir, 'other.json')
        with open(malformed_json, 'w') as f:
            json.dump(
                {
                    'packages': [
                        {
                            'path': 12345,
                            'platforms': ['linux-amd64'],
                            'tags': ['git_revision:123'],
                        }
                    ]
                },
                f,
            )

        analyzer = CipdSetupAnalyzer()
        consumer = PipelineSink()
        analyzer.connect(consumer)

        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            max_retries=0,
        )

        with self.assertRaises(AssertionError):
            await analyzer.run()

    async def test_configure_skip_setup(self) -> None:
        """Tests configure sets skip_setup when pigweed.json is not in files."""
        analyzer = CipdSetupAnalyzer()
        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            files=None,
        )
        self.assertFalse(analyzer.skip_setup)

        analyzer = CipdSetupAnalyzer()
        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            files=['pigweed.json'],
        )
        self.assertFalse(analyzer.skip_setup)

        analyzer = CipdSetupAnalyzer()
        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            files=['other.txt'],
        )
        self.assertTrue(analyzer.skip_setup)


if __name__ == '__main__':
    unittest.main()
