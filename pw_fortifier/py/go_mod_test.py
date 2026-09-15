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
"""Tests for go_mod."""
# pylint: disable=protected-access


from datetime import date
import json
import os
import subprocess
import unittest
from unittest.mock import patch, MagicMock, AsyncMock

from pyfakefs.fake_filesystem_unittest import TestCaseMixin
from pw_fortifier.go_mod import GoModAnalyzer
from pw_fortifier.freshness_result import PackageVersion, FreshnessResult
from pw_fortifier.pipeline_stage import PipelineSink
from pw_fortifier.scanner import configure_stage_for_test


class TestGoModAnalyzer(unittest.IsolatedAsyncioTestCase, TestCaseMixin):
    """Tests for GoModAnalyzer."""

    def setUp(self):
        """Set up test environment."""
        self.setUpPyfakefs()
        self.test_dir = '/test'
        self.working_dir = '/working'
        self._date_patcher = None
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
        self.fs.create_file(self.go_mod, contents=self.go_mod_content)

        # Write a fake OWNERS file
        self.owners_path = os.path.join(self.test_dir, 'OWNERS')
        owners_content = (
            'assignee-grpc@google.com\n'
            'assignee-examples@google.com\n'
            'assignee-protobuf@google.com\n'
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
                                'hash1 (<assignee-grpc@google.com> '
                                '2026-06-01 10:00:00 +0000 6) '
                                '\tgoogle.golang.org/grpc v1.62.0\n'
                            )
                        elif l_val == '7,7':
                            stdout = (
                                'hash1 (<assignee-examples@google.com> '
                                '2026-06-01 10:00:00 +0000 7) '
                                '\tgoogle.golang.org/grpc/examples '
                                'v0.0.0-20240717174749-64adc816bf5a\n'
                            )
                        elif l_val == '8,8':
                            stdout = (
                                'hash1 (<assignee-protobuf@google.com> '
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
    async def _mock_run_go(args, _cwd):
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
    async def test_run(self, mock_run):
        """Test scanning Go modules."""
        mock_run.side_effect = self._mock_subprocess_run

        analyzer = GoModAnalyzer()
        analyzer._run_go = self._mock_run_go
        self.set_date(date(2024, 4, 10))

        consumer = PipelineSink()
        analyzer.connect(consumer)

        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            max_retries=0,
        )
        await analyzer.run()

        # Collect reports
        results = []
        while True:
            result_path = await consumer.input_queue.get()
            if result_path is None:
                break
            result = await FreshnessResult.load(result_path)
            results.append(result)

        self.assertEqual(len(results), 3)

        # 1. grpc (has update)
        grpc = next(r for r in results if r.package == 'google.golang.org/grpc')
        self.assertEqual(grpc.source, 'go.mod')
        self.assertEqual(grpc.location.file, 'go.mod')
        self.assertEqual(grpc.location.lines, (6, 6))
        self.assertEqual(grpc.pkg_type, 'go_mod')
        self.assertEqual(
            grpc.current, PackageVersion('v1.62.0', date(2024, 3, 1))
        )
        self.assertEqual(
            grpc.earliest, PackageVersion('v1.62.0', date(2024, 3, 1))
        )
        self.assertEqual(grpc.tier, 2)
        self.assertIsNone(grpc.assignee)

        # 2. protobuf (no update, latest == current)
        proto = next(
            r for r in results if r.package == 'google.golang.org/protobuf'
        )
        self.assertEqual(proto.source, 'go.mod')
        self.assertEqual(proto.location.file, 'go.mod')
        self.assertEqual(proto.location.lines, (8, 8))
        self.assertEqual(proto.pkg_type, 'go_mod')
        self.assertEqual(
            proto.current, PackageVersion('v1.33.0', date(2024, 3, 5))
        )
        self.assertEqual(
            proto.earliest, PackageVersion('v1.33.0', date(2024, 3, 5))
        )
        self.assertEqual(proto.tier, 2)
        self.assertIsNone(proto.assignee)

        # 3. examples (pseudo-version, earliest == current)
        examples = next(
            r for r in results if r.package == 'google.golang.org/grpc/examples'
        )
        self.assertEqual(examples.source, 'go.mod')
        self.assertEqual(examples.location.file, 'go.mod')
        self.assertEqual(examples.location.lines, (7, 7))
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
        self.assertIsNone(examples.assignee)

    @patch('subprocess.run')
    async def test_go_not_found(self, mock_run):
        """Test _run_go raises FileNotFoundError when go is missing."""
        mock_run.side_effect = FileNotFoundError('go not found')
        analyzer = GoModAnalyzer()

        with self.assertRaises(FileNotFoundError) as ctx:
            await analyzer._run_go(['version'], self.test_dir)
        self.assertIn('`go` command not found', str(ctx.exception))
        self.assertIn('activate the Pigweed environment', str(ctx.exception))

    @patch.object(GoModAnalyzer, '_run_go_list', new_callable=AsyncMock)
    async def test_set_up_unversioned_module_logs_warning(
        self, mock_run_go: AsyncMock
    ) -> None:
        """Tests unversioned module logs warning and continues."""
        mock_proc = MagicMock()
        mock_proc.stdout = json.dumps({'Path': 'local/pkg'})
        mock_run_go.return_value = mock_proc

        analyzer = GoModAnalyzer()
        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
        )

        with self.assertLogs('pw_fortifier.go_mod', level='WARNING') as cm:
            await analyzer._set_up()

        self.assertTrue(
            any(
                'Skipping local or unversioned Go module' in msg
                for msg in cm.output
            )
        )

    @patch.object(GoModAnalyzer, '_get_all_versions', new_callable=AsyncMock)
    @patch.object(GoModAnalyzer, '_run_go_list', new_callable=AsyncMock)
    async def test_set_up_resolution_failure_raises(
        self, mock_run_go: AsyncMock, mock_get_versions: AsyncMock
    ) -> None:
        """Tests failure to resolve versions for direct module raises."""
        mock_proc1 = MagicMock()
        mock_proc1.stdout = json.dumps(
            {
                'Path': 'example.com/mod',
                'Version': 'v1.0.0',
                'Time': '2026-01-01T00:00:00Z',
            }
        )
        mock_proc2 = MagicMock()
        mock_proc2.stdout = json.dumps({'Versions': ['v1.0.0']})
        mock_run_go.side_effect = [mock_proc1, mock_proc2]
        mock_get_versions.return_value = []

        analyzer = GoModAnalyzer()
        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
        )

        with self.assertRaises(RuntimeError) as ctx:
            await analyzer._set_up()

        self.assertIn('Failed to resolve versions', str(ctx.exception))

    async def test_configure_skip_setup(self) -> None:
        """Tests configure sets skip_setup when go.mod is not in files."""
        analyzer = GoModAnalyzer()
        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            files=None,
        )
        self.assertFalse(analyzer.skip_setup)

        analyzer = GoModAnalyzer()
        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            files=['go.mod'],
        )
        self.assertFalse(analyzer.skip_setup)

        analyzer = GoModAnalyzer()
        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            files=['other.txt'],
        )
        self.assertTrue(analyzer.skip_setup)


if __name__ == '__main__':
    unittest.main()
