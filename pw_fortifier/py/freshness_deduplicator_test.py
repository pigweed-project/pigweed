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
"""Tests for FreshnessDeduplicator."""

# pylint: disable=protected-access

from datetime import date
import unittest

from pyfakefs.fake_filesystem_unittest import TestCaseMixin
from pw_fortifier.async_path import AsyncPath
from pw_fortifier.code_snippet import CodeSnippet
from pw_fortifier.freshness_deduplicator import FreshnessDeduplicator
from pw_fortifier.freshness_result import FreshnessResult, PackageVersion
from pw_fortifier.issue import Issue
from pw_fortifier.issue_tracker import IssueTrackerStub
from pw_fortifier.scanner import configure_stage_for_test


class TestFreshnessDeduplicator(
    unittest.IsolatedAsyncioTestCase, TestCaseMixin
):
    """Unit tests for FreshnessDeduplicator."""

    def setUp(self) -> None:
        self.setUpPyfakefs()
        self.working_dir = AsyncPath('/working')
        self.fs.create_dir(self.working_dir.path)

    async def test_is_duplicate_with_legacy_git_revision_title(self) -> None:
        """Tests that deduplicator matches legacy git_revision: issues."""
        sha = '702eb9654703a7cec1cadf93a7e3aa269d053943'
        legacy_desc = '\n'.join(
            [
                'pw_fortifier has detected that a third-party dependency that '
                'is on device is stale according to '
                'http://go/pigweed-3p-freshness-policy:',
                '',
                f'fuchsia/sysroot has a current version of git_revision:{sha} '
                'from 2026-01-01.',
                '',
                'The earliest version that is still considered fresh according '
                'to the policy is git_revision:newer123, from 2026-06-01.',
                '',
                "This bazel_cipd dependency's version was determined from "
                'MODULE.bazel.',
                '',
                'Please update this package.',
            ]
        )
        existing_issue = Issue(
            issue_id=55555,
            title=(
                f'Version git_revision:{sha[:9]} of fuchsia/sysroot is stale '
                'and needs to be updated'
            ),
            description=legacy_desc,
        )

        tracker = IssueTrackerStub()
        tracker.primary_hotlist_id = 12345
        tracker.add_issue(existing_issue, hotlist_ids=[12345])

        dedup = FreshnessDeduplicator()
        dedup.issue_tracker = tracker

        await configure_stage_for_test(
            dedup,
            working_dir=str(self.working_dir),
        )

        new_finding = FreshnessResult(
            package='fuchsia/sysroot',
            location=CodeSnippet(file='MODULE.bazel'),
            pkg_type='bazel_cipd',
            current=PackageVersion(sha, date(2026, 1, 1)),
            earliest=PackageVersion('newer123', date(2026, 6, 1)),
            tier=0,
        )

        dup_id = await dedup._is_duplicate(new_finding)
        self.assertEqual(dup_id, 55555)

    async def test_is_duplicate_non_duplicate(self) -> None:
        """Tests that different packages are not flagged as duplicates."""
        existing_issue = Issue(
            issue_id=55555,
            title='Version 1.0.0 of other-pkg is stale and needs to be updated',
            description=(
                'pw_fortifier has detected that a third-party dependency that '
                'is on device is stale according to '
                'http://go/pigweed-3p-freshness-policy:\n\n'
                'other-pkg has a current version of 1.0.0 from 2026-01-01.\n\n'
                'The earliest version that is still considered fresh according '
                'to the policy is 2.0.0, from 2026-06-01.\n\n'
                "This pip dependency's version was determined from "
                'requirements.txt.'
            ),
        )

        tracker = IssueTrackerStub()
        tracker.primary_hotlist_id = 12345
        tracker.add_issue(existing_issue, hotlist_ids=[12345])

        dedup = FreshnessDeduplicator()
        dedup.issue_tracker = tracker

        await configure_stage_for_test(
            dedup,
            working_dir=str(self.working_dir),
            hotlists=[12345],
        )

        new_finding = FreshnessResult(
            package='my-pkg',
            location=CodeSnippet(file='requirements.txt'),
            pkg_type='pip',
            current=PackageVersion('1.0.0', date(2026, 1, 1)),
            earliest=PackageVersion('2.0.0', date(2026, 6, 1)),
            tier=0,
        )

        dup_id = await dedup._is_duplicate(new_finding)
        self.assertIsNone(dup_id)

    async def test_is_duplicate_with_git_revisions_tag(self) -> None:
        """Tests that deduplicator matches git_revisions: multi-hash tags."""
        sha1 = 'e457a7b0d326d67b4322ef0d11bd715cfaeda48f'
        sha2 = 'af31bd52e300df040127e17c1823ee160609ed9c'
        desc = '\n'.join(
            [
                'pw_fortifier has detected that a third-party dependency that '
                'is used to build is stale according to '
                'http://go/pigweed-3p-freshness-policy:',
                '',
                'fuchsia/third_party/rust has a current version of '
                f'git_revisions:{sha1},{sha2} from 2026-01-01.',
                '',
                'The earliest version that is still considered fresh according '
                f'to the policy is git_revisions:{sha1},{sha2}, '
                'from 2026-06-01.',
                '',
                "This bazel_cipd dependency's version was determined from "
                'MODULE.bazel.',
                '',
                'Please update this package.',
            ]
        )
        existing_issue = Issue(
            issue_id=77777,
            title=(
                f'Version {sha1[:9]} of fuchsia/third_party/rust is stale '
                'and needs to be updated'
            ),
            description=desc,
        )

        tracker = IssueTrackerStub()
        tracker.primary_hotlist_id = 12345
        tracker.add_issue(existing_issue, hotlist_ids=[12345])

        dedup = FreshnessDeduplicator()
        dedup.issue_tracker = tracker

        await configure_stage_for_test(
            dedup,
            working_dir=str(self.working_dir),
            hotlists=[12345],
        )

        new_finding = FreshnessResult(
            package='fuchsia/third_party/rust',
            location=CodeSnippet(file='MODULE.bazel'),
            pkg_type='bazel_cipd',
            current=PackageVersion(
                f'git_revisions:{sha1},{sha2}', date(2026, 1, 1)
            ),
            earliest=PackageVersion(
                f'git_revisions:{sha1},{sha2}', date(2026, 6, 1)
            ),
            tier=1,
        )

        dup_id = await dedup._is_duplicate(new_finding)
        self.assertEqual(dup_id, 77777)


if __name__ == '__main__':
    unittest.main()
