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
"""Tests for bazel_cipd_scanner."""
# pylint: disable=protected-access


from datetime import date
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from pw_fortifier.bazel_cipd_scanner import BazelCipdScanner
from pw_fortifier.package_scanner import PackageVersion


class TestBazelCipdScanner(unittest.TestCase):
    """Tests for BazelCipdScanner."""

    def setUp(self):
        """Set up test environment."""
        self.test_dir = tempfile.mkdtemp()
        self.module_bazel = os.path.join(self.test_dir, 'MODULE.bazel')

        # Line 2: llvm_toolchain
        # Line 3: bloaty
        # Line 4: other
        self.module_bazel_content = (
            '# Fake MODULE.bazel\n'
            'use_repo(pw_cxx_toolchain, "llvm_toolchain")\n'
            'use_repo(cipd_ext, bloaty = "pigweed.bloaty")\n'
            'use_repo(other_ext, "other")\n'
        )
        with open(self.module_bazel, 'w') as f:
            f.write(self.module_bazel_content)

        # Write a fake OWNERS file
        self.owners_path = os.path.join(self.test_dir, 'OWNERS')
        with open(self.owners_path, 'w') as f:
            f.write('owner-llvm@google.com\nowner-bloaty@google.com\n')

    def tearDown(self):
        """Tear down test environment."""
        shutil.rmtree(self.test_dir)

    def _mock_subprocess_run(self, cmd, **_kwargs):
        """Mock git commands."""
        if cmd[0] == 'git':
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
                                'hash1 (<owner-llvm@google.com> '
                                '2026-06-01 10:00:00 +0000 2) '
                                'use_repo(pw_cxx_toolchain, '
                                '"llvm_toolchain")\n'
                            )
                        elif l_val == '3,3':
                            stdout = (
                                'hash1 (<owner-bloaty@google.com> '
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
    def _mock_run_bazelisk(args, _cwd):
        """Mock bazelisk commands."""
        if args == ['mod', 'dump_repo_mapping', '']:
            data = {
                'llvm_toolchain': '+pw_cxx_toolchain+llvm_toolchain',
                'bloaty': '+cipd+pigweed.bloaty',
                'other': '+other+other',
            }
            return subprocess.CompletedProcess(
                args=args, returncode=0, stdout=json.dumps(data), stderr=''
            )

        if args[0] == 'mod' and args[1] == 'show_repo':
            repos = set(args[2:-1])
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
                                        'bloaty/linux-amd64'
                                    ),
                                    'value': 'git_revision:bloaty_123',
                                },
                                {
                                    'key': 'fuchsia/third_party/bloaty/common',
                                    'value': 'git_revision:bloaty_common_123',
                                },
                            ],
                        }
                    ],
                }
                other_obj = {
                    'canonicalName': '+other+other',
                    'repoRuleName': 'cipd_repository',
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
        if args[1] == 'fuchsia/third_party/bloaty':
            stdout = (
                'fuchsia/third_party/bloaty/linux-amd64\n'
                'fuchsia/third_party/bloaty/mac-amd64\n'
            )
            return subprocess.CompletedProcess(
                args=args, returncode=0, stdout=stdout, stderr=''
            )
        raise ValueError(f'Unexpected cipd ls path: {args[1]}')

    @staticmethod
    def _mock_cipd_instances(args):
        pkg = args[1]
        if 'clang' in pkg:
            latest_tag = 'git_revision:456'
            mid_tag = 'git_revision:clang_mid_hash'
            current_tag = 'git_revision:123'
        elif 'bloaty/linux-amd64' in pkg or 'bloaty/mac-amd64' in pkg:
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
            "Instance ID                                   "
            "Registered by        Registered at\n"
            "----------------------------------------"
            "-----------------------------------------\n"
            f"{latest_tag}                                   "
            "user@google.com      2026-06-08 12:00:00 -0700 MST\n"
            f"{mid_tag}                                      "
            "user@google.com      2026-06-07 12:00:00 -0700 MST\n"
            f"{current_tag}                                  "
            "user@google.com      2026-06-05 12:00:00 -0700 MST\n"
        )
        return subprocess.CompletedProcess(
            args=args, returncode=0, stdout=stdout, stderr=''
        )

    def _mock_cipd_describe(self, args):
        pkg = args[1]
        tag = args[3]
        if '_mid_hash' in tag:
            stdout = self._make_describe_output(pkg, '2026-06-07', tag)
        else:
            tag_map = {
                'git_revision:123': {
                    'fuchsia/third_party/clang/linux-amd64': '2026-06-05',
                    'fuchsia/third_party/clang/mac-amd64': '2026-06-05',
                },
                'git_revision:bloaty_123': {
                    'fuchsia/third_party/bloaty/linux-amd64': '2026-06-06',
                    'fuchsia/third_party/bloaty/mac-amd64': '2026-06-06',
                },
                'git_revision:bloaty_common_123': {
                    'fuchsia/third_party/bloaty/common': '2026-06-06',
                },
                'git_revision:456': {
                    'fuchsia/third_party/clang/linux-amd64': '2026-06-08',
                    'fuchsia/third_party/clang/mac-amd64': '2026-06-08',
                },
                'git_revision:bloaty_456': {
                    'fuchsia/third_party/bloaty/linux-amd64': '2026-06-08',
                    'fuchsia/third_party/bloaty/mac-amd64': '2026-06-08',
                },
                'git_revision:bloaty_common_456': {
                    'fuchsia/third_party/bloaty/common': '2026-06-08',
                },
            }
            pkg_map = tag_map.get(tag)
            if pkg_map and pkg in pkg_map:
                stdout = self._make_describe_output(pkg, pkg_map[pkg], tag)
            else:
                raise ValueError(f'Unexpected tag/pkg combination: {tag}/{pkg}')

        return subprocess.CompletedProcess(
            args=args, returncode=0, stdout=stdout, stderr=''
        )

    def _mock_run_cipd(self, args):
        """Mock cipd commands."""
        subcommands = {
            'ls': self._mock_cipd_ls,
            'instances': self._mock_cipd_instances,
            'describe': self._mock_cipd_describe,
        }
        subcmd = subcommands.get(args[0])
        if subcmd:
            return subcmd(args)
        raise ValueError(f'Unexpected cipd args: {args}')

    @patch('subprocess.run')
    @patch('pw_fortifier.bazel_cipd_scanner._LOG')
    def test_scan(self, mock_log, mock_run):
        """Test successful scan of CIPD repos in MODULE.bazel."""
        mock_run.side_effect = self._mock_subprocess_run

        scanner = BazelCipdScanner(self.test_dir)
        scanner._run_bazelisk = self._mock_run_bazelisk
        scanner._run_cipd = self._mock_run_cipd
        scanner.date = date(2026, 6, 10)

        results = list(scanner.pre_scan())

        # We expect 4 results:
        # 2 from llvm_toolchain (linux, mac)
        # 2 from bloaty (linux, common)
        self.assertEqual(len(results), 4)

        mock_log.warning.assert_any_call(
            "the version of %s couldn't be determined", '+other+other'
        )

        # 1. Clang linux-amd64
        clang_linux = next(
            r
            for r in results
            if r.package == 'fuchsia/third_party/clang/linux-amd64'
        )
        self.assertEqual(clang_linux.source, 'MODULE.bazel')
        self.assertEqual(clang_linux.pkg_type, 'bazel_cipd')
        self.assertEqual(
            clang_linux.current,
            PackageVersion('git_revision:123', date(2026, 6, 5)),
        )
        self.assertEqual(
            clang_linux.earliest,
            PackageVersion('git_revision:123', date(2026, 6, 5)),
        )
        self.assertEqual(clang_linux.tier, 1)
        self.assertEqual(clang_linux.owner, 'owner-llvm@google.com')

        # 2. Clang mac-amd64
        clang_mac = next(
            r
            for r in results
            if r.package == 'fuchsia/third_party/clang/mac-amd64'
        )
        self.assertEqual(clang_mac.tier, 1)
        self.assertEqual(
            clang_mac.current,
            PackageVersion('git_revision:123', date(2026, 6, 5)),
        )
        self.assertEqual(
            clang_mac.earliest,
            PackageVersion('git_revision:123', date(2026, 6, 5)),
        )
        self.assertEqual(clang_mac.owner, 'owner-llvm@google.com')

        # 3. Bloaty linux-amd64
        bloaty_linux = next(
            r
            for r in results
            if r.package == 'fuchsia/third_party/bloaty/linux-amd64'
        )
        self.assertEqual(bloaty_linux.source, 'MODULE.bazel')
        self.assertEqual(bloaty_linux.pkg_type, 'bazel_cipd')
        self.assertEqual(
            bloaty_linux.current,
            PackageVersion('git_revision:bloaty_123', date(2026, 6, 6)),
        )
        self.assertEqual(
            bloaty_linux.earliest,
            PackageVersion('git_revision:bloaty_123', date(2026, 6, 6)),
        )
        self.assertEqual(bloaty_linux.tier, 1)
        self.assertEqual(bloaty_linux.owner, 'owner-bloaty@google.com')

        # 4. Bloaty common (from package_repo, does NOT match os-arch ->
        # added directly).
        bloaty_common = next(
            r
            for r in results
            if r.package == 'fuchsia/third_party/bloaty/common'
        )
        self.assertEqual(bloaty_common.source, 'MODULE.bazel')
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
        self.assertEqual(bloaty_common.owner, 'owner-bloaty@google.com')

    @patch('subprocess.run')
    def test_cipd_failed(self, mock_run):
        """Test scan when cipd command fails."""

        def mock_subprocess_run(cmd, **_kwargs):
            if cmd[0] == 'cipd':
                return subprocess.CompletedProcess(
                    args=cmd,
                    returncode=1,
                    stdout='',
                    stderr='mocked cipd error',
                )
            if cmd[0] == 'git':
                return self._mock_subprocess_run(cmd, **_kwargs)
            raise ValueError(f'Unexpected command in test_cipd_failed: {cmd}')

        mock_run.side_effect = mock_subprocess_run

        scanner = BazelCipdScanner(self.test_dir)
        scanner._run_bazelisk = self._mock_run_bazelisk

        with self.assertRaises(RuntimeError) as ctx:
            list(scanner.pre_scan())
        self.assertIn('activate the Pigweed environment', str(ctx.exception))


if __name__ == '__main__':
    unittest.main()
