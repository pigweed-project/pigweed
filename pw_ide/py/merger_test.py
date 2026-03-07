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
    fs,
    output_path: Path,
    base_name: str,
    platform: str,
    content: list,
    target_info: list | None = None,
):
    fragment_path = output_path / f'{base_name}.{platform}{_FRAGMENT_SUFFIX}'

    fragment_data = {
        'commands': content,
    }
    if target_info is not None:
        fragment_data['target_info'] = target_info

    fs.create_file(fragment_path, contents=json.dumps(fragment_data))


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

    # pylint: disable=too-many-public-methods

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

        self.mock_environ_patcher = mock.patch.dict(
            os.environ,
            {
                'BUILD_WORKSPACE_DIRECTORY': str(self.workspace_root),
                'BUILD_WORKING_DIRECTORY': str(self.workspace_root),
            },
        )
        self.mock_environ = self.mock_environ_patcher.start()
        self.addCleanup(self.mock_environ_patcher.stop)

        self.mock_run_bazel_patcher = mock.patch('pw_ide.merger._run_bazel')
        self.mock_run_bazel = self.mock_run_bazel_patcher.start()
        self.addCleanup(self.mock_run_bazel_patcher.stop)

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

        # Mock collection for tests that rely on pre-existing fragments
        # (simulating the old globbing behavior).
        self.mock_collect_fragments_patcher = mock.patch(
            'pw_ide.merger._collect_fragments'
        )
        self.mock_collect_fragments = (
            self.mock_collect_fragments_patcher.start()
        )
        self.addCleanup(self.mock_collect_fragments_patcher.stop)

        def collect_fragments_side_effect(
            execution_root,
            forwarded_args,
            verbose,
            compile_command_groups=None,
        ):
            # If arguments are provided, use the real implementation (which
            # delegates to other internal functions like
            # _build_and_collect_fragments).
            # But here we are mocking the top-level dispatch function.
            # So, for tests that want to test collection logic (like
            # test_build_and_collect_fragments), they must unpatch this or we
            # must implement the dispatch here. Implementing dispatch is
            # cleaner.

            # pylint: disable=protected-access
            if compile_command_groups:
                yield from merger._build_and_collect_fragments_from_groups(
                    compile_command_groups,
                    verbose,
                    execution_root,
                )
            elif forwarded_args:
                yield from merger._build_and_collect_fragments(
                    forwarded_args,
                    verbose,
                    execution_root,
                )
            else:
                # Fallback to globbing for tests that don't invoke Bazel.
                # This mimics the old behavior solely for the purpose of testing
                # the merging logic on pre-existing files.
                yield from execution_root.joinpath('bazel-out').rglob(
                    f'*{_FRAGMENT_SUFFIX}'
                )

        self.mock_collect_fragments.side_effect = collect_fragments_side_effect

    def test_no_fragments_found(self):
        with io.StringIO() as buf, redirect_stderr(buf):
            with self.assertRaises(SystemExit):
                merger.main()
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

    def test_validate_environment_symlinks(self):
        """Test that _validate_environment resolves symlinks (e.g. /var vs
        /private/var)."""
        # Create canonical paths
        real_workspace = Path('/private/real_workspace')
        real_output_base = Path('/private/real_output_base')
        self.fs.create_dir(real_workspace)
        self.fs.create_dir(real_output_base)
        self.fs.create_dir(
            real_output_base / "execroot" / "_main" / "bazel-out"
        )  # bazel_output_path

        # Create symlinked paths
        symlink_workspace = Path('/symlink_workspace')
        symlink_output_base = Path('/symlink_output_base')
        self.fs.create_symlink(symlink_workspace, real_workspace)
        self.fs.create_symlink(symlink_output_base, real_output_base)

        # Update environment to use symlinked workspace
        self.mock_environ['BUILD_WORKSPACE_DIRECTORY'] = str(symlink_workspace)

        # Mock run_bazel to return symlinked output base
        def mock_run_bazel(args, **_kwargs):
            if args == ['info', 'output_path']:
                # Assume output_path is inside execution_root inside output_base
                return mock.Mock(
                    stdout=f'{symlink_output_base}/execroot/_main/bazel-out\n'
                )
            if args == ['info', 'output_base']:
                return mock.Mock(stdout=f'{symlink_output_base}\n')
            if args == ['info', 'execution_root']:
                return mock.Mock(
                    stdout=f'{symlink_output_base}/execroot/_main\n'
                )
            raise AssertionError(f'Unhandled Bazel request: {args}')

        self.mock_run_bazel.side_effect = mock_run_bazel

        # Call _validate_environment directly
        # pylint: disable=protected-access
        (
            workspace_root,
            bazel_output_path,
            output_base_path,
            execution_root_path,
        ) = merger._validate_environment()
        # pylint: enable=protected-access

        # Assert that returned paths are canonical (resolved)
        self.assertEqual(workspace_root, real_workspace)
        self.assertEqual(output_base_path, real_output_base)
        self.assertEqual(
            execution_root_path, real_output_base / "execroot" / "_main"
        )
        self.assertEqual(
            bazel_output_path,
            real_output_base / "execroot" / "_main" / "bazel-out",
        )

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
        # Mock the bazel-out symlink so they are relativized
        self.fs.create_dir(self.workspace_root / 'bazel-out')

        self.assertEqual(merger.main(), 0)
        merged_path = (
            self.workspace_root
            / '.compile_commands'
            / 'k8-fastbuild'
            / 'compile_commands.json'
        )
        with open(merged_path, 'r') as f:
            data = json.load(f)
        expected_file = 'bazel-out/k8-fastbuild/bin/a.cc'
        expected_arg = '-Ibazel-out/k8-fastbuild/genfiles'

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
        expected_file = 'external/my_repo/a.cc'

        expected_args = [
            '-Iexternal/my_repo/include',
            '-iquote',
            'external/+_repo_rules8+my_external_thing',
        ]
        self.assertEqual(data[0]['file'], expected_file)
        self.assertEqual(data[0]['arguments'], expected_args)

    def test_external_repo_absolute_paths(self):
        """Test that absolute external repo paths are preserved/handled."""
        # Simulate Bazel providing absolute paths in the fragment (e.g. from
        # action.argv)
        abs_external_path = self.output_base / 'external/my_repo/a.cc'
        abs_include_path = self.output_base / 'external/my_repo/include'

        _create_fragment(
            self.fs,
            self.output_path,
            't1',
            'k8-fastbuild',
            [
                {
                    'file': str(abs_external_path),
                    'directory': '__WORKSPACE_ROOT__',
                    'arguments': [
                        f'-I{abs_include_path}',
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

        # Expect 'file' to be relativized to external/ if it is in external repo
        # The logic falls back to absolute path if not under workspace.
        # expected_external_file = 'external/my_repo/a.cc'
        expected_external_file = 'external/my_repo/a.cc'
        self.assertEqual(data[0]['file'], expected_external_file)

        # Expect 'arguments' to be relativized to external/
        self.assertEqual(data[0]['arguments'], ['-Iexternal/my_repo/include'])

    def test_external_repo_absolute_paths_string_match(self):
        """Test that absolute external repo paths are matched by string."""
        # This simulates the case where the path is logically in the output
        # base, but resolve() might fail or resolve to a different path (e.g.
        # symlinks) or we just want to ensure the string prefix check works.
        # output_base: /private/var/tmp/_bazel_user/hash
        # arg: /private/var/tmp/_bazel_user/hash/external/...
        #
        # We ensure this matches textually even if filesystem logic is mocked
        # strict. Use the actual self.output_base but ensure it looks "absolute"
        # and we use string concat.
        abs_external_path = f"{self.output_base}/external/my_repo/a.cc"
        abs_include_path = f"{self.output_base}/external/my_repo/include"

        _create_fragment(
            self.fs,
            self.output_path,
            't1',
            'k8-fastbuild',
            [
                {
                    'file': abs_external_path,
                    'directory': '__WORKSPACE_ROOT__',
                    'arguments': [
                        f'-I{abs_include_path}',
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

        expected_external_file = 'external/my_repo/a.cc'
        self.assertEqual(data[0]['file'], expected_external_file)
        self.assertEqual(data[0]['arguments'], ['-Iexternal/my_repo/include'])

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

        expected_file = 'real_repo/a.cc'
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
            with self.assertRaises(SystemExit):
                merger.main()
            self.assertIn("must be run with 'bazel run'", buf.getvalue())

    def test_output_path_does_not_exist(self):
        """Ensure an error occurs if the Bazel output path does not exist."""
        self.fs.remove_object(str(self.output_path))
        with io.StringIO() as buf, redirect_stderr(buf):
            with self.assertRaises(SystemExit):
                merger.main()
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
            with self.assertRaises(SystemExit):
                merger.main()
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

    def test_relativize_path(self):
        """Test _relativize_path logic."""
        # pylint: disable=protected-access
        workspace = Path("/workspace")
        self.assertEqual(
            merger._relativize_path("/workspace/foo/bar.cc", workspace),
            "foo/bar.cc",
        )
        self.assertEqual(
            merger._relativize_path("/workspace/bar.h", workspace),
            "bar.h",
        )
        # Test path outside workspace (should return relative path with ..)
        self.assertEqual(
            merger._relativize_path("/other/path.cc", workspace),
            "../other/path.cc",
        )
        # Test relative path (should return as-is)
        self.assertEqual(
            merger._relativize_path("foo/bar.cc", workspace),
            "foo/bar.cc",
        )
        # Test None start (should return as-is)
        self.assertEqual(
            merger._relativize_path("/workspace/foo/bar.cc", None),
            "/workspace/foo/bar.cc",
        )
        # pylint: enable=protected-access

    def test_remap_virtual_includes(self):
        """Test resolving virtual includes via map and dynamically."""
        cmd = merger.CompileCommand(
            file="foo/bar.cc",
            directory="",
            arguments=[
                "-Ibazel-out/k8-fastbuild/bin/external/"
                "foo/_virtual_includes/foo_lib",
                "-Ibazel-out/k8-fastbuild/bin/external/"
                "bar/_virtual_includes/bar_lib",
            ],
        )

        vrmaps = {
            (
                "bazel-out/k8-fastbuild/bin/external/"
                "foo/_virtual_includes/foo_lib"
            ): "external/foo/include",
        }

        # Set up a fake symlink for the dynamic fallback (the second arg)
        v_dir = (
            self.output_path
            / "k8-fastbuild/bin/external/bar/_virtual_includes/bar_lib"
        )
        self.fs.create_dir(v_dir)
        # Create a fake file to act as the true relative directory
        real_dir = self.workspace_root / "external/bar/include"
        self.fs.create_dir(real_dir)
        real_file = real_dir / "hdr.h"
        self.fs.create_file(real_file)

        # Symlink inside virtual include points to the real file
        symlink_path = v_dir / "hdr.h"
        self.fs.create_symlink(symlink_path, real_file)

        cmd = merger.remap_virtual_includes(
            cmd,
            vrmaps,
            self.output_path,
            self.workspace_root,
        )

        self.assertEqual(
            cmd.arguments[0],
            "-Iexternal/foo/include",
            "Failed to use mapping dict",
        )
        # Using suffix `external/bar/include` relative to workspace root here.
        # It's an absolute path but relative_to converts it. Wait,
        # `remap_virtual_includes` returns the relative_to logic directly!
        real_dir_str = "-I" + str(real_dir.relative_to(self.workspace_root))

        self.assertEqual(
            cmd.arguments[1],
            real_dir_str,
            "Failed dynamic resolution",
        )

    def test_resolve_bazel_out_paths_relative(self):
        """Test resolving bazel-out paths with relativization."""
        cmd = merger.CompileCommand(
            file="bazel-out/k8-fastbuild/bin/foo/bar.cc",
            directory="",
            arguments=["-Ibazel-out/k8-fastbuild/genfiles/include"],
        )
        # output_path is
        # /home/.../_bazel_somebody/123abc/execroot/_main/bazel-out
        # workspace_root is /workspace

        # We need to mock a scenario where the resolved path IS inside the
        # workspace or we want relative path to it from workspace.
        # But bazel-out is usually outside workspace.
        # ide_query.py relativize logic returns absolute if it cannot
        # relativize.
        # merger.py uses os.path.relpath which produces "../..." if possible.

        # Let's verify it produces relative path even if it's ".."

        # Create the bazel-out symlink so it gets relativized
        self.fs.create_dir(self.workspace_root / "bazel-out")

        cmd = merger.resolve_bazel_out_paths(
            cmd, self.output_path, relative_to=self.workspace_root
        )

        # Expected relative path from /workspace to .../bazel-out/...
        # /workspace -> /home/.../bazel-out/...
        # This depends on exact paths.
        # Let's use a simpler fake setup for this test if needed, or calculate
        # expected.

        expected_file_rel = "bazel-out/k8-fastbuild/bin/foo/bar.cc"
        expected_arg_rel = "bazel-out/k8-fastbuild/genfiles/include"

        self.assertEqual(cmd.file, expected_file_rel)
        self.assertEqual(cmd.arguments[0], f"-I{expected_arg_rel}")

    def test_resolve_external_paths_relative(self):
        """Test resolving external paths with relativization."""
        cmd = merger.CompileCommand(
            file="external/foo/bar.cc",
            directory="",
            arguments=["-Iexternal/foo/include"],
        )

        cmd = merger.resolve_external_paths(
            cmd, self.output_base, relative_to=self.workspace_root
        )

        # Since external/foo/... matches expected pattern but is NOT relative
        # to workspace_root (workspace_root is /workspace, output_base is
        # /home/somebody/...), it should fall back to absolute paths.
        expected_file = "external/foo/bar.cc"
        expected_arg = "external/foo/include"

        self.assertEqual(cmd.file, expected_file)

        self.assertEqual(cmd.arguments[0], f"-I{expected_arg}")

    def test_main_with_relative_paths(self):
        """Test that relativization of paths."""
        _create_fragment(
            self.fs,
            self.output_path,
            'target1',
            'k8-fastbuild',
            [
                {
                    'file': 'external/foo/bar.cc',
                    'directory': '__WORKSPACE_ROOT__',
                    'arguments': [],
                }
            ],
        )

        with mock.patch.object(sys, 'argv', ['merger.py']):
            self.assertEqual(merger.main(), 0)

        merged_path = (
            self.workspace_root
            / '.compile_commands'
            / 'k8-fastbuild'
            / 'compile_commands.json'
        )

        with open(merged_path, 'r') as f:
            data = json.load(f)

        expected_file_rel = "external/foo/bar.cc"
        self.assertEqual(data[0]['file'], expected_file_rel)

    def test_resolve_absolute_paths_relative(self):
        """Test resolving absolute paths with relativization."""
        # Create a fragment with an absolute path that is inside the output base
        abs_path = self.output_base / "external/protobuf/foo.cc"

        _create_fragment(
            self.fs,
            self.output_path,
            'target1',
            'k8-fastbuild',
            [
                {
                    'file': str(abs_path),
                    'directory': '__WORKSPACE_ROOT__',
                    'arguments': [],
                }
            ],
        )

        with mock.patch.object(sys, 'argv', ['merger.py']):
            self.assertEqual(merger.main(), 0)

        merged_path = (
            self.workspace_root
            / '.compile_commands'
            / 'k8-fastbuild'
            / 'compile_commands.json'
        )

        with open(merged_path, 'r') as f:
            data = json.load(f)

        expected_file_rel = "external/protobuf/foo.cc"
        self.assertEqual(data[0]['file'], expected_file_rel)

    def test_target_info_merge_deduplication(self):
        """Test target info dict accurately merges identical keys via sets."""
        _create_fragment(
            self.fs,
            self.output_path,
            'target1',
            'k8-fastbuild',
            [],
            target_info=[
                {
                    'label': '//my:target',
                    'hdrs': ['a.h'],
                    'srcs': ['a.cc'],
                    'deps': [],
                },
                {
                    'label': '//my:target',
                    'hdrs': ['b.h', 'a.h'],
                    'srcs': [],
                    'deps': ['//other:dep'],
                },
            ],
        )

        with mock.patch.object(sys, 'argv', ['merger.py']):
            self.assertEqual(merger.main(), 0)

        deps_mapping_path = (
            self.workspace_root
            / '.compile_commands'
            / 'k8-fastbuild'
            / 'deps_mapping.json'
        )

        with open(deps_mapping_path, 'r') as f:
            data = json.load(f)

        target_data = data['targets']['//my:target']
        self.assertEqual(set(target_data['hdrs']), {'a.h', 'b.h'})
        self.assertEqual(target_data['srcs'], ['a.cc'])
        self.assertEqual(target_data['deps'], ['//other:dep'])

    def test_local_path_override_resolution(self):
        """Test that local_path_override (symlink) is resolved to local path."""
        # Create a local dependency directory
        local_dep_path = self.workspace_root / 'dep1'
        self.fs.create_dir(local_dep_path)
        local_header = local_dep_path / 'header.h'
        self.fs.create_file(local_header)

        # Create a symlink in external/ pointing to it (simulating
        # local_path_override)
        external_dep_path = self.output_base / 'external' / 'dep1'
        self.fs.create_dir(self.output_base / 'external')
        self.fs.create_symlink(external_dep_path, local_dep_path)

        _create_fragment(
            self.fs,
            self.output_path,
            't1',
            'k8-fastbuild',
            [
                {
                    'file': 'external/dep1/lib.cc',
                    'directory': '__WORKSPACE_ROOT__',
                    'arguments': ['-Iexternal/dep1'],
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

        # EXPECTED BEHAVIOR: It should resolve to dep1/lib.cc
        self.assertEqual(data[0]['file'], 'dep1/lib.cc')
        self.assertEqual(data[0]['arguments'], ['-Idep1'])

    def test_new_local_repository_resolution(self):
        """Test that new_local_repository is resolved."""
        # Create the actual source directory in third_party
        third_party_path = self.workspace_root / 'third_party' / 'my_lib'
        self.fs.create_dir(third_party_path)
        self.fs.create_file(third_party_path / 'foo.c')

        # Create the symlink in external/ pointing to it
        external_repo_path = self.output_base / 'external' / 'my_lib'
        # ensure external dir exists (might be created by other tests)
        if not self.fs.exists(self.output_base / 'external'):
            self.fs.create_dir(self.output_base / 'external')
        self.fs.create_symlink(external_repo_path, third_party_path)

        _create_fragment(
            self.fs,
            self.output_path,
            'my_lib_target',
            'k8-fastbuild',
            [
                {
                    'file': 'external/my_lib/foo.c',
                    'directory': '__WORKSPACE_ROOT__',
                    'arguments': ['-Iexternal/my_lib/include'],
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

        # EXPECTED BEHAVIOR: It should resolve to third_party/my_lib/foo.c
        self.assertEqual(data[0]['file'], 'third_party/my_lib/foo.c')
        self.assertEqual(data[0]['arguments'], ['-Ithird_party/my_lib/include'])


if __name__ == '__main__':
    unittest.main()
