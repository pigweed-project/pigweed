# Copyright 2025 The Pigweed Authors
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
"""Test for the pw_ide compile commands fragment merger."""

import io
import json
import os
import unittest
from pathlib import Path
from unittest import mock
from contextlib import redirect_stderr

# pylint: disable=unused-import

# Mocked imports.
import subprocess
import sys
import time

# pylint: enable=unused-import

from pyfakefs import fake_filesystem_unittest

from pw_ide import merger

_FRAGMENT_SUFFIX = '.pw_aspect.compile_commands.json'


def _create_fragment(
    fs, output_path: Path, base_name: str, platform: str, content: list
):
    fragment_path = output_path / f'{base_name}.{platform}{_FRAGMENT_SUFFIX}'
    fs.create_file(fragment_path, contents=json.dumps(content))


# pylint: disable=line-too-long

_BEP_CONTENT_LOCAL_BUILD = """\
{"id":{"namedSet":{"id":"6"}},"namedSetOfFiles":{"files":[{"name":"pw_status/pw_status.k8-fastbuild.pw_aspect.compile_commands.json","uri":"file:///home/somebody/.cache/bazel/_bazel_somebody/123abc/execroot/_main/bazel-out/k8-fastbuild/bin/pw_status/pw_status.k8-fastbuild.pw_aspect.compile_commands.json","pathPrefix":["bazel-out","k8-fastbuild","bin"],"digest":"b5f9dd673261c07a2afc7ae9029aafd56dd873227153923288ec64ba14a250d0","length":"1532"},{"name":"pw_string/format.k8-fastbuild.pw_aspect.compile_commands.json","uri":"file:///home/somebody/.cache/bazel/_bazel_somebody/123abc/execroot/_main/bazel-out/k8-fastbuild/bin/pw_string/format.k8-fastbuild.pw_aspect.compile_commands.json","pathPrefix":["bazel-out","k8-fastbuild","bin"],"digest":"8f982c0c3ba094c72a886e3a51eb2537dcbe26cef88be720845ac329214cb808","length":"1495"}]}}
{"id":{"progress":{"opaqueCount":17}},"children":[{"progress":{"opaqueCount":18}},{"namedSet":{"id":"5"}}],"progress":{}}
"""

_BEP_CONTENT_REMOTE_BUILD = """\
{"id":{"namedSet":{"id":"12"}},"namedSetOfFiles":{"files":[{"name":"pw_unit_test/simple_printing_main.k8-fastbuild.pw_aspect.compile_commands.json","uri":"bytestream://remotebuildexecution.googleapis.com/projects/pigweed-rbe-open/instances/default-instance/blobs/a5d56997f015627de35be59531ba37032684f0682aac6526a0bbf7744c3b4e1f/2248","pathPrefix":["bazel-out","k8-fastbuild","bin"],"digest":"a5d56997f015627de35be59531ba37032684f0682aac6526a0bbf7744c3b4e1f","length":"2248"}],"fileSets":[{"id":"13"},{"id":"17"},{"id":"2"}]}}
{"id":{"progress":{"opaqueCount":36}},"children":[{"progress":{"opaqueCount":37}},{"namedSet":{"id":"11"}}],"progress":{}}
"""

# pylint: enable=line-too-long

# Join both remote and local BEP file contents.
_BEP_CONTENT = _BEP_CONTENT_LOCAL_BUILD + _BEP_CONTENT_REMOTE_BUILD


class MergerTest(fake_filesystem_unittest.TestCase):
    """Tests for the compile commands fragment merger."""

    def setUp(self):
        self.setUpPyfakefs()
        self.workspace_root = Path('/workspace')
        self.output_base = Path(
            '/home/somebody/.cache/bazel/_bazel_somebody/123abc'
        )
        self.execution_root = self.output_base / 'execroot' / '_main'
        self.output_path = self.execution_root / 'bazel-out'

        self.fs.create_dir(self.workspace_root)
        self.fs.create_dir(self.output_path)

        self.mock_environ = self.enterContext(
            mock.patch.dict(
                os.environ,
                {
                    'BUILD_WORKSPACE_DIRECTORY': str(self.workspace_root),
                    'BUILD_WORKING_DIRECTORY': str(self.workspace_root),
                },
            )
        )

        self.mock_run_bazel = self.enterContext(
            mock.patch('pw_ide.merger._run_bazel')
        )

        def run_bazel_side_effect(
            args,
            **kwargs,  # pylint: disable=unused-argument
        ):
            if args == ['info', 'output_path']:
                return mock.Mock(stdout=f'{self.output_path}\n')
            if args == ['info', 'output_base']:
                return mock.Mock(stdout=f'{self.output_base}\n')
            if args == ['info', 'execution_root']:
                return mock.Mock(stdout=f'{self.execution_root}\n')
            raise AssertionError(f'Unhandled Bazel request: {args}')

        self.mock_run_bazel.side_effect = run_bazel_side_effect

    def test_no_fragments_found(self):
        with io.StringIO() as buf, redirect_stderr(buf):
            self.assertEqual(merger.main(), 1)
            self.assertIn(
                'Could not find any generated fragments', buf.getvalue()
            )

    def test_basic_merge(self):
        """Test that a single compile command produces a database."""
        _create_fragment(
            self.fs,
            self.output_path,
            'target1',
            'k8-fastbuild',
            [
                {
                    'file': 'a.cc',
                    'directory': '__WORKSPACE_ROOT__',
                    'arguments': ['c', 'd'],
                }
            ],
        )
        self.assertEqual(merger.main(), 0)
        merged_path = (
            self.workspace_root
            / '.compile_commands'
            / 'k8-fastbuild'
            / 'compile_commands.json'
        )
        self.assertTrue(merged_path.exists())
        with open(merged_path, 'r') as f:
            data = json.load(f)
        self.assertEqual(len(data), 1)
        self.assertEqual(data[0]['file'], 'a.cc')
        self.assertEqual(data[0]['directory'], str(self.workspace_root))

    def test_writes_last_generation_time(self):
        """Test that the last generation time file is written."""
        _create_fragment(
            self.fs,
            self.output_path,
            'target1',
            'k8-fastbuild',
            [
                {
                    'file': 'a.cc',
                    'directory': '__WORKSPACE_ROOT__',
                    'arguments': ['c', 'd'],
                }
            ],
        )
        self.assertEqual(merger.main(), 0)
        timestamp_path = (
            self.workspace_root
            / '.compile_commands'
            / 'pw_lastGenerationTime.txt'
        )
        self.assertTrue(timestamp_path.exists())
        content = timestamp_path.read_text(encoding='utf-8')
        self.assertTrue(content.isdigit())

    def test_merge_multiple_platforms(self):
        """Test that multiple platforms are correctly separated."""
        _create_fragment(
            self.fs,
            self.output_path,
            't1',
            'k8-fastbuild',
            [
                {
                    'file': 'a.cc',
                    'directory': '__WORKSPACE_ROOT__',
                    'arguments': [],
                }
            ],
        )
        _create_fragment(
            self.fs,
            self.output_path,
            't2',
            'mac',
            [
                {
                    'file': 'b.cc',
                    'directory': '__WORKSPACE_ROOT__',
                    'arguments': [],
                }
            ],
        )
        self.assertEqual(merger.main(), 0)
        host_fastbuild_path = (
            self.workspace_root
            / '.compile_commands'
            / 'k8-fastbuild'
            / 'compile_commands.json'
        )
        mac_path = (
            self.workspace_root
            / '.compile_commands'
            / 'mac'
            / 'compile_commands.json'
        )
        self.assertTrue(host_fastbuild_path.exists())
        self.assertTrue(mac_path.exists())

    def test_merge_with_json_error(self):
        """Test corrupt compile command fragments."""
        fragment_path = self.output_path / f'bad.k8-fastbuild{_FRAGMENT_SUFFIX}'
        self.fs.create_file(fragment_path, contents='not json')
        _create_fragment(
            self.fs,
            self.output_path,
            'good',
            'k8-fastbuild',
            [
                {
                    'file': 'a.cc',
                    'directory': '__WORKSPACE_ROOT__',
                    'arguments': [],
                }
            ],
        )

        with io.StringIO() as buf, redirect_stderr(buf):
            self.assertEqual(merger.main(), 0)
            self.assertIn(f'Could not parse {fragment_path}', buf.getvalue())

        merged_path = (
            self.workspace_root
            / '.compile_commands'
            / 'k8-fastbuild'
            / 'compile_commands.json'
        )
        with open(merged_path, 'r') as f:
            data = json.load(f)
        self.assertEqual(len(data), 1)

    def test_filter_unsupported_march(self):
        """Ensures an unsupported -march value is removed."""
        _create_fragment(
            self.fs,
            self.output_path,
            't1',
            'k8-fastbuild',
            [
                {
                    'file': 'a.cc',
                    'directory': '__WORKSPACE_ROOT__',
                    'arguments': ['-march=unsupported', '-march=x86-64'],
                }
            ],
        )
        self.assertEqual(merger.main(), 0)
        merged_path = (
            self.workspace_root
            / '.compile_commands'
            / 'k8-fastbuild'
            / 'compile_commands.json'
        )
        with open(merged_path, 'r') as f:
            data = json.load(f)
        self.assertEqual(data[0]['arguments'], ['-march=x86-64'])

    def test_resolve_bazel_out_paths(self):
        """Test that generated files are remapped to their absolute path."""
        _create_fragment(
            self.fs,
            self.output_path,
            't1',
            'k8-fastbuild',
            [
                {
                    'file': 'bazel-out/k8-fastbuild/bin/a.cc',
                    'directory': '__WORKSPACE_ROOT__',
                    'arguments': ['-Ibazel-out/k8-fastbuild/genfiles'],
                }
            ],
        )
        self.assertEqual(merger.main(), 0)
        merged_path = (
            self.workspace_root
            / '.compile_commands'
            / 'k8-fastbuild'
            / 'compile_commands.json'
        )
        with open(merged_path, 'r') as f:
            data = json.load(f)
        expected_file = str(self.output_path / 'k8-fastbuild/bin/a.cc')
        expected_arg = '-I' + str(self.output_path / 'k8-fastbuild/genfiles')
        self.assertEqual(data[0]['file'], expected_file)
        self.assertEqual(data[0]['arguments'], [expected_arg])

    def test_external_repo_paths(self):
        """Test that files in external repos are remapped to their real path."""
        _create_fragment(
            self.fs,
            self.output_path,
            't1',
            'k8-fastbuild',
            [
                {
                    'file': 'external/my_repo/a.cc',
                    'directory': '__WORKSPACE_ROOT__',
                    'arguments': [
                        '-Iexternal/my_repo/include',
                        '-iquote',
                        'external/+_repo_rules8+my_external_thing',
                    ],
                }
            ],
        )
        self.assertEqual(merger.main(), 0)
        merged_path = (
            self.workspace_root
            / '.compile_commands'
            / 'k8-fastbuild'
            / 'compile_commands.json'
        )
        with open(merged_path, 'r') as f:
            data = json.load(f)
        expected_file = str(self.output_base / 'external/my_repo/a.cc')
        expected_args = [
            '-I' + str(self.output_base / 'external/my_repo/include'),
            '-iquote',
            str(self.output_base / 'external/+_repo_rules8+my_external_thing'),
        ]
        self.assertEqual(data[0]['file'], expected_file)
        self.assertEqual(data[0]['arguments'], expected_args)

    def test_external_repo_symlinks(self):
        """Test that symlinks in external repos are resolved."""
        real_repo_path = self.workspace_root / 'real_repo'
        self.fs.create_dir(real_repo_path)
        real_file_path = real_repo_path / 'a.cc'
        self.fs.create_file(real_file_path)

        # Create a symlink in the external directory pointing to the real repo
        external_repo_path = self.output_base / 'external/my_repo'
        self.fs.create_dir(self.output_base / 'external')
        self.fs.create_symlink(external_repo_path, real_repo_path)

        _create_fragment(
            self.fs,
            self.output_path,
            't1',
            'k8-fastbuild',
            [
                {
                    'file': 'external/my_repo/a.cc',
                    'directory': '__WORKSPACE_ROOT__',
                    'arguments': [],
                }
            ],
        )
        self.assertEqual(merger.main(), 0)
        merged_path = (
            self.workspace_root
            / '.compile_commands'
            / 'k8-fastbuild'
            / 'compile_commands.json'
        )
        with open(merged_path, 'r') as f:
            data = json.load(f)

        # The expected path should be the real path, not the symlink path
        expected_file = str(real_file_path)
        self.assertEqual(data[0]['file'], expected_file)

    def test_empty_fragment_file(self):
        """Test that an empty fragment file doesn't cause issues."""
        _create_fragment(self.fs, self.output_path, 'empty', 'k8-fastbuild', [])
        _create_fragment(
            self.fs,
            self.output_path,
            'good',
            'k8-fastbuild',
            [
                {
                    'file': 'a.cc',
                    'directory': '__WORKSPACE_ROOT__',
                    'arguments': [],
                }
            ],
        )
        self.assertEqual(merger.main(), 0)
        merged_path = (
            self.workspace_root
            / '.compile_commands'
            / 'k8-fastbuild'
            / 'compile_commands.json'
        )
        with open(merged_path, 'r') as f:
            data = json.load(f)
        self.assertEqual(len(data), 1)

    def test_workspace_root_not_set(self):
        """Ensure BUILD_WORKSPACE_DIRECTORY checking traps."""
        del os.environ['BUILD_WORKSPACE_DIRECTORY']
        with io.StringIO() as buf, redirect_stderr(buf):
            self.assertEqual(merger.main(), 1)
            self.assertIn("must be run with 'bazel run'", buf.getvalue())

    def test_output_path_does_not_exist(self):
        """Ensure an error occurs if the Bazel output path does not exist."""
        self.fs.remove_object(str(self.output_path))
        with io.StringIO() as buf, redirect_stderr(buf):
            self.assertEqual(merger.main(), 1)
            self.assertIn('not found', buf.getvalue())

    def _configure_bep_based_collection(self, mock_run_bazel):
        """Configure a test for bep-based collection."""

        def run_bazel_side_effect(
            args,
            **kwargs,  # pylint: disable=unused-argument
        ):
            if args == ['info', 'output_path']:
                return mock.Mock(stdout=f'{self.output_path}\n')
            if args == ['info', 'output_base']:
                return mock.Mock(stdout=f'{self.output_base}\n')
            if args == ['info', 'execution_root']:
                return mock.Mock(stdout=f'{self.execution_root}\n')

            bep_path_arg = next(
                arg
                for arg in args
                if arg.startswith('--build_event_json_file=')
            )
            bep_path = Path(bep_path_arg.split('=', 1)[1])
            bep_path.write_text(_BEP_CONTENT)

            self.fs.create_file(
                self.output_path.joinpath(
                    'k8-fastbuild',
                    'bin',
                    'pw_status',
                    'pw_status.k8-fastbuild' + _FRAGMENT_SUFFIX,
                ),
                contents=json.dumps(
                    [
                        {
                            'file': 'a.cc',
                            'directory': '__WORKSPACE_ROOT__',
                            'arguments': ['c', 'd'],
                        }
                    ]
                ),
            )
            self.fs.create_file(
                self.output_path.joinpath(
                    'k8-fastbuild',
                    'bin',
                    'pw_unit_test',
                    'simple_printing_main.k8-fastbuild' + _FRAGMENT_SUFFIX,
                ),
                contents=json.dumps(
                    [
                        {
                            'file': 'b.cc',
                            'directory': '__WORKSPACE_ROOT__',
                            'arguments': ['e', 'f'],
                        }
                    ]
                ),
            )
            self.fs.create_file(
                self.output_path.joinpath(
                    'k8-fastbuild',
                    'bin',
                    'pw_string',
                    'format.k8-fastbuild' + _FRAGMENT_SUFFIX,
                ),
                contents=json.dumps(
                    [
                        {
                            'file': 'c.cc',
                            'directory': '__WORKSPACE_ROOT__',
                            'arguments': ['e', 'f', 'g'],
                        }
                    ]
                ),
            )

            return mock.Mock(returncode=0)

        mock_run_bazel.side_effect = run_bazel_side_effect

    @mock.patch('pw_ide.merger._run_bazel')
    def test_build_and_collect_fragments(self, mock_run_bazel):
        """Tests that fragments are collected via `bazel build`."""
        self._configure_bep_based_collection(mock_run_bazel)

        with mock.patch.object(
            sys, 'argv', ['merger.py', '--', 'build', '//...']
        ):
            self.assertEqual(merger.main(), 0)

        merged_path = (
            self.workspace_root
            / '.compile_commands'
            / 'k8-fastbuild'
            / 'compile_commands.json'
        )
        self.assertTrue(merged_path.exists())
        with open(merged_path, 'r') as f:
            data = json.load(f)
        self.assertEqual(len(data), 3)
        files = {item['file'] for item in data}
        self.assertIn('a.cc', files)
        self.assertIn('b.cc', files)
        self.assertIn('c.cc', files)

    @mock.patch('pw_ide.merger._run_bazel')
    def test_build_and_collect_fragments_with_forwarded_args(
        self, mock_run_bazel
    ):
        """Tests that forwarded `bazel run` args are stripped."""
        self._configure_bep_based_collection(mock_run_bazel)

        with mock.patch.object(
            sys,
            'argv',
            ['merger.py', '--', 'run', '//...', '--', 'some-arg', '--another'],
        ):
            self.assertEqual(merger.main(), 0)

        # Check that 'run' was converted to 'build' and '-- extra' was stripped.
        build_args = mock_run_bazel.call_args_list[-1].args[0]
        self.assertIn('build', build_args)
        self.assertIn('//...', build_args)
        self.assertNotIn('run', build_args)
        self.assertNotIn('--', build_args)
        self.assertNotIn('some-arg', build_args)
        self.assertNotIn('--another', build_args)

        merged_path = (
            self.workspace_root
            / '.compile_commands'
            / 'k8-fastbuild'
            / 'compile_commands.json'
        )
        self.assertTrue(merged_path.exists())
        with open(merged_path, 'r') as f:
            data = json.load(f)
        self.assertEqual(len(data), 3)

    def test_overwrite_threshold(self):
        """Tests the --overwrite-threshold flag."""
        merged_path = (
            self.workspace_root
            / '.compile_commands'
            / 'k8-fastbuild'
            / 'compile_commands.json'
        )
        self.fs.create_file(merged_path, contents='[{"test": "entry"}]')

        _create_fragment(
            self.fs,
            self.output_path,
            'target1',
            'k8-fastbuild',
            [
                {
                    'file': 'a.cc',
                    'directory': '__WORKSPACE_ROOT__',
                    'arguments': ['c', 'd'],
                }
            ],
        )

        # Set modification time to a known point in the past.
        past_time = int(time.time()) - 100
        os.utime(merged_path, (past_time, past_time))

        # Run with a threshold OLDER than the file. This should skip generation.
        threshold = past_time - 1
        with mock.patch.object(
            sys, 'argv', ['merger.py', f'--overwrite-threshold={threshold}']
        ):
            self.assertEqual(merger.main(), 0)

        # Assert the file was NOT overwritten.
        with open(merged_path, 'r') as f:
            data = json.load(f)
        self.assertEqual(data, [{"test": "entry"}])

        # Run with a threshold NEWER than the file. This should NOT skip
        # generation.
        threshold = int(past_time + 1)
        with mock.patch.object(
            sys, 'argv', ['merger.py', f'--overwrite-threshold={threshold}']
        ):
            self.assertEqual(merger.main(), 0)

        # Ensure the file WAS overwritten.
        with open(merged_path, 'r') as f:
            data = json.load(f)
        self.assertNotEqual(data, [{"test": "entry"}])
        self.assertEqual(len(data), 1)
        self.assertEqual(data[0]['file'], 'a.cc')

    def test_merge_conflict_with_outputs_key(self):
        """Tests that a conflict is detected for the same file and output."""
        _create_fragment(
            self.fs,
            self.output_path,
            'target1',
            'k8-fastbuild',
            [
                {
                    'file': 'a.cc',
                    'directory': '__WORKSPACE_ROOT__',
                    'arguments': ['c', '-DVERSION=1'],
                    'outputs': ['a.o'],
                }
            ],
        )
        _create_fragment(
            self.fs,
            self.output_path,
            'target2',
            'k8-fastbuild',
            [
                {
                    'file': 'a.cc',
                    'directory': '__WORKSPACE_ROOT__',
                    'arguments': ['c', '-DVERSION=2'],
                    'outputs': ['a.o'],
                }
            ],
        )

        with io.StringIO() as buf, redirect_stderr(buf):
            self.assertEqual(merger.main(), 1)
            self.assertIn('Conflict for file a.cc', buf.getvalue())
            self.assertIn('-DVERSION=1', buf.getvalue())
            self.assertIn('-DVERSION=2', buf.getvalue())

    def test_no_conflict_with_different_outputs_key(self):
        """Tests no conflict is detected for the same file, different output."""
        _create_fragment(
            self.fs,
            self.output_path,
            'target1',
            'k8-fastbuild',
            [
                {
                    'file': 'a.cc',
                    'directory': '__WORKSPACE_ROOT__',
                    'arguments': ['c', '-DVERSION=1'],
                    'outputs': ['a.v1.o'],
                }
            ],
        )
        _create_fragment(
            self.fs,
            self.output_path,
            'target2',
            'k8-fastbuild',
            [
                {
                    'file': 'a.cc',
                    'directory': '__WORKSPACE_ROOT__',
                    'arguments': ['c', '-DVERSION=2'],
                    'outputs': ['a.v2.o'],
                }
            ],
        )

        self.assertEqual(merger.main(), 0)
        merged_path = (
            self.workspace_root
            / '.compile_commands'
            / 'k8-fastbuild'
            / 'compile_commands.json'
        )
        self.assertTrue(merged_path.exists())
        with open(merged_path, 'r') as f:
            data = json.load(f)
        self.assertEqual(len(data), 2)

    @mock.patch('pw_ide.merger._run_bazel_build_for_fragments')
    def test_merge_groups_conflict_skip(self, mock_build):
        """Tests that conflicting fragments from groups are skipped."""
        fragment_path = self.output_path / f'target.p1{_FRAGMENT_SUFFIX}'
        safe_fragment_path = self.output_path / f'safe.p1{_FRAGMENT_SUFFIX}'

        def side_effect(build_args, _verbose, _execution_root):
            # Simulate different content for different calls
            if self.fs.exists(fragment_path):
                self.fs.remove_object(str(fragment_path))

            # Always create the safe fragment
            if not self.fs.exists(safe_fragment_path):
                self.fs.create_file(
                    safe_fragment_path,
                    contents=(
                        '[{"file": "safe.cc", '
                        '"directory": "__WORKSPACE_ROOT__", '
                        '"arguments": ["3"]}]'
                    ),
                )

            if '//t1' in build_args:
                self.fs.create_file(
                    fragment_path,
                    contents=(
                        '[{"file": "a.cc", '
                        '"directory": "__WORKSPACE_ROOT__", '
                        '"arguments": ["1"]}]'
                    ),
                )
            else:
                self.fs.create_file(
                    fragment_path,
                    contents=(
                        '[{"file": "a.cc", '
                        '"directory": "__WORKSPACE_ROOT__", '
                        '"arguments": ["2"]}]'
                    ),
                )
            return {fragment_path, safe_fragment_path}

        mock_build.side_effect = side_effect

        groups_file = self.workspace_root / 'groups.json'
        self.fs.create_file(
            groups_file,
            contents=json.dumps(
                {
                    'compile_commands_patterns': [
                        {'platform': 'p1', 'target_patterns': ['//t1']},
                        {'platform': 'p1', 'target_patterns': ['//t2']},
                    ]
                }
            ),
        )

        with mock.patch.object(
            sys,
            'argv',
            ['merger.py', f'--compile-command-groups={groups_file}'],
        ):
            with io.StringIO() as buf, redirect_stderr(buf):
                self.assertEqual(merger.main(), 0)
                self.assertIn('Skipping this fragment', buf.getvalue())

        merged_path = (
            self.workspace_root
            / '.compile_commands'
            / 'p1'
            / 'compile_commands.json'
        )
        with open(merged_path, 'r') as f:
            data = json.load(f)
        # Should contain ONLY safe.cc
        self.assertEqual(len(data), 1)
        self.assertEqual(data[0]['file'], 'safe.cc')


if __name__ == '__main__':
    unittest.main()
