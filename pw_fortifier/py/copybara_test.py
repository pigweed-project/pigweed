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
"""Tests for copybara."""
# pylint: disable=protected-access

from datetime import date
import os
import subprocess
import unittest
from unittest.mock import MagicMock, patch

from pyfakefs.fake_filesystem_unittest import TestCaseMixin
from pw_fortifier.async_path import AsyncPath
from pw_fortifier.copybara import CopybaraAnalyzer
from pw_fortifier.freshness_result import PackageVersion, FreshnessResult
from pw_fortifier.pipeline_stage import PipelineSink
from pw_fortifier.scanner import configure_stage_for_test


class TestCopybaraAnalyzer(unittest.IsolatedAsyncioTestCase, TestCaseMixin):
    """Tests for CopybaraAnalyzer."""

    def setUp(self):
        """Set up test environment."""
        self.setUpPyfakefs()
        self.test_dir = '/test'
        self.working_dir = '/working'
        self._date_patcher = None

        # Create fake third_party/fuchsia/copy.bara.sky
        self.fuchsia_dir = os.path.join(self.test_dir, 'third_party', 'fuchsia')
        self.copybara_sky = os.path.join(self.fuchsia_dir, 'copy.bara.sky')
        copybara_content = (
            'core.workflow(\n'
            '    name = "default",\n'
            '    origin = git.origin(\n'
            '        url = "https://github.com/fuchsia/something.git",\n'
            '    ),\n'
            ')\n'
        )
        self.fs.create_file(self.copybara_sky, contents=copybara_content)

        # Create fake repo dir so git log path exists
        self.repo_dir = os.path.join(self.fuchsia_dir, 'repo')
        self.fs.create_dir(self.repo_dir)

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
            args = cmd[1:]
            if args and args[0] == '-C':
                args = args[2:]

            if args:
                subcmd = args[0]
                if subcmd == 'log':
                    has_grep = '--grep=GitOrigin-RevId' in cmd
                    has_fmt = '--format=%H,%cd' in cmd
                    if has_grep and has_fmt:
                        return subprocess.CompletedProcess(
                            args=cmd,
                            returncode=0,
                            stdout='curr_rev_123,2026-06-01 12:00:00 -0700\n',
                            stderr='',
                        )
                    if '--format=%H,%cd' in cmd:
                        stdout = (
                            'migrated_rev_456_long,2026-06-05 12:00:00 -0700\n'
                            'migrated_rev_123_mid,2026-06-03 12:00:00 -0700\n'
                            'curr_rev_123,2026-06-01 12:00:00 -0700\n'
                        )
                        return subprocess.CompletedProcess(
                            args=cmd, returncode=0, stdout=stdout, stderr=''
                        )
                    return subprocess.CompletedProcess(
                        args=cmd,
                        returncode=0,
                        stdout='hash1 Fake commit\n',
                        stderr='',
                    )
                if subcmd == 'clone':
                    cwd = _kwargs.get('cwd')
                    if cwd:
                        os.makedirs(os.path.join(cwd, 'src'), exist_ok=True)
                    return subprocess.CompletedProcess(
                        args=cmd, returncode=0, stdout='', stderr=''
                    )
                if subcmd == 'show':
                    arg = cmd[cmd.index('show') + 1]
                    path = arg.split(':')[-1]
                    return subprocess.CompletedProcess(
                        args=cmd, returncode=0, stdout=f'{path}\n', stderr=''
                    )
                if subcmd == 'blame':
                    filepath = cmd[-1]
                    if os.path.isabs(filepath):
                        rel_path = os.path.relpath(filepath, self.test_dir)
                    else:
                        rel_path = filepath
                    if 'copy.bara.sky' in rel_path:
                        norm_rel_path = rel_path.replace(os.sep, '/')
                        parts = norm_rel_path.split('/')
                        project = parts[-2] if len(parts) > 1 else 'default'
                        stdout = (
                            f'hash1 (<assignee-{project}@google.com> '
                            '2026-06-01 10:00:00 +0000 1) // '
                            'fake copy.bara.sky\n'
                        )
                        return subprocess.CompletedProcess(
                            args=cmd, returncode=0, stdout=stdout, stderr=''
                        )
                    return subprocess.CompletedProcess(
                        args=cmd, returncode=0, stdout='', stderr=''
                    )

        raise ValueError(f'Unexpected command: {cmd}')

    @patch('subprocess.run')
    @patch('pw_fortifier.find_core_owners.CoreOwnerFinder.core_members')
    async def test_run(self, mock_cores, mock_run):
        """Test scanning copybara packages."""
        mock_run.side_effect = self._mock_subprocess_run
        mock_cores.return_value = {'assignee-fuchsia@google.com'}

        analyzer = CopybaraAnalyzer()
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
            AsyncPath(self.test_dir) / 'third_party/fuchsia/copy.bara.sky'
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

        self.assertEqual(len(results), 1)
        res = results[0]
        self.assertEqual(res.package, 'fuchsia')
        self.assertEqual(
            res.source, os.path.normpath('third_party/fuchsia/copy.bara.sky')
        )
        self.assertEqual(res.pkg_type, 'copybara')
        self.assertEqual(
            res.current, PackageVersion('curr_rev_123', date(2026, 6, 1))
        )
        self.assertEqual(
            res.earliest, PackageVersion('curr_rev_123', date(2026, 6, 1))
        )
        self.assertEqual(res.tier, 0)
        self.assertIsNone(res.assignee)
        self.assertEqual(res.location.file, res.source)

    @patch('subprocess.run')
    @patch('pw_fortifier.find_core_owners.CoreOwnerFinder.core_members')
    async def test_run_nested(self, mock_cores, mock_run):
        """Test scanning nested copybara packages."""
        # Create fake third_party/nested/foo/copy.bara.sky
        nested_dir = os.path.join(self.test_dir, 'third_party', 'nested', 'foo')
        os.makedirs(nested_dir)
        copybara_sky = os.path.join(nested_dir, 'copy.bara.sky')
        with open(copybara_sky, 'w') as f:
            f.write(
                'core.workflow(\n'
                '    name = "default",\n'
                '    origin = git.origin(\n'
                '        url = "https://github.com/nested/foo.git",\n'
                '    ),\n'
                ')\n'
            )
        repo_dir = os.path.join(nested_dir, 'repo')
        os.makedirs(repo_dir)

        mock_run.side_effect = self._mock_subprocess_run
        mock_cores.return_value = {'assignee-foo@google.com'}

        analyzer = CopybaraAnalyzer()
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
            AsyncPath(self.test_dir) / 'third_party/nested/foo/copy.bara.sky'
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

        self.assertEqual(len(results), 1)
        res = results[0]
        self.assertEqual(res.package, 'foo')
        self.assertEqual(
            res.source,
            os.path.normpath('third_party/nested/foo/copy.bara.sky'),
        )
        self.assertEqual(res.pkg_type, 'copybara')
        self.assertIsNone(res.assignee)
        self.assertEqual(res.location.file, res.source)

    async def test_process_one_root_path_raises_value_error(self) -> None:
        """Test copy.bara.sky at root without parent project dir raises."""
        root_sky = os.path.join(self.test_dir, 'copy.bara.sky')
        self.fs.create_file(root_sky, contents='core.workflow()')

        analyzer = CopybaraAnalyzer()
        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            max_retries=0,
        )

        with self.assertRaises(ValueError) as ctx:
            await analyzer._process_one(AsyncPath(root_sky))
        self.assertIn('Cannot determine project name', str(ctx.exception))

    async def test_process_one_missing_origin_url_raises_value_error(
        self,
    ) -> None:
        """Test copy.bara.sky without git origin URL raises ValueError."""
        no_url_dir = os.path.join(self.test_dir, 'third_party', 'no_url')
        sky_file = os.path.join(no_url_dir, 'copy.bara.sky')
        self.fs.create_file(sky_file, contents='core.workflow(name = "x")')

        analyzer = CopybaraAnalyzer()
        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            max_retries=0,
        )

        with self.assertRaises(ValueError) as ctx:
            await analyzer._process_one(AsyncPath(sky_file))
        self.assertIn('No git origin URL found', str(ctx.exception))

    @patch('subprocess.run')
    async def test_process_one_missing_local_versions_raises_runtime_error(
        self, mock_run
    ) -> None:
        """Test missing GitOrigin-RevId in local repo raises RuntimeError."""
        mock_run.return_value = subprocess.CompletedProcess(
            args=[], returncode=0, stdout='', stderr=''
        )

        analyzer = CopybaraAnalyzer()
        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            max_retries=0,
        )

        with self.assertRaises(RuntimeError) as ctx:
            await analyzer._process_one(AsyncPath(self.copybara_sky))
        self.assertIn('No GitOrigin-RevId commits found', str(ctx.exception))

    @patch('subprocess.run')
    @patch('pw_fortifier.git_utils.ReadOnlyGitWorkspace.get_versions')
    @patch('pw_fortifier.git_utils.ReadOnlyGitWorkspace.clone')
    async def test_process_one_missing_upstream_versions_raises(
        self, mock_clone, mock_get_versions, _mock_run
    ) -> None:
        """Test empty versions in upstream repo raises RuntimeError."""
        mock_get_versions.return_value = [
            PackageVersion('curr_rev_123', date(2026, 6, 1))
        ]
        mock_upstream = MagicMock()
        mock_upstream.get_versions.return_value = []
        mock_clone.return_value = mock_upstream

        analyzer = CopybaraAnalyzer()
        await configure_stage_for_test(
            analyzer,
            src_repo=self.test_dir,
            working_dir=self.working_dir,
            max_retries=0,
        )

        with self.assertRaises(RuntimeError) as ctx:
            await analyzer._process_one(AsyncPath(self.copybara_sky))
        self.assertIn('Failed to resolve any versions', str(ctx.exception))


if __name__ == '__main__':
    unittest.main()
