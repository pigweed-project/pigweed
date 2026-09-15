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
"""Tests for freshness_scanner."""
# pylint: disable=protected-access

from datetime import date
import logging
import os
from pathlib import Path
import unittest
from unittest.mock import AsyncMock, MagicMock, patch

from pyfakefs.fake_filesystem_unittest import TestCaseMixin
from pw_fortifier.async_path import AsyncPath
from pw_fortifier.code_snippet import CodeSnippet
from pw_fortifier.freshness_collector import FreshnessResultCollector
from pw_fortifier.freshness_result import FreshnessResult, PackageVersion
from pw_fortifier.freshness_scanner import FreshnessScanner
from pw_fortifier.issue_tracker import IssueTrackerStub
from pw_fortifier.package_analyzer import PackageAnalyzer, PackageAnalyzerStub
from pw_fortifier.package_updater import PackageUpdaterStub
from pw_fortifier.roll_generator import (
    RollGenerator,
    RollGeneratorStub,
)


def create_test_freshness_scanner(
    name: str = 'test_registry',
) -> FreshnessScanner:
    """Creates a configured FreshnessScanner for testing."""
    scanner = FreshnessScanner(name)
    scanner.repo_url = 'https://pigweed.googlesource.com/pigweed/pigweed'
    tracker_stub = IssueTrackerStub()
    tracker_stub.ISSUE_TYPE = FreshnessResult
    tracker_stub.component_id = 12345
    scanner.issue_tracker = tracker_stub
    scanner.code_generator = RollGeneratorStub()
    return scanner


class FooPackageAnalyzerStub(PackageAnalyzerStub):
    """Stub package analyzer targeting foo.json."""

    TARGET = 'foo.json'


class BazPackageAnalyzerStub(PackageAnalyzerStub):
    """Stub package analyzer targeting baz.json."""

    TARGET = 'baz.json'


class QuxPackageAnalyzerStub(PackageAnalyzerStub):
    """Stub package analyzer targeting qux.json."""

    TARGET = 'qux.json'


class IgnoredPackageAnalyzerStub(PackageAnalyzerStub):
    """Stub package analyzer targeting ignored.json."""

    TARGET = 'ignored.json'


class DeepPackageAnalyzerStub(PackageAnalyzerStub):
    """Stub package analyzer targeting deep.json."""

    TARGET = 'deep.json'


class LifecyclePackageAnalyzer(PackageAnalyzer):
    """Package scanner to test _set_up and _tear_down lifecycle."""

    TARGET = 'foo.json'

    def __init__(self):
        super().__init__()
        self.prescan_called = False
        self.postscan_called = False
        self.scan_called = False

    async def _set_up(self) -> None:
        self.prescan_called = True
        result = FreshnessResult(
            package='setup-pkg',
            location=CodeSnippet(file='setup'),
            pkg_type='lifecycle',
            current=PackageVersion('1.0', date(2026, 1, 1)),
            earliest=PackageVersion('1.0', date(2026, 1, 1)),
            tier=1,
            assignee='me',
        )
        result_path = await self._generate_out_path()
        await result.save(result_path)
        self.send(result_path)

    async def _process_one(self, path: AsyncPath) -> None:
        self.scan_called = True
        result = FreshnessResult(
            package='scan-pkg',
            location=CodeSnippet(file=path.name),
            pkg_type='lifecycle',
            current=PackageVersion('1.0', date(2026, 1, 1)),
            earliest=PackageVersion('1.0', date(2026, 1, 1)),
            tier=1,
            assignee='me',
        )
        result_path = await self._generate_out_path()
        await result.save(result_path)
        self.send(result_path)

    async def _tear_down(self) -> None:
        self.postscan_called = True
        result = FreshnessResult(
            package='teardown-pkg',
            location=CodeSnippet(file='teardown'),
            pkg_type='lifecycle',
            current=PackageVersion('1.0', date(2026, 1, 1)),
            earliest=PackageVersion('1.0', date(2026, 1, 1)),
            tier=1,
            assignee='me',
        )
        result_path = await self._generate_out_path()
        await result.save(result_path)
        self.send(result_path)
        await super()._tear_down()


class NoneTargetPackageAnalyzer(PackageAnalyzer):
    """Scanner with None target, only runs _set_up and _tear_down."""

    def __init__(self):
        super().__init__()
        self.prescan_called = False
        self.postscan_called = False
        self.scan_called = False

    async def _set_up(self) -> None:
        self.prescan_called = True
        result = FreshnessResult(
            package='none-pattern-setup',
            location=CodeSnippet(file='setup'),
            pkg_type='lifecycle',
            current=PackageVersion('1.0', date(2026, 1, 1)),
            earliest=PackageVersion('1.0', date(2026, 1, 1)),
            tier=1,
            assignee='me',
        )
        result_path = await self._generate_out_path()
        await result.save(result_path)
        self.send(result_path)
        self.input_queue.put_nowait(None)

    async def _process_one(self, path: AsyncPath) -> None:
        self.scan_called = True

    async def _tear_down(self) -> None:
        self.postscan_called = True
        result = FreshnessResult(
            package='none-pattern-teardown',
            location=CodeSnippet(file='teardown'),
            pkg_type='lifecycle',
            current=PackageVersion('1.0', date(2026, 1, 1)),
            earliest=PackageVersion('1.0', date(2026, 1, 1)),
            tier=1,
            assignee='me',
        )
        result_path = await self._generate_out_path()
        await result.save(result_path)
        self.send(result_path)
        await super()._tear_down()


class TestFreshnessScanner(unittest.IsolatedAsyncioTestCase, TestCaseMixin):
    """Tests for FreshnessScanner."""

    def setUp(self):
        """Set up test environment."""
        self.setUpPyfakefs()
        self.test_dir = Path('/test').resolve()

        # Create structure:
        # /test/
        #   foo.json
        #   bar.py
        #   out/
        #     baz.json
        #   nested/
        #     qux.json
        #     pruned/
        #       ignored.json
        #     other/
        #       deep.json

        self.fs.create_file(
            self.test_dir / 'OWNERS', contents='owner@google.com\n'
        )
        self.fs.create_file(self.test_dir / 'foo.json', contents='{}')
        self.fs.create_file(self.test_dir / 'bar.py', contents='# python')
        self.fs.create_file(self.test_dir / 'out/baz.json', contents='{}')
        self.fs.create_file(self.test_dir / 'nested/qux.json', contents='{}')
        self.fs.create_file(
            self.test_dir / 'nested/pruned/ignored.json', contents='{}'
        )
        self.fs.create_file(
            self.test_dir / 'nested/other/deep.json', contents='{}'
        )

    def write_file(self, path: Path | str, content: str) -> None:
        """Helper to write file."""
        self.fs.create_file(path, contents=content)

    @patch('pw_fortifier.scanner.WritableGitWorkspace')
    @patch(
        'pw_fortifier.freshness_collector.FreshnessResultCollector._print_item'
    )
    async def test_default_no_gitignore(self, mock_print, mock_git_ws_cls):
        """Test scanning includes all matches when no .gitignore is present."""
        mock_branch = MagicMock()
        mock_branch.setup = AsyncMock(return_value=mock_branch)
        mock_branch.teardown = AsyncMock()
        mock_branch.add = AsyncMock()
        mock_branch.commit = AsyncMock()
        mock_branch.commit_msg = AsyncMock(
            return_value=['roll: Update 3p deps', '', 'Bug: 123']
        )
        mock_branch.push = AsyncMock(return_value=123456)
        mock_repo = MagicMock()
        mock_repo.project_dir = self.test_dir
        mock_repo.branch = MagicMock(return_value=mock_branch)
        mock_git_ws_cls.return_value = mock_repo
        results = []

        async def log_result(path):
            result = await FreshnessResult.load(path)
            results.append(result)

        mock_print.side_effect = log_result

        freshness_scanner = create_test_freshness_scanner('test_registry')
        scanners = [
            FooPackageAnalyzerStub(),
            BazPackageAnalyzerStub(),
            QuxPackageAnalyzerStub(),
            IgnoredPackageAnalyzerStub(),
            DeepPackageAnalyzerStub(),
        ]
        for s in scanners:
            freshness_scanner.register(s)

        await freshness_scanner.run(
            '--src-repo', str(self.test_dir), '--dst-repo', str(self.test_dir)
        )

        # Should find everything including out/baz.json
        expected_paths = {
            os.path.normpath(str(self.test_dir / 'foo.json')),
            os.path.normpath(str(self.test_dir / 'out/baz.json')),
            os.path.normpath(str(self.test_dir / 'nested/qux.json')),
            os.path.normpath(str(self.test_dir / 'nested/pruned/ignored.json')),
            os.path.normpath(str(self.test_dir / 'nested/other/deep.json')),
        }
        scanned_paths = set()
        for s in scanners:
            scanned_paths.update(s.scanned_paths)
        self.assertEqual(scanned_paths, expected_paths)
        self.assertEqual(len(results), 5)

    @patch('pw_fortifier.scanner.WritableGitWorkspace')
    @patch(
        'pw_fortifier.freshness_collector.FreshnessResultCollector._print_item'
    )
    async def test_gitignore_exclusion(self, mock_print, mock_git_ws_cls):
        """Test scanning excludes files matching .gitignore."""
        self.write_file(self.test_dir / '.gitignore', 'nested/pruned/\n')

        mock_branch = MagicMock()
        mock_branch.setup = AsyncMock(return_value=mock_branch)
        mock_branch.teardown = AsyncMock()
        mock_branch.add = AsyncMock()
        mock_branch.commit = AsyncMock()
        mock_branch.commit_msg = AsyncMock(
            return_value=['roll: Update 3p deps', '', 'Bug: 123']
        )
        mock_branch.push = AsyncMock(return_value=123456)
        mock_repo = MagicMock()
        mock_repo.project_dir = self.test_dir
        mock_repo.branch = MagicMock(return_value=mock_branch)
        mock_git_ws_cls.return_value = mock_repo
        results = []

        async def log_result(path):
            result = await FreshnessResult.load(path)
            results.append(result)

        mock_print.side_effect = log_result

        freshness_scanner = create_test_freshness_scanner('test_registry')
        scanners = [
            FooPackageAnalyzerStub(),
            BazPackageAnalyzerStub(),
            QuxPackageAnalyzerStub(),
            IgnoredPackageAnalyzerStub(),
            DeepPackageAnalyzerStub(),
        ]
        for s in scanners:
            freshness_scanner.register(s)

        await freshness_scanner.run(
            '--src-repo', str(self.test_dir), '--dst-repo', str(self.test_dir)
        )

        # Should find foo.json, out/baz.json, nested/qux.json, deep.json
        # Should NOT find nested/pruned/ignored.json because of .gitignore
        expected_paths = {
            os.path.normpath(str(self.test_dir / 'foo.json')),
            os.path.normpath(str(self.test_dir / 'out/baz.json')),
            os.path.normpath(str(self.test_dir / 'nested/qux.json')),
            os.path.normpath(str(self.test_dir / 'nested/other/deep.json')),
        }
        scanned_paths = set()
        for s in scanners:
            scanned_paths.update(s.scanned_paths)
        self.assertEqual(scanned_paths, expected_paths)
        self.assertEqual(len(results), 4)

    @patch('pw_fortifier.scanner.WritableGitWorkspace')
    @patch(
        'pw_fortifier.freshness_collector.FreshnessResultCollector._print_item'
    )
    async def test_lifecycle_methods(self, mock_print, mock_git_ws_cls):
        """Test that pre_scan and post_scan are called and yield results."""
        mock_branch = MagicMock()
        mock_branch.setup = AsyncMock(return_value=mock_branch)
        mock_branch.teardown = AsyncMock()
        mock_branch.add = AsyncMock()
        mock_branch.commit = AsyncMock()
        mock_branch.commit_msg = AsyncMock(
            return_value=['roll: Update 3p deps', '', 'Bug: 123']
        )
        mock_branch.push = AsyncMock(return_value=123456)
        mock_repo = MagicMock()
        mock_repo.project_dir = self.test_dir
        mock_repo.branch = MagicMock(return_value=mock_branch)
        mock_git_ws_cls.return_value = mock_repo
        results = []

        async def log_result(path):
            result = await FreshnessResult.load(path)
            results.append(result)

        mock_print.side_effect = log_result

        freshness_scanner = create_test_freshness_scanner('test_registry')
        scanner = LifecyclePackageAnalyzer()
        freshness_scanner.register(scanner)

        await freshness_scanner.run(
            '--src-repo', str(self.test_dir), '--dst-repo', str(self.test_dir)
        )

        self.assertTrue(scanner.prescan_called)
        self.assertTrue(scanner.scan_called)
        self.assertTrue(scanner.postscan_called)

        self.assertEqual(len(results), 3)
        self.assertEqual(results[0].package, 'setup-pkg')
        self.assertEqual(results[1].package, 'scan-pkg')
        self.assertEqual(results[2].package, 'teardown-pkg')

    @patch('pw_fortifier.scanner.WritableGitWorkspace')
    @patch(
        'pw_fortifier.freshness_collector.FreshnessResultCollector._print_item'
    )
    async def test_none_target_scanner(self, mock_print, mock_git_ws_cls):
        """Test scanner with None target only runs pre_scan and post_scan."""
        mock_branch = MagicMock()
        mock_branch.setup = AsyncMock(return_value=mock_branch)
        mock_branch.teardown = AsyncMock()
        mock_branch.add = AsyncMock()
        mock_branch.commit = AsyncMock()
        mock_branch.commit_msg = AsyncMock(
            return_value=['roll: Update 3p deps', '', 'Bug: 123']
        )
        mock_branch.push = AsyncMock(return_value=123456)
        mock_repo = MagicMock()
        mock_repo.project_dir = self.test_dir
        mock_repo.branch = MagicMock(return_value=mock_branch)
        mock_git_ws_cls.return_value = mock_repo
        results = []

        async def log_result(path):
            result = await FreshnessResult.load(path)
            results.append(result)

        mock_print.side_effect = log_result

        freshness_scanner = create_test_freshness_scanner('test_registry')
        scanner = NoneTargetPackageAnalyzer()
        freshness_scanner.register(scanner)

        await freshness_scanner.run(
            '--src-repo', str(self.test_dir), '--dst-repo', str(self.test_dir)
        )

        self.assertTrue(scanner.prescan_called)
        self.assertFalse(scanner.scan_called)
        self.assertTrue(scanner.postscan_called)

        self.assertEqual(len(results), 2)
        self.assertEqual(results[0].package, 'none-pattern-setup')
        self.assertEqual(results[1].package, 'none-pattern-teardown')

    def test_register_with_updater(self) -> None:
        """Tests register with updater registers it with RollGenerator."""
        scanner = create_test_freshness_scanner('test_registry')
        analyzer = FooPackageAnalyzerStub()
        updater = PackageUpdaterStub()
        updater.PKG_TYPE = 'foo_type'
        scanner.register(analyzer, updater)

        assert isinstance(scanner.code_generator, RollGenerator)
        self.assertIn('foo_type', scanner.code_generator._pkg_registry)
        self.assertIs(scanner.code_generator._pkg_registry['foo_type'], updater)

    def test_scanner_errors_flag_writes_to_file(self) -> None:
        """Tests that specifying --errors writes logs to the requested file."""
        log_file = self.test_dir / 'test.log'
        scanner = create_test_freshness_scanner('test_logging')
        args = scanner._parse_args('--errors', str(log_file))
        self.assertEqual(args.errors, str(log_file))

        scanner._setup_logging(args)
        logging.getLogger('test_scanner_logger').warning(
            'Test scanner log entry'
        )

        for handler in logging.getLogger().handlers:
            handler.flush()

        self.assertTrue(log_file.exists())
        content = Path(log_file).read_text()
        self.assertIn('Test scanner log entry', content)

    def test_scanner_errors_flag_short_option(self) -> None:
        """Tests that -E sets the errors file path."""
        scanner = create_test_freshness_scanner('test_logging')
        args = scanner._parse_args('-E', '/path/to/log.txt')
        self.assertEqual(args.errors, '/path/to/log.txt')


class TestFreshnessResultCollector(
    unittest.IsolatedAsyncioTestCase, TestCaseMixin
):
    """Unit tests for FreshnessResultCollector."""

    def setUp(self) -> None:
        self.setUpPyfakefs()
        self.working_dir = AsyncPath('/working').resolve()
        self.fs.create_dir(self.working_dir.path)

    async def test_print_item_fields(self) -> None:
        """Tests that collector formats display versions correctly."""
        collector = FreshnessResultCollector()
        sha1 = 'e457a7b0d326d67b4322ef0d11bd715cfaeda48f'
        sha2 = 'af31bd52e300df040127e17c1823ee160609ed9c'
        result = FreshnessResult(
            package='fuchsia/third_party/rust',
            location=CodeSnippet(file='MODULE.bazel'),
            pkg_type='bazel_cipd',
            current=PackageVersion(
                f'git_revisions:{sha1},{sha2}', date(2026, 1, 1)
            ),
            earliest=PackageVersion(
                f'git_revisions:{sha1},{sha2}', date(2026, 1, 1)
            ),
            tier=1,
            assignee='rust-owner@google.com',
        )
        res_file = self.working_dir / 'result.json'
        await result.save(res_file)

        reported_rows = []

        def mock_report_fields(*args):
            reported_rows.append(args)

        collector._report_fields = mock_report_fields  # type: ignore

        await collector._print_item(res_file)

        self.assertEqual(len(reported_rows), 1)
        row = reported_rows[0]
        # Verify package, current_ver (hidden), current,
        # earliest_ver (hidden), earliest
        self.assertEqual(row[0], 'fuchsia/third_party/rust')
        self.assertEqual(row[6], f'git_revisions:{sha1},{sha2}')
        self.assertEqual(row[7], sha1[:9])
        self.assertEqual(row[9], f'git_revisions:{sha1},{sha2}')
        self.assertEqual(row[10], sha1[:9])


if __name__ == '__main__':
    unittest.main()
