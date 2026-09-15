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
"""Tests for the DefectScanner class and related stages in pw_fortifier."""

# pylint: disable=protected-access

import asyncio
import io
from pathlib import Path
import sys
import unittest
from unittest.mock import MagicMock, patch, AsyncMock

from pyfakefs.fake_filesystem_unittest import TestCaseMixin
from pw_fortifier.async_path import AsyncPath
from pw_fortifier.code_analyzer import CodeAnalyzerStub
from pw_fortifier.code_snippet import CodeSnippet
from pw_fortifier.poc_and_fix_generator import PocAndFixGeneratorStub
from pw_fortifier.critic import CriticStub
from pw_fortifier.deduplicator import DeduplicatorStub
from pw_fortifier.git_utils import (
    ReadOnlyGitWorkspace,
    WritableGitWorkspace,
)
from pw_fortifier.issue_tracker import IssueWriter, IssueTrackerStub
from pw_fortifier.defect import Defect
from pw_fortifier.triager import TriagerStub
from pw_fortifier.defect_tracker import DefectSummarizer
from pw_fortifier.defect_scanner import (
    DefectScanner,
    Collector,
    DefectCollector,
)
from pw_fortifier.scanner import configure_stage_for_test

EXPECTED_HEADER = '| Issue ID  | Severity | CL     | Title' + ' ' * 80 + '|'
EXPECTED_SEPARATOR = '+-----------+----------+--------+' + '-' * 86 + '+'


class StubDefectSummarizer(DefectSummarizer):
    """Stub defect summarizer for testing."""

    def summarize(self, defect: Defect) -> tuple[str, str]:
        return ('PW-SEC-001', defect.title if defect.title else 'Defect')


class TestDefectScanner(unittest.IsolatedAsyncioTestCase, TestCaseMixin):
    """Unit tests for DefectScanner CLI parsing and execution."""

    def setUp(self) -> None:
        self.setUpPyfakefs()
        self.working_dir = AsyncPath('/working').resolve()
        self.fs.create_dir(self.working_dir.path)
        self.stub_defect = Defect(
            location=CodeSnippet(file='report-init.json'),
            issue_id=None,
            title='Fix and PoC Defect',
            description='A defect that needs testing and fixing.',
            cl_num=None,
            severity=2,
        )

        self.defect_scanner = DefectScanner('test_defect_scanner')
        self.defect_scanner.repo_url = (
            'https://pigweed.googlesource.com/pigweed/pigweed'
        )
        self.analyzer_stub = CodeAnalyzerStub()
        self.defect_scanner.code_analyzer = self.analyzer_stub
        self.critic_stub = CriticStub()
        self.defect_scanner.critic = self.critic_stub
        self.summarizer_stub = StubDefectSummarizer()
        self.defect_scanner.summarizer = self.summarizer_stub
        self.deduplicator_stub = DeduplicatorStub()
        self.deduplicator_stub.ISSUE_TYPE = Defect
        self.defect_scanner.deduplicator = self.deduplicator_stub
        self.triager_stub = TriagerStub(self.stub_defect)
        self.triager_stub.ISSUE_TYPE = Defect
        self.defect_scanner.triager = self.triager_stub
        self.issue_tracker_stub = IssueTrackerStub()
        self.issue_tracker_stub.ISSUE_TYPE = Defect
        self.defect_scanner.issue_tracker = self.issue_tracker_stub
        self.code_generator_stub = PocAndFixGeneratorStub()
        self.defect_scanner.code_generator = self.code_generator_stub

    def test_parse_args(self) -> None:
        """Tests parsing options in Scanner."""
        args = self.defect_scanner._parse_args(
            *['-w', '/tmp/work', '-m', '5', '-f', 'pw_sync*', '-v']
        )
        self.assertEqual(args.working_dir, '/tmp/work')
        self.assertEqual(args.max_retries, 5)
        self.assertEqual(args.files, ['pw_sync*'])
        self.assertTrue(args.verbose)

    def test_parse_args_issue_id_and_hotlist_id(self) -> None:
        """Tests parsing of issue_id and hotlist_id options."""
        args = self.defect_scanner._parse_args(
            *[
                '-i',
                '12345',
                '-i',
                '23456',
                '-l',
                '67890',
                '-l',
                '78901',
            ]
        )
        self.assertEqual(args.issues, [12345, 23456])
        self.assertEqual(args.hotlists, [67890, 78901])

    def test_parse_args_clean(self) -> None:
        """Tests parsing of clean option."""
        args_short = self.defect_scanner._parse_args('-c')
        self.assertTrue(args_short.clean)

        args_long = self.defect_scanner._parse_args('--clean')
        self.assertTrue(args_long.clean)

        args_default = self.defect_scanner._parse_args()
        self.assertFalse(args_default.clean)

    @patch('pw_fortifier.scanner.ReadOnlyGitWorkspace')
    @patch('pw_fortifier.scanner.WritableGitWorkspace')
    async def test_configure_clean_working_dir(
        self,
        mock_writable_git_workspace_class: MagicMock,
        mock_read_only_git_workspace_class: MagicMock,
    ) -> None:
        """Tests that _configure clears working_dir if clean=True."""
        del mock_writable_git_workspace_class
        del mock_read_only_git_workspace_class
        work_dir = Path('/tmp/test_clean_work')
        old_file = work_dir / 'old_file.txt'
        old_subdir = work_dir / 'old_subdir'
        self.fs.create_file(old_file, contents='old')
        self.fs.create_dir(old_subdir)
        self.fs.create_file(old_subdir / 'nested.txt', contents='nested')

        args = self.defect_scanner._parse_args(
            '-w',
            str(work_dir),
            '-c',
            '-s',
            str(self.working_dir.path),
            '-d',
            str(self.working_dir.path),
        )
        await self.defect_scanner._configure(args)

        self.assertTrue(work_dir.exists())
        self.assertFalse(old_file.exists())
        self.assertFalse(old_subdir.exists())

    @patch('pw_fortifier.scanner.get_git_repo_root', return_value=None)
    @patch('pw_fortifier.scanner.ReadOnlyGitWorkspace')
    @patch('pw_fortifier.scanner.WritableGitWorkspace')
    async def test_run_pipeline_end_to_end(
        self,
        mock_writable_git_workspace_class: MagicMock,
        mock_read_only_git_workspace_class: MagicMock,
        mock_get_git_repo_root: MagicMock,
    ) -> None:
        """Tests a full pipeline run starting from Emitter using stubs."""
        del mock_get_git_repo_root
        mock_branch = MagicMock()
        mock_branch.setup = AsyncMock(return_value=mock_branch)
        mock_branch.teardown = AsyncMock()
        mock_branch.add = AsyncMock()
        mock_branch.diff = AsyncMock(return_value=['diff line 1'])
        mock_branch.commit = AsyncMock()
        mock_branch.push = AsyncMock(return_value=123456)

        mock_internal_repo = MagicMock(spec=WritableGitWorkspace)
        mock_internal_repo.project_dir = self.working_dir.path / 'internal'
        mock_internal_repo.diffs = AsyncMock(return_value=['diff line 1'])
        mock_internal_repo.push = AsyncMock(return_value=123456)
        mock_internal_repo.create_branch = AsyncMock()
        mock_internal_repo.delete_branch = AsyncMock()
        mock_internal_repo.branch = MagicMock(return_value=mock_branch)

        mock_public_repo = MagicMock(spec=ReadOnlyGitWorkspace)
        mock_public_repo.project_dir = self.working_dir.path / 'public'
        mock_public_repo.project_dir.mkdir(parents=True, exist_ok=True)
        (mock_public_repo.project_dir / 'OWNERS').write_text(
            'owner@google.com\n'
        )
        (mock_public_repo.project_dir / 'foo.cc').touch()

        mock_read_only_git_workspace_class.clone = AsyncMock(
            return_value=mock_public_repo
        )
        mock_writable_git_workspace_class.clone = AsyncMock(
            return_value=mock_internal_repo
        )

        # Capture stdout to verify the summary table print from Collector
        captured_stdout = io.StringIO()
        sys.stdout = captured_stdout

        try:
            await self.defect_scanner.run(
                *['-w', str(self.working_dir), '-b', '-e', '-u', '-f', 'foo.cc']
            )
        finally:
            sys.stdout = sys.__stdout__

        # Verify that stages were connected:
        # emitter -> scan -> challenge -> deduplicate -> triage -> report ->
        # generate -> terminate
        assert self.defect_scanner._emitter is not None
        self.assertIs(
            self.defect_scanner._emitter._output_queue,
            self.analyzer_stub.input_queue,
        )
        self.assertIs(
            self.analyzer_stub._output_queue,
            self.critic_stub.input_queue,
        )
        self.assertIs(
            self.critic_stub._output_queue,
            self.deduplicator_stub.input_queue,
        )
        self.assertIs(
            self.deduplicator_stub._output_queue,
            self.triager_stub.input_queue,
        )
        issue_writer = next(
            stage
            for stage in self.defect_scanner._stages
            if isinstance(stage, IssueWriter)
        )
        collector = next(
            stage
            for stage in self.defect_scanner._stages
            if isinstance(stage, Collector)
        )
        assert isinstance(collector, Collector)
        self.assertIs(
            self.triager_stub._output_queue,
            issue_writer.input_queue,
        )
        self.assertIs(
            self.code_generator_stub._output_queue,
            collector.input_queue,
        )

        # Verify table formatting in stdout (now printed by Collector)
        output_str = captured_stdout.getvalue()
        self.assertIn(EXPECTED_HEADER, output_str)
        expected_title = 'PW-SEC-001 - report-init.json: Fix and PoC Defect'
        expected_row = (
            f"| {'8675309':<9} | {'S2':<8} | {'123456':<6} | "
            f"{expected_title:<84} |"
        )
        self.assertIn(expected_row, output_str)
        self.assertIn('DONE! Processed 1 reports.', output_str)

        # Verify correct repository was used
        mock_branch.push.assert_called_once()
        mock_internal_repo.push.assert_not_called()

    @patch('pw_fortifier.scanner.get_git_repo_root', return_value=None)
    @patch('pw_fortifier.scanner.ReadOnlyGitWorkspace')
    @patch('pw_fortifier.scanner.WritableGitWorkspace')
    async def test_run_pipeline_scan_only(
        self,
        mock_writable_git_workspace_class: MagicMock,
        mock_read_only_git_workspace_class: MagicMock,
        mock_get_git_repo_root: MagicMock,
    ) -> None:
        """Tests scan stage without and_following, verifying outputs."""
        del mock_get_git_repo_root
        # Create mock repos
        mock_branch = MagicMock()
        mock_branch.setup = AsyncMock(return_value=mock_branch)
        mock_branch.teardown = AsyncMock()
        mock_branch.add = AsyncMock()
        mock_branch.diff = AsyncMock(return_value=['diff line 1'])
        mock_branch.commit = AsyncMock()
        mock_branch.push = AsyncMock(return_value=123456)

        mock_internal_repo = MagicMock(spec=WritableGitWorkspace)
        mock_internal_repo.project_dir = self.working_dir.path / 'internal'
        mock_internal_repo.branch = MagicMock(return_value=mock_branch)

        mock_public_repo = MagicMock(spec=ReadOnlyGitWorkspace)
        mock_public_repo.project_dir = self.working_dir.path / 'public'

        mock_read_only_git_workspace_class.clone = AsyncMock(
            return_value=mock_public_repo
        )
        mock_writable_git_workspace_class.clone = AsyncMock(
            return_value=mock_internal_repo
        )

        # Create a mock source file in the repo
        repo_dir = self.working_dir.path / 'public'
        repo_dir.mkdir(parents=True, exist_ok=True)
        (repo_dir / 'OWNERS').write_text('owner@google.com\n')
        source_file = repo_dir / 'foo.cc'
        source_file.touch()

        # Create an emitted file (representing Emitter output) in working_dir
        emitted_file = repo_dir / '000000-foo.cc.txt'
        emitted_file.write_text(str(source_file))

        # Capture stdout to verify the raw table print from Collector
        captured_stdout = io.StringIO()
        sys.stdout = captured_stdout

        try:
            await self.defect_scanner.run(
                *[
                    '-w',
                    str(self.working_dir),
                    '-b',
                    '-e',
                    '-u',
                    '-f',
                    str(emitted_file),
                ]
            )
        finally:
            sys.stdout = sys.__stdout__

        collector = next(
            stage
            for stage in self.defect_scanner._stages
            if isinstance(stage, Collector)
        )
        assert isinstance(collector, Collector)

        # Verify that stages were connected:
        # scan (Analyzer) -> terminate (Collector)
        self.assertIs(
            self.analyzer_stub._output_queue,
            self.critic_stub.input_queue,
        )

        # Verify collector output in stdout
        output_str = captured_stdout.getvalue()
        self.assertIn(EXPECTED_HEADER, output_str)
        self.assertIn(EXPECTED_SEPARATOR, output_str)

        expected_title = 'PW-SEC-001 - report-init.json: Fix and PoC Defect'
        expected_row = (
            f"| {'8675309':<9} | {'S2':<8} | {'123456':<6} | "
            f"{expected_title:<84} |"
        )
        self.assertIn(expected_row, output_str)

        self.assertIn('DONE! Processed 1 reports.', output_str)

    @patch('pw_fortifier.scanner.ReadOnlyGitWorkspace')
    @patch('pw_fortifier.scanner.WritableGitWorkspace')
    async def test_run_pipeline_with_provided_repos(
        self,
        mock_writable_git_workspace_class: MagicMock,
        mock_read_only_git_workspace_class: MagicMock,
    ) -> None:
        """Tests using provided repo paths instead of cloning."""
        # Setup mock instances returned by the constructor
        mock_branch = MagicMock()
        mock_branch.setup = AsyncMock(return_value=mock_branch)
        mock_branch.teardown = AsyncMock()
        mock_branch.add = AsyncMock()
        mock_branch.diff = AsyncMock(return_value=['diff line 1'])
        mock_branch.commit = AsyncMock()
        mock_branch.push = AsyncMock(return_value=123456)

        mock_internal_repo = MagicMock(spec=WritableGitWorkspace)
        custom_internal = Path('/custom/internal').resolve()
        mock_internal_repo.project_dir = custom_internal
        mock_internal_repo.diffs = AsyncMock(return_value=['diff line 1'])
        mock_internal_repo.push = AsyncMock(return_value=123456)
        mock_internal_repo.create_branch = AsyncMock()
        mock_internal_repo.delete_branch = AsyncMock()
        mock_internal_repo.branch = MagicMock(return_value=mock_branch)

        custom_public = self.working_dir.path / 'custom_public'
        custom_public.mkdir(parents=True, exist_ok=True)
        (custom_public / 'OWNERS').write_text('owner@google.com\n')
        (custom_public / 'foo.cc').touch()

        mock_public_repo = MagicMock(spec=ReadOnlyGitWorkspace)
        mock_public_repo.project_dir = custom_public

        mock_writable_git_workspace_class.return_value = mock_internal_repo
        mock_read_only_git_workspace_class.return_value = mock_public_repo

        captured_stdout = io.StringIO()
        sys.stdout = captured_stdout

        try:
            await self.defect_scanner.run(
                *[
                    '-w',
                    str(self.working_dir),
                    '-d',
                    str(custom_internal),
                    '-s',
                    str(custom_public),
                    '-b',
                    '-e',
                    '-u',
                    '-f',
                    'foo.cc',
                ]
            )
        finally:
            sys.stdout = sys.__stdout__

        # Verify GitWorkspace.clone was NOT called
        mock_writable_git_workspace_class.clone.assert_not_called()
        mock_read_only_git_workspace_class.clone.assert_not_called()

        # Verify GitWorkspace constructor was called with the correct paths
        mock_writable_git_workspace_class.assert_called_once_with(
            custom_internal
        )
        mock_read_only_git_workspace_class.assert_called_once_with(
            custom_public
        )

        # Verify the pipeline ran and used the provided repos
        mock_branch.push.assert_called_once()

        # Verify src_repo and dst_repo were set on stages
        analyzer_src_repo = self.analyzer_stub.src_repo
        assert analyzer_src_repo is not None
        self.assertEqual(
            analyzer_src_repo.project_dir,
            custom_public,
        )
        codegen_dst_repo = self.code_generator_stub.dst_repo
        assert codegen_dst_repo is not None
        self.assertEqual(
            codegen_dst_repo.project_dir,
            custom_internal,
        )

        # Verify internal/public repos were set on CodeGenerator
        self.assertIs(
            codegen_dst_repo,
            mock_internal_repo,
        )
        codegen_src_repo = self.code_generator_stub.src_repo
        assert codegen_src_repo is not None
        self.assertIs(
            codegen_src_repo,
            mock_public_repo,
        )


class TestDefectCollector(unittest.IsolatedAsyncioTestCase, TestCaseMixin):
    """Unit tests for the DefectCollector class."""

    def setUp(self) -> None:
        self.setUpPyfakefs()
        self.working_dir = AsyncPath('/working').resolve()
        self.fs.create_dir(self.working_dir.path)

    async def test_process_one_collects_defect_details(self) -> None:
        """Tests DefectCollector extracts and aggregates defects in memory."""
        collector = DefectCollector()

        await configure_stage_for_test(
            collector,
            working_dir=str(self.working_dir),
        )

        # Save two defects to the filesystem
        defect_file1 = AsyncPath(self.working_dir, 'report1.json')
        defect1 = Defect(
            location=CodeSnippet(file='report1.json'),
            issue_id=111,
            title='Defect One',
            description='First bug details.',
            cl_num=None,
            severity=2,
        )
        await defect1.save(defect_file1)

        defect_file2 = AsyncPath(self.working_dir, 'report2.json')
        defect2 = Defect(
            location=CodeSnippet(file='report2.json'),
            issue_id=222,
            title='Defect Two',
            description='Second bug details.',
            cl_num=55555,
            severity=4,
        )
        await defect2.save(defect_file2)

        # Start stage execution in background
        # Capture stdout to verify printed output
        captured_stdout = io.StringIO()
        with patch('sys.stdout', new=captured_stdout):
            run_task = asyncio.create_task(collector.run())

            # Send both defects and then sentinel
            await collector._input_queue.put(defect_file1)
            await collector._input_queue.put(defect_file2)
            await collector._input_queue.put(None)
            await run_task

        # Verify output in stdout
        output_str = captured_stdout.getvalue()
        self.assertIn(EXPECTED_HEADER, output_str)
        self.assertIn(EXPECTED_SEPARATOR, output_str)

        # Verify the two rows
        self.assertIn(
            f"| {'111':<9} | {'S2':<8} | {'':<6} | {'Defect One':<84} |",
            output_str,
        )
        self.assertIn(
            f"| {'222':<9} | {'S4':<8} | {'55555':<6} | {'Defect Two':<84} |",
            output_str,
        )

        # Verify count in summary
        self.assertIn('DONE! Processed 2 reports.', output_str)


if __name__ == '__main__':
    unittest.main()
