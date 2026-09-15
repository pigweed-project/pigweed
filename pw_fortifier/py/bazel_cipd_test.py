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
"""Tests for bazel_cipd."""
# pylint: disable=protected-access


from datetime import date
import json
import os
import subprocess
import unittest
from unittest.mock import patch

from pyfakefs.fake_filesystem_unittest import TestCaseMixin
from pw_fortifier.bazel_cipd import BazelCipdAnalyzer
from pw_fortifier.freshness_result import PackageVersion, FreshnessResult
from pw_fortifier.pipeline_stage import PipelineSink
from pw_fortifier.scanner import configure_stage_for_test


class TestBazelCipdAnalyzer(unittest.IsolatedAsyncioTestCase, TestCaseMixin):
    """Tests for BazelCipdAnalyzer."""

    def setUp(self):
        """Set up test environment."""
        self.setUpPyfakefs()
        self.test_dir = '/test'
        self.working_dir = '/working'
        self.module_bazel = os.path.join(self.test_dir, 'MODULE.bazel')
        self._date_patcher = None

        self.module_bazel_content = (
            '# Fake MODULE.bazel\n'
            'use_repo(pw_cxx_toolchain, "llvm_toolchain")\n'
            'use_repo(cipd_ext, bloaty = "pigweed.bloaty")\n'
            'use_repo(other_ext, "other")\n'
        )
        self.fs.create_file(
            self.module_bazel, contents=self.module_bazel_content
        )

        # Write a fake OWNERS file
        self.owners_path = os.path.join(self.test_dir, 'OWNERS')
        self.fs.create_file(
            self.owners_path,
            contents='assignee-llvm@google.com\nassignee-bloaty@google.com\n',
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
                    args=cmd, returncode=0, stdout='MODULE.bazel\n', stderr=''
                )
            if cmd[1] == 'blame':
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
                                'hash1 (<assignee-llvm@google.com> '
                                '2026-06-01 10:00:00 +0000 2) '
                                'use_repo(pw_cxx_toolchain, '
                                '"llvm_toolchain")\n'
                            )
                        elif l_val == '3,3':
                            stdout = (
                                'hash1 (<assignee-bloaty@google.com> '
                                '2026-06-01 10:00:00 +0000 3) '
                                'use_repo(cipd_ext, '
                                'bloaty = "pigweed.bloaty")\n'
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

        raise ValueError(f'Unexpected command: {cmd}')

    @staticmethod
    async def _mock_run_bazelisk(args, _cwd):
        """Mock bazelisk commands."""
        if args[:2] == ['mod', 'dump_repo_mapping']:
            data = {
                'llvm_toolchain': '+pw_cxx_toolchain+llvm_toolchain',
                'bloaty': '+cipd+pigweed.bloaty',
                'other': '+other+other',
            }
            return subprocess.CompletedProcess(
                args=args, returncode=0, stdout=json.dumps(data), stderr=''
            )

        if args[0] == 'mod' and args[1] == 'show_repo':
            repos = {a for a in args[2:] if a.startswith('@')}
            expected_repos = {'@llvm_toolchain', '@bloaty', '@other'}
            if repos == expected_repos:
                llvm_obj = {
                    'canonicalName': '+pw_cxx_toolchain+llvm_toolchain',
                    'repoRuleName': 'cipd_repository',
                    'originalName': 'llvm_toolchain',
                    'attribute': [
                        {
                            'name': 'path',
                            'type': 'STRING',
                            'stringValue': (
                                'fuchsia/third_party/' 'clang/${os}-${arch}'
                            ),
                        },
                        {
                            'name': 'tag',
                            'type': 'STRING',
                            'stringValue': 'git_revision:123',
                        },
                    ],
                }
                # bloaty is package_repo, has packages which will be processed
                bloaty_obj = {
                    'canonicalName': '+cipd+pigweed.bloaty',
                    'repoRuleName': 'package_repo',
                    'originalName': 'pigweed.bloaty',
                    'attribute': [
                        {
                            'name': 'packages',
                            'type': 'STRING_DICT',
                            'stringDictValue': [
                                {
                                    'key': (
                                        'fuchsia/third_party/'
                                        '3pp/bloaty/${platform}'
                                    ),
                                    'value': 'git_revision:bloaty_123',
                                },
                                {
                                    'key': (
                                        'fuchsia/third_party/'
                                        '3pp/bloaty/common'
                                    ),
                                    'value': 'git_revision:bloaty_common_123',
                                },
                            ],
                        }
                    ],
                }
                other_obj = {
                    'canonicalName': '+other+other',
                    'repoRuleName': 'other_rule',
                    'originalName': 'other',
                    'attribute': [],
                }
                stdout = (
                    json.dumps(llvm_obj)
                    + '\n'
                    + json.dumps(bloaty_obj)
                    + '\n'
                    + json.dumps(other_obj)
                    + '\n'
                )
                return subprocess.CompletedProcess(
                    args=args, returncode=0, stdout=stdout, stderr=''
                )

        raise ValueError(f'Unexpected bazelisk args: {args}')

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
    def _mock_cipd_ls(args):
        if args[1] == 'fuchsia/third_party/clang':
            stdout = (
                'fuchsia/third_party/clang/linux-amd64\n'
                'fuchsia/third_party/clang/mac-amd64\n'
            )
            return subprocess.CompletedProcess(
                args=args, returncode=0, stdout=stdout, stderr=''
            )
        if args[1] == 'fuchsia/third_party/3pp/bloaty':
            stdout = (
                'fuchsia/third_party/3pp/bloaty/linux-amd64\n'
                'fuchsia/third_party/3pp/bloaty/mac-arm64\n'
            )
            return subprocess.CompletedProcess(
                args=args, returncode=0, stdout=stdout, stderr=''
            )
        return subprocess.CompletedProcess(
            args=args, returncode=0, stdout='', stderr=''
        )

    @staticmethod
    def _mock_cipd_instances(args):
        pkg = args[1]
        if 'clang' in pkg:
            latest_tag = 'git_revision:456'
            mid_tag = 'git_revision:clang_mid_hash'
            current_tag = 'git_revision:123'
        elif 'bloaty/linux-amd64' in pkg or 'bloaty/mac-arm64' in pkg:
            latest_tag = 'git_revision:bloaty_456'
            mid_tag = 'git_revision:bloaty_mid_hash'
            current_tag = 'git_revision:bloaty_123'
        elif 'bloaty/common' in pkg:
            latest_tag = 'git_revision:bloaty_common_456'
            mid_tag = 'git_revision:bloaty_common_mid_hash'
            current_tag = 'git_revision:bloaty_common_123'
        else:
            latest_tag = 'git_revision:unknown_latest'
            mid_tag = 'git_revision:unknown_mid'
            current_tag = 'git_revision:unknown_current'

        stdout = (
            'Instance ID                                   '
            'Registered by        Registered at\n'
            '----------------------------------------'
            '-----------------------------------------\n'
            f'{latest_tag}                                   '
            'user@google.com      2026-06-08 12:00:00 -0700 MST\n'
            f'{mid_tag}                                      '
            'user@google.com      2026-06-07 12:00:00 -0700 MST\n'
            f'{current_tag}                                  '
            'user@google.com      2026-06-05 12:00:00 -0700 MST\n'
        )
        return subprocess.CompletedProcess(
            args=args, returncode=0, stdout=stdout, stderr=''
        )

    def _mock_cipd_describe(self, args):
        pkg = args[1]
        tag = args[3]
        tag_key = (
            tag if tag.startswith('git_revision:') else f'git_revision:{tag}'
        )
        if '_mid_hash' in tag:
            stdout = self._make_describe_output(pkg, '2026-06-07', tag)
        else:
            tag_map = {
                'git_revision:123': {
                    'fuchsia/third_party/clang/linux-amd64': '2026-06-05',
                    'fuchsia/third_party/clang/mac-amd64': '2026-06-05',
                },
                'git_revision:bloaty_123': {
                    'fuchsia/third_party/3pp/bloaty/linux-amd64': '2026-06-06',
                    'fuchsia/third_party/3pp/bloaty/mac-arm64': '2026-06-06',
                },
                'git_revision:bloaty_common_123': {
                    'fuchsia/third_party/3pp/bloaty/common': '2026-06-06',
                },
                'git_revision:456': {
                    'fuchsia/third_party/clang/linux-amd64': '2026-06-08',
                    'fuchsia/third_party/clang/mac-amd64': '2026-06-08',
                },
                'git_revision:bloaty_456': {
                    'fuchsia/third_party/3pp/bloaty/linux-amd64': '2026-06-08',
                    'fuchsia/third_party/3pp/bloaty/mac-arm64': '2026-06-08',
                },
                'git_revision:bloaty_common_456': {
                    'fuchsia/third_party/3pp/bloaty/common': '2026-06-08',
                },
            }
            pkg_map = tag_map.get(tag_key)
            if pkg_map and pkg in pkg_map:
                stdout = self._make_describe_output(pkg, pkg_map[pkg], tag)
            else:
                raise ValueError(f'Unexpected tag/pkg combination: {tag}/{pkg}')

        return subprocess.CompletedProcess(
            args=args, returncode=0, stdout=stdout, stderr=''
        )

    async def _mock_run_cipd(self, args):
        """Mock cipd commands."""
        subcommands = {
            'ls': self._mock_cipd_ls,
            'instances': self._mock_cipd_instances,
            'describe': self._mock_cipd_describe,
        }
        subcmd = subcommands.get(args[0])
        if subcmd:
            proc = subcmd(args)
            return [l.rstrip() for l in proc.stdout.splitlines() if l.strip()]
        raise ValueError(f'Unexpected cipd args: {args}')

    @patch('pw_fortifier.cipd_utils._run_cipd')
    @patch('pw_fortifier.bazelisk_utils.run_bazelisk')
    @patch('subprocess.run')
    async def test_run(self, mock_run, mock_bazelisk, mock_cipd):
        """Test successful scan of CIPD repos in MODULE.bazel."""
        mock_run.side_effect = self._mock_subprocess_run
        mock_bazelisk.side_effect = self._mock_run_bazelisk
        mock_cipd.side_effect = self._mock_run_cipd

        analyzer = BazelCipdAnalyzer()
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

        # We expect 3 results:
        # 1 from llvm_toolchain (clang)
        # 2 from bloaty (bloaty, bloaty/common)
        self.assertEqual(len(results), 3)

        # 1. Clang
        clang = next(
            r for r in results if r.package == 'fuchsia/third_party/clang'
        )
        self.assertEqual(clang.source, 'MODULE.bazel')
        self.assertEqual(clang.location.file, 'MODULE.bazel')
        self.assertEqual(clang.location.lines, (2, 2))
        self.assertEqual(clang.pkg_type, 'bazel_cipd')
        self.assertEqual(
            clang.current,
            PackageVersion('git_revision:123', date(2026, 6, 5)),
        )
        self.assertEqual(
            clang.earliest,
            PackageVersion('git_revision:123', date(2026, 6, 5)),
        )
        self.assertEqual(clang.tier, 1)
        self.assertIsNone(clang.assignee)

        # 2. Bloaty
        bloaty = next(
            r for r in results if r.package == 'fuchsia/third_party/3pp/bloaty'
        )
        self.assertEqual(bloaty.source, 'MODULE.bazel')
        self.assertEqual(bloaty.location.file, 'MODULE.bazel')
        self.assertEqual(bloaty.location.lines, (3, 3))
        self.assertEqual(bloaty.pkg_type, 'bazel_cipd')
        self.assertEqual(
            bloaty.current,
            PackageVersion('git_revision:bloaty_123', date(2026, 6, 6)),
        )
        self.assertEqual(
            bloaty.earliest,
            PackageVersion('git_revision:bloaty_123', date(2026, 6, 6)),
        )
        self.assertEqual(bloaty.tier, 1)
        self.assertIsNone(bloaty.assignee)

        # 3. Bloaty common (from package_repo, does NOT match os-arch ->
        # added directly).
        bloaty_common = next(
            r
            for r in results
            if r.package == 'fuchsia/third_party/3pp/bloaty/common'
        )
        self.assertEqual(bloaty_common.source, 'MODULE.bazel')
        self.assertEqual(bloaty_common.location.file, 'MODULE.bazel')
        self.assertEqual(bloaty_common.location.lines, (3, 3))
        self.assertEqual(bloaty_common.pkg_type, 'bazel_cipd')
        self.assertEqual(
            bloaty_common.current,
            PackageVersion('git_revision:bloaty_common_123', date(2026, 6, 6)),
        )
        self.assertEqual(
            bloaty_common.earliest,
            PackageVersion('git_revision:bloaty_common_123', date(2026, 6, 6)),
        )
        self.assertEqual(bloaty_common.tier, 1)
        self.assertIsNone(bloaty_common.assignee)

    @patch('pw_fortifier.bazelisk_utils.run_bazelisk')
    @patch('subprocess.run')
    async def test_missing_attributes_fails(self, mock_run, mock_bazelisk):
        """Test scan fails when cipd repository is missing attributes."""

        async def mock_bazelisk_with_malformed(args, _cwd):
            if args[:2] == ['mod', 'dump_repo_mapping']:
                data = {'other': '+other+other'}
                return subprocess.CompletedProcess(
                    args=args, returncode=0, stdout=json.dumps(data), stderr=''
                )
            if args[0] == 'mod' and args[1] == 'show_repo':
                malformed_obj = {
                    'canonicalName': '+other+other',
                    'repoRuleName': 'cipd_repository',
                    'originalName': 'other',
                    'attribute': [],
                }
                stdout = json.dumps(malformed_obj) + '\n'
                return subprocess.CompletedProcess(
                    args=args, returncode=0, stdout=stdout, stderr=''
                )
            raise ValueError(f'Unexpected bazelisk args: {args}')

        mock_run.side_effect = self._mock_subprocess_run
        mock_bazelisk.side_effect = mock_bazelisk_with_malformed

        analyzer = BazelCipdAnalyzer()

        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            max_retries=0,
        )

        with self.assertRaises(AssertionError):
            await analyzer._set_up()

    @patch('pw_fortifier.bazelisk_utils.run_bazelisk')
    @patch('subprocess.run')
    async def test_cipd_failed(self, mock_run, mock_bazelisk):
        """Test scan when cipd command fails."""
        mock_bazelisk.side_effect = self._mock_run_bazelisk

        def mock_subprocess_run(cmd, **_kwargs):
            if cmd[0] == 'cipd':
                raise subprocess.CalledProcessError(
                    1, cmd, stderr='mocked cipd error'
                )
            if cmd[0] == 'git':
                return self._mock_subprocess_run(cmd, **_kwargs)
            raise ValueError(f'Unexpected command in test_cipd_failed: {cmd}')

        mock_run.side_effect = mock_subprocess_run

        analyzer = BazelCipdAnalyzer()

        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            max_retries=0,
        )

        with self.assertRaises(RuntimeError) as ctx:
            await analyzer._set_up()
        self.assertIn('cipd failed', str(ctx.exception))

    async def test_configure_skip_setup(self) -> None:
        """Tests configure sets skip_setup when MODULE.bazel is not in files."""
        analyzer = BazelCipdAnalyzer()
        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            files=None,
        )
        self.assertFalse(analyzer.skip_setup)

        analyzer = BazelCipdAnalyzer()
        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            files=['MODULE.bazel'],
        )
        self.assertFalse(analyzer.skip_setup)

        analyzer = BazelCipdAnalyzer()
        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            files=['other.txt'],
        )
        self.assertTrue(analyzer.skip_setup)


if __name__ == '__main__':
    unittest.main()
