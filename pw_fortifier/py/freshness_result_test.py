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
"""Tests for the FreshnessResult class."""

from datetime import date
import json
import os
import unittest

from pyfakefs.fake_filesystem_unittest import TestCaseMixin
from pw_fortifier.async_path import AsyncPath
from pw_fortifier.code_snippet import CodeSnippet
from pw_fortifier.freshness_result import (
    FreshnessResult,
    PackageVersion,
    get_display_version,
)
from pw_fortifier.issue import Issue


class TestFreshnessResultSaveLoad(
    unittest.IsolatedAsyncioTestCase, TestCaseMixin
):
    """Tests for FreshnessResult save and load methods."""

    def setUp(self):
        self.setUpPyfakefs()
        self.test_dir = '/test'
        self.fs.create_dir(self.test_dir)

    async def test_save_load(self):
        """Tests saving and loading a FreshnessResult."""
        result = FreshnessResult(
            package='test-pkg',
            location=CodeSnippet(file='MODULE.bazel', lines=(10, 15)),
            pkg_type='bazel_dep',
            current=PackageVersion('1.0.0', date(2026, 6, 1)),
            earliest=PackageVersion('2.0.0', date(2026, 6, 15)),
            tier=1,
            assignee='assignee@google.com',
        )
        self.assertEqual(result.source, 'MODULE.bazel')
        path = AsyncPath(os.path.join(self.test_dir, 'result.json'))
        await result.save(path)

        loaded = await FreshnessResult.load(path)
        self.assertEqual(result, loaded)
        self.assertEqual(
            loaded.location,
            CodeSnippet(file='MODULE.bazel', lines=(10, 15)),
        )

    async def test_save_load_optional_owner_none(self):
        """Tests saving and loading a FreshnessResult without an assignee."""
        result = FreshnessResult(
            package='test-pkg2',
            location=CodeSnippet(file='package.json'),
            pkg_type='npm',
            current=PackageVersion('1.0.0', date(2026, 6, 1)),
            earliest=PackageVersion('1.0.0', date(2026, 6, 1)),
            tier=2,
            assignee=None,
        )
        path = AsyncPath(os.path.join(self.test_dir, 'result2.json'))
        await result.save(path)

        loaded = await FreshnessResult.load(path)
        self.assertEqual(result, loaded)

    async def test_load_backward_compatibility(self):
        """Tests loading FreshnessResult from old JSON format with 'source'."""
        old_json = {
            'package': 'test-pkg',
            'source': 'MODULE.bazel',
            'pkg_type': 'bazel_dep',
            'current': {'version': '1.0.0', 'timestamp': '2026-06-01'},
            'earliest': {'version': '2.0.0', 'timestamp': '2026-06-15'},
            'tier': 1,
        }
        path = AsyncPath(os.path.join(self.test_dir, 'old_result.json'))
        await path.write_text(json.dumps(old_json))

        loaded = await FreshnessResult.load(path)
        self.assertEqual(loaded.source, 'MODULE.bazel')
        self.assertEqual(loaded.location, CodeSnippet(file='MODULE.bazel'))

    def test_from_issue_with_git_revision_prefix(self):
        """Tests from_issue preserves git_revision: prefix from descriptions."""
        description = '\n'.join(
            [
                'pw_fortifier has detected that a third-party dependency that '
                'is on device is stale according to '
                'http://go/pigweed-3p-freshness-policy:',
                '',
                'fuchsia/sysroot has a current version of '
                'git_revision:702eb9654703a7cec1cadf93a7e3aa269d053943 from '
                '2026-01-01.',
                '',
                'The earliest version that is still considered fresh according '
                'to the policy is '
                'git_revision:newer1234703a7cec1cadf93a7e3aa269d053943, from '
                '2026-06-01.',
                '',
                "This bazel_cipd dependency's version was determined from "
                'MODULE.bazel.',
                '',
                'Please update this package.',
            ]
        )
        issue = FreshnessResult(
            issue_id=456,
            title=(
                'Version 702eb9654 of fuchsia/sysroot is stale '
                'and needs to be updated'
            ),
            description=description,
            package='fuchsia/sysroot',
            pkg_type='bazel_cipd',
            current=PackageVersion(
                'git_revision:702eb9654703a7cec1cadf93a7e3aa269d053943',
                date(2026, 1, 1),
            ),
            earliest=PackageVersion(
                'git_revision:newer1234703a7cec1cadf93a7e3aa269d053943',
                date(2026, 6, 1),
            ),
            tier=0,
        )
        converted = FreshnessResult.from_issue(issue)
        assert converted is not None
        self.assertEqual(
            converted.current.version,
            'git_revision:702eb9654703a7cec1cadf93a7e3aa269d053943',
        )
        self.assertEqual(
            converted.earliest.version,
            'git_revision:newer1234703a7cec1cadf93a7e3aa269d053943',
        )

    def test_from_issue_malformed_current_date(self) -> None:
        """Tests issues with invalid current dates are rejected and logged."""
        description = (
            'pw_fortifier has detected that a third-party dependency that '
            'is on device is stale according to '
            'http://go/pigweed-3p-freshness-policy:\n'
            'It has a current version of 1.0.0 from 2026-99-99.\n'
        )
        issue = Issue(
            issue_id=789,
            title='Version 1.0.0 of mypkg is stale and needs to be updated',
            description=description,
        )
        with self.assertLogs(
            'pw_fortifier.freshness_result', level='WARNING'
        ) as cm:
            result = FreshnessResult.from_issue(issue)
            self.assertIsNone(result)
        self.assertTrue(any('invalid current date' in log for log in cm.output))

    def test_from_issue_missing_current_pattern(self) -> None:
        """Tests issues missing current version details are rejected."""
        description = (
            'pw_fortifier has detected that a third-party dependency that '
            'is on device is stale according to '
            'http://go/pigweed-3p-freshness-policy:\n'
        )
        issue = Issue(
            issue_id=789,
            title='Version 1.0.0 of mypkg is stale and needs to be updated',
            description=description,
        )
        with self.assertLogs(
            'pw_fortifier.freshness_result', level='WARNING'
        ) as cm:
            result = FreshnessResult.from_issue(issue)
            self.assertIsNone(result)
        self.assertTrue(
            any('missing current version pattern' in log for log in cm.output)
        )


class TestGetDisplayVersion(unittest.TestCase):
    """Tests for get_display_version helper."""

    def test_git_revision_prefix(self):
        sha = 'e457a7b0d326d67b4322ef0d11bd715cfaeda48f'
        self.assertEqual(
            get_display_version(f'git_revision:{sha}'),
            sha[:9],
        )

    def test_git_revisions_plural(self):
        sha1 = 'e457a7b0d326d67b4322ef0d11bd715cfaeda48f'
        sha2 = 'af31bd52e300df040127e17c1823ee160609ed9c'
        self.assertEqual(
            get_display_version(f'git_revisions:{sha1},{sha2}'),
            sha1[:9],
        )

    def test_version_prefix(self):
        self.assertEqual(get_display_version('version:1.2.3'), '1.2.3')

    def test_g3_revision_prefix(self):
        g3_tag = (
            'g3-revision:fuchsia.infra.coverage.upload_clients_20260827_RC00'
        )
        self.assertEqual(
            get_display_version(g3_tag),
            'fuchsia.infra.coverage.upload_clients_20260827_RC00',
        )

    def test_plain_semver(self):
        self.assertEqual(get_display_version('1.2.3'), '1.2.3')

    def test_plain_sha(self):
        sha = 'e457a7b0d326d67b4322ef0d11bd715cfaeda48f'
        self.assertEqual(get_display_version(sha), sha[:9])


if __name__ == '__main__':
    unittest.main()
