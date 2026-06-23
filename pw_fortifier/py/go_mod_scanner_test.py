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
"""Tests for go_mod_scanner."""
# pylint: disable=protected-access


from datetime import date
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from pw_fortifier.go_mod_scanner import GoModScanner
from pw_fortifier.package_scanner import PackageVersion


class TestGoModScanner(unittest.TestCase):
    """Tests for GoModScanner."""

    def setUp(self):
        """Set up test environment."""
        self.test_dir = tempfile.mkdtemp()
        self.go_mod = os.path.join(self.test_dir, 'go.mod')

        # Line 1: module
        # Line 3: go directive
        # Line 6: grpc (direct)
        # Line 7: grpc/examples (direct pseudo-version)
        # Line 8: protobuf (direct)
        # Line 11: net (indirect)
        self.go_mod_content = (
            'module pigweed.dev\n'
            '\n'
            'go 1.24.12\n'
            '\n'
            'require (\n'
            '\tgoogle.golang.org/grpc v1.62.0\n'
            '\tgoogle.golang.org/grpc/examples '
            'v0.0.0-20240717174749-64adc816bf5a\n'
            '\tgoogle.golang.org/protobuf v1.33.0\n'
            ')\n'
            '\n'
            'require (\n'
            '\tgolang.org/x/net v0.22.0 // indirect\n'
            ')\n'
        )
        with open(self.go_mod, 'w') as f:
            f.write(self.go_mod_content)

        # Write a fake OWNERS file
        self.owners_path = os.path.join(self.test_dir, 'OWNERS')
        with open(self.owners_path, 'w') as f:
            f.write(
                'owner-grpc@google.com\n'
                'owner-examples@google.com\n'
                'owner-protobuf@google.com\n'
            )

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
                    args=cmd, returncode=0, stdout='go.mod\n', stderr=''
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

                    if rel_path == 'go.mod':
                        if l_val == '6,6':
                            stdout = (
                                'hash1 (<owner-grpc@google.com> '
                                '2026-06-01 10:00:00 +0000 6) '
                                '\tgoogle.golang.org/grpc v1.62.0\n'
                            )
                        elif l_val == '7,7':
                            stdout = (
                                'hash1 (<owner-examples@google.com> '
                                '2026-06-01 10:00:00 +0000 7) '
                                '\tgoogle.golang.org/grpc/examples '
                                'v0.0.0-20240717174749-64adc816bf5a\n'
                            )
                        elif l_val == '8,8':
                            stdout = (
                                'hash1 (<owner-protobuf@google.com> '
                                '2026-06-01 10:00:00 +0000 8) '
                                '\tgoogle.golang.org/protobuf v1.33.0\n'
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
    def _mock_run_go(args, _cwd):
        """Mock go command execution."""
        # args is e.g. ['list', '-json', '-m', 'all']
        if args == ['list', '-json', '-m', 'all']:
            main_obj = {'Path': 'pigweed.dev', 'Main': True}
            grpc_obj = {
                'Path': 'google.golang.org/grpc',
                'Version': 'v1.62.0',
                'Time': '2024-03-01T12:00:00Z',
            }
            examples_obj = {
                'Path': 'google.golang.org/grpc/examples',
                'Version': 'v0.0.0-20240717174749-64adc816bf5a',
                'Time': '2024-07-17T17:47:49Z',
            }
            proto_obj = {
                'Path': 'google.golang.org/protobuf',
                'Version': 'v1.33.0',
                'Time': '2024-03-05T12:00:00Z',
            }
            net_obj = {
                'Path': 'golang.org/x/net',
                'Version': 'v0.22.0',
                'Time': '2024-03-02T12:00:00Z',
                'Indirect': True,
            }
            stdout = (
                json.dumps(main_obj)
                + '\n'
                + json.dumps(grpc_obj)
                + '\n'
                + json.dumps(examples_obj)
                + '\n'
                + json.dumps(proto_obj)
                + '\n'
                + json.dumps(net_obj)
                + '\n'
            )
            return subprocess.CompletedProcess(
                args=args, returncode=0, stdout=stdout, stderr=''
            )

        if len(args) == 5 and args[:4] == [
            'list',
            '-json',
            '-m',
            '-versions',
        ]:
            pkg_path = args[4]
            if pkg_path == 'google.golang.org/grpc':
                data = {'Versions': ['v1.61.0', 'v1.62.0', 'v1.63.0']}
            elif pkg_path == 'google.golang.org/protobuf':
                data = {'Versions': ['v1.32.0', 'v1.33.0']}
            else:
                data = {}
            return subprocess.CompletedProcess(
                args=args, returncode=0, stdout=json.dumps(data), stderr=''
            )

        if (
            len(args) == 4
            and args[:3] == ['list', '-json', '-m']
            and '@' in args[3]
        ):
            target = args[3]
            pkg_path, v = target.split('@')
            time_str = None
            if pkg_path == 'google.golang.org/grpc':
                if v == 'v1.61.0':
                    time_str = '2024-02-01T12:00:00Z'
                elif v == 'v1.62.0':
                    time_str = '2024-03-01T12:00:00Z'
                elif v == 'v1.63.0':
                    time_str = '2024-04-01T12:00:00Z'
            elif pkg_path == 'google.golang.org/protobuf':
                if v == 'v1.32.0':
                    time_str = '2024-02-05T12:00:00Z'
                elif v == 'v1.33.0':
                    time_str = '2024-03-05T12:00:00Z'

            if time_str:
                data = {'Path': pkg_path, 'Version': v, 'Time': time_str}
                return subprocess.CompletedProcess(
                    args=args, returncode=0, stdout=json.dumps(data), stderr=''
                )
            return subprocess.CompletedProcess(
                args=args, returncode=1, stdout='', stderr='Not found'
            )

        raise ValueError(f'Unexpected go command: {args}')

    @patch('subprocess.run')
    def test_scan(self, mock_run):
        """Test scanning Go modules."""
        mock_run.side_effect = self._mock_subprocess_run

        scanner = GoModScanner(self.test_dir)
        scanner._run_go = self._mock_run_go
        scanner.date = date(2024, 4, 10)

        results = list(scanner.pre_scan())

        self.assertEqual(len(results), 3)

        # 1. grpc (has update)
        grpc = next(r for r in results if r.package == 'google.golang.org/grpc')
        self.assertEqual(grpc.source, 'go.mod')
        self.assertEqual(grpc.pkg_type, 'go_mod')
        self.assertEqual(
            grpc.current, PackageVersion('v1.62.0', date(2024, 3, 1))
        )
        self.assertEqual(
            grpc.earliest, PackageVersion('v1.62.0', date(2024, 3, 1))
        )
        self.assertEqual(grpc.tier, 2)
        self.assertEqual(grpc.owner, 'owner-grpc@google.com')

        # 2. protobuf (no update, latest == current)
        proto = next(
            r for r in results if r.package == 'google.golang.org/protobuf'
        )
        self.assertEqual(proto.source, 'go.mod')
        self.assertEqual(proto.pkg_type, 'go_mod')
        self.assertEqual(
            proto.current, PackageVersion('v1.33.0', date(2024, 3, 5))
        )
        self.assertEqual(
            proto.earliest, PackageVersion('v1.33.0', date(2024, 3, 5))
        )
        self.assertEqual(proto.tier, 2)
        self.assertEqual(proto.owner, 'owner-protobuf@google.com')

        # 3. examples (pseudo-version, earliest == current)
        examples = next(
            r for r in results if r.package == 'google.golang.org/grpc/examples'
        )
        self.assertEqual(examples.source, 'go.mod')
        self.assertEqual(examples.pkg_type, 'go_mod')
        self.assertEqual(
            examples.current,
            PackageVersion(
                'v0.0.0-20240717174749-64adc816bf5a', date(2024, 7, 17)
            ),
        )
        self.assertEqual(
            examples.earliest,
            PackageVersion(
                'v0.0.0-20240717174749-64adc816bf5a', date(2024, 7, 17)
            ),
        )
        self.assertEqual(examples.tier, 2)
        self.assertEqual(examples.owner, 'owner-examples@google.com')


if __name__ == '__main__':
    unittest.main()
