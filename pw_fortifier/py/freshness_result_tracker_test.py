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
"""Tests for freshness_result_tracker classes."""

# pylint: disable=protected-access

import asyncio
from datetime import date
import unittest

from pyfakefs.fake_filesystem_unittest import TestCaseMixin
from pw_fortifier.async_path import AsyncPath
from pw_fortifier.code_snippet import CodeSnippet
from pw_fortifier.freshness_result import FreshnessResult, PackageVersion
from pw_fortifier.freshness_result_tracker import (
    FreshnessResultReader,
    FreshnessResultWriter,
)
from pw_fortifier.issue import Issue
from pw_fortifier.issue_tracker import IssueTrackerStub
from pw_fortifier.pipeline_stage import PipelineSink
from pw_fortifier.scanner import configure_stage_for_test


class TestFreshnessResultReader(
    unittest.IsolatedAsyncioTestCase, TestCaseMixin
):
    """Unit tests for FreshnessResultReader."""

    def setUp(self) -> None:
        self.setUpPyfakefs()
        self.working_dir = AsyncPath('/working')
        self.fs.create_dir(self.working_dir.path)

    def test_convert_valid_issue(self) -> None:
        """Tests converting a base Issue to a FreshnessResult."""
        description = '\n'.join(
            [
                'pw_fortifier has detected that a third-party dependency that '
                'is on device is stale according to '
                'http://go/pigweed-3p-freshness-policy:',
                '',
                'test-package has a current version of 1.0.0 from 2026-01-01.',
                '',
                'The earliest version that is still considered fresh according '
                'to the policy is 2.0.0, from 2026-06-01.',
                '',
                "This bazel_dep dependency's version was determined from "
                'MODULE.bazel.',
                '',
                'Please update this package or file for an exception in '
                'accordance with the freshness policy.',
            ]
        )
        issue = Issue(
            issue_id=123,
            title=(
                'Version 1.0.0 of test-package is stale and needs to be updated'
            ),
            description=description,
            priority=1,
            severity=2,
            assignee='dev@google.com',
            cl_num=999,
        )
        converted = FreshnessResult.from_issue(issue)
        assert converted is not None
        assert isinstance(converted, FreshnessResult)
        self.assertEqual(converted.issue_id, 123)
        self.assertEqual(converted.package, 'test-package')
        self.assertEqual(converted.source, 'MODULE.bazel')
        self.assertEqual(converted.pkg_type, 'bazel_dep')
        self.assertEqual(converted.tier, 0)
        self.assertEqual(converted.current.version, '1.0.0')
        self.assertEqual(converted.current.timestamp, date(2026, 1, 1))
        self.assertEqual(converted.earliest.version, '2.0.0')
        self.assertEqual(converted.earliest.timestamp, date(2026, 6, 1))
        self.assertEqual(converted.priority, 1)
        self.assertEqual(converted.severity, 2)
        self.assertEqual(converted.assignee, 'dev@google.com')
        self.assertEqual(converted.cl_num, 999)

    def test_convert_with_line_numbers(self) -> None:
        """Tests converting an issue where source has line numbers."""
        description = '\n'.join(
            [
                'pw_fortifier has detected that a third-party dependency that '
                'is on device is stale according to '
                'http://go/pigweed-3p-freshness-policy:',
                '',
                'test-package has a current version of 1.0.0 from 2026-01-01.',
                '',
                'The earliest version that is still considered fresh according '
                'to the policy is 2.0.0, from 2026-06-01.',
                '',
                "This bazel_dep dependency's version was determined from "
                'MODULE.bazel:12-25.',
                '',
                'Please update this package.',
            ]
        )
        issue = Issue(
            issue_id=123,
            title=(
                'Version 1.0.0 of test-package is stale and needs to be '
                'updated'
            ),
            description=description,
        )
        converted = FreshnessResult.from_issue(issue)
        assert converted is not None
        self.assertEqual(
            converted.location,
            CodeSnippet(file='MODULE.bazel', lines=(12, 25)),
        )
        self.assertEqual(converted.source, 'MODULE.bazel')

    def test_convert_with_full_sha_title(self) -> None:
        """Tests converting issue with full 40-character SHA in title."""
        full_sha = '0123456789abcdef0123456789abcdef01234567'
        description = '\n'.join(
            [
                'pw_fortifier has detected that a third-party dependency that '
                'is on device is stale according to '
                'http://go/pigweed-3p-freshness-policy:',
                '',
                f'my-git-repo has a current version of {full_sha} from '
                '2026-01-01.',
                '',
                'The earliest version that is still considered fresh '
                'according to the policy is '
                'fedcba9876543210fedcba9876543210fedcba98, from 2026-06-01.',
                '',
                "This copybara dependency's version was determined from "
                'copy.bara.sky.',
                '',
                'Please update this package.',
            ]
        )
        issue = Issue(
            issue_id=123,
            title=(
                f'Version {full_sha} of my-git-repo is stale and needs to be '
                'updated'
            ),
            description=description,
        )
        converted = FreshnessResult.from_issue(issue)
        assert converted is not None
        self.assertEqual(converted.package, 'my-git-repo')
        self.assertEqual(converted.current.version, full_sha)
        self.assertEqual(converted.pkg_type, 'copybara')

    def test_convert_with_short_sha_title(self) -> None:
        """Tests converting issue with truncated 9-character SHA in title."""
        full_sha = '0123456789abcdef0123456789abcdef01234567'
        short_sha = full_sha[:9]
        description = '\n'.join(
            [
                'pw_fortifier has detected that a third-party dependency that '
                'is on device is stale according to '
                'http://go/pigweed-3p-freshness-policy:',
                '',
                f'my-git-repo has a current version of {full_sha} from '
                '2026-01-01.',
                '',
                'The earliest version that is still considered fresh '
                'according to the policy is '
                'fedcba9876543210fedcba9876543210fedcba98, from 2026-06-01.',
                '',
                "This copybara dependency's version was determined from "
                'copy.bara.sky.',
                '',
                'Please update this package.',
            ]
        )
        issue = Issue(
            issue_id=123,
            title=(
                f'Version {short_sha} of my-git-repo is stale and needs to be '
                'updated'
            ),
            description=description,
        )
        converted = FreshnessResult.from_issue(issue)
        assert converted is not None
        self.assertEqual(converted.package, 'my-git-repo')
        self.assertEqual(converted.current.version, full_sha)
        self.assertEqual(converted.pkg_type, 'copybara')

    def test_convert_invalid_title(self) -> None:
        """Tests that issues with non-matching titles return None."""
        issue = Issue(
            issue_id=123,
            title='Some other title',
        )
        self.assertIsNone(FreshnessResult.from_issue(issue))

    def test_convert_missing_policy_marker(self) -> None:
        """Tests that issues without policy marker return None."""
        issue = Issue(
            issue_id=123,
            title='Version 1.0.0 of pkg is stale and needs to be updated',
            description='Some random description without policy URL.',
        )
        self.assertIsNone(FreshnessResult.from_issue(issue))

    def test_convert_missing_issue_id(self) -> None:
        """Tests that issues without issue_id return None."""
        description = (
            'pw_fortifier has detected that a third-party dependency that '
            'is on device is stale according to '
            'http://go/pigweed-3p-freshness-policy:'
        )
        issue = Issue(
            issue_id=None,
            title='Version 1.0.0 of pkg is stale and needs to be updated',
            description=description,
        )
        self.assertIsNone(FreshnessResult.from_issue(issue))

    async def test_run_reads_and_forwards_results(self) -> None:
        """Tests running FreshnessResultReader end-to-end."""
        description = '\n'.join(
            [
                'pw_fortifier has detected that a third-party dependency that '
                'is dev tooling is stale according to '
                'http://go/pigweed-3p-freshness-policy:',
                '',
                'npm-pkg has a current version of 1.0.0 from 2026-01-01.',
                '',
                'The earliest version that is still considered fresh according '
                'to the policy is 2.0.0, from 2026-06-01.',
                '',
                "This npm dependency's version was determined from "
                'package.json.',
                '',
                'Please update this package.',
            ]
        )
        tracker_stub = IssueTrackerStub()
        tracker_stub.add_issue(
            Issue(
                issue_id=100,
                title=(
                    'Version 1.0.0 of npm-pkg is stale and needs to be updated'
                ),
                description=description,
            )
        )

        reader = FreshnessResultReader(tracker_stub)
        next_stage = PipelineSink()
        reader.connect(next_stage)

        await configure_stage_for_test(
            reader,
            working_dir=str(self.working_dir),
            issues=[100],
        )

        await reader.run()

        results = []
        while not next_stage.input_queue.empty():
            path = await next_stage.input_queue.get()
            if path is None:
                break
            result = await FreshnessResult.load(path)
            results.append((result.issue_id, result.package, result.pkg_type))

        self.assertEqual(results, [(100, 'npm-pkg', 'npm')])


class TestFreshnessResultWriter(
    unittest.IsolatedAsyncioTestCase, TestCaseMixin
):
    """Unit tests for FreshnessResultWriter."""

    def setUp(self) -> None:
        self.setUpPyfakefs()
        self.working_dir = AsyncPath('/working')
        self.fs.create_dir(self.working_dir.path)

    def test_make_title_and_description(self) -> None:
        """Tests title and description generation methods."""
        result = FreshnessResult(
            package='foo-lib',
            location=CodeSnippet(file='requirements.txt', lines=(15, 20)),
            pkg_type='pip',
            current=PackageVersion('1.2.3', date(2026, 1, 1)),
            earliest=PackageVersion('2.0.0', date(2026, 6, 1)),
            tier=1,
        )
        title = FreshnessResultWriter.make_title(result)
        self.assertEqual(
            title, 'Version 1.2.3 of foo-lib is stale and needs to be updated'
        )

        desc = FreshnessResultWriter.make_description(result)
        self.assertIn(
            'http://go/pigweed-3p-freshness-policy:',
            desc,
        )
        self.assertIn('foo-lib has a current version of 1.2.3', desc)
        self.assertIn(
            "This pip dependency's version was determined from "
            'requirements.txt:15-20.',
            desc,
        )

    def test_make_title_git_revision_truncated(self) -> None:
        """Tests 40-character SHA1 versions are truncated to 9 in title."""
        full_sha = '0123456789abcdef0123456789abcdef01234567'
        result = FreshnessResult(
            package='my-git-repo',
            location=CodeSnippet(file='copy.bara.sky'),
            pkg_type='copybara',
            current=PackageVersion(
                f'git_revision:{full_sha}', date(2026, 1, 1)
            ),
            earliest=PackageVersion(
                'git_revision:fedcba9876543210fedcba9876543210fedcba98',
                date(2026, 6, 1),
            ),
            tier=0,
        )
        title = FreshnessResultWriter.make_title(result)
        self.assertEqual(
            title,
            f'Version {full_sha[:9]} of my-git-repo is stale and needs to be '
            'updated',
        )

    def test_make_title_git_revisions_plural(self) -> None:
        """Tests multi-revision git_revisions tag uses first SHA truncated."""
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
        )
        title = FreshnessResultWriter.make_title(result)
        self.assertEqual(
            title,
            f'Version {sha1[:9]} of fuchsia/third_party/rust is stale '
            'and needs to be updated',
        )

    async def test_process_one_dry_run(self) -> None:
        """Tests FreshnessResultWriter processing in dry-run mode."""
        tracker_stub = IssueTrackerStub()
        writer = FreshnessResultWriter(tracker_stub)
        next_stage = PipelineSink()
        writer.connect(next_stage)

        await configure_stage_for_test(
            writer,
            working_dir=str(self.working_dir),
            create_bugs=False,
        )

        result = FreshnessResult(
            package='foo-lib',
            location=CodeSnippet(file='requirements.txt'),
            pkg_type='pip',
            current=PackageVersion('1.2.3', date(2026, 1, 1)),
            earliest=PackageVersion('2.0.0', date(2026, 6, 1)),
            tier=1,
        )
        result_file = AsyncPath(self.working_dir, 'freshness.json')
        await result.save(result_file)

        run_task = asyncio.create_task(writer.run())
        await writer.input_queue.put(result_file)
        await writer.input_queue.put(None)
        await run_task

        forwarded_path = await next_stage.input_queue.get()
        assert forwarded_path is not None
        updated_result = await FreshnessResult.load(forwarded_path)
        self.assertEqual(
            updated_result.title,
            'Version 1.2.3 of foo-lib is stale and needs to be updated',
        )

    async def test_process_one_creates_issue(self) -> None:
        """Tests FreshnessResultWriter filing a new issue."""
        tracker_stub = IssueTrackerStub()
        writer = FreshnessResultWriter(tracker_stub)
        next_stage = PipelineSink()
        writer.connect(next_stage)

        await configure_stage_for_test(
            writer,
            working_dir=str(self.working_dir),
            create_bugs=True,
        )

        result = FreshnessResult(
            package='foo-lib',
            location=CodeSnippet(file='requirements.txt'),
            pkg_type='pip',
            current=PackageVersion('1.2.3', date(2026, 1, 1)),
            earliest=PackageVersion('2.0.0', date(2026, 6, 1)),
            tier=1,
        )
        result_file = AsyncPath(self.working_dir, 'freshness.json')
        await result.save(result_file)

        run_task = asyncio.create_task(writer.run())
        await writer.input_queue.put(result_file)
        await writer.input_queue.put(None)
        await run_task

        forwarded_path = await next_stage.input_queue.get()
        assert forwarded_path is not None
        updated_result = await FreshnessResult.load(forwarded_path)
        self.assertEqual(updated_result.issue_id, 8675309)
        self.assertEqual(
            updated_result.title,
            'Version 1.2.3 of foo-lib is stale and needs to be updated',
        )


if __name__ == '__main__':
    unittest.main()
