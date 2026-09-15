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
"""Tests for package_analyzer."""
# pylint: disable=protected-access

from datetime import date
import unittest
from unittest.mock import patch

from pw_fortifier.freshness_result import PackageVersion
from pw_fortifier.package_analyzer import (
    TIER0_ON_DEVICE,
    TIER1_TOOLCHAIN,
    PackageAnalyzerStub,
    SemVer,
)


class TestFindLowest(unittest.TestCase):
    """Tests for PackageAnalyzer.find_lowest."""

    def setUp(self):
        """Set up test environment."""
        self.analyzer = PackageAnalyzerStub()
        self._date_patcher = None

    def tearDown(self):
        if self._date_patcher is not None:
            self._date_patcher.stop()

    def set_date(self, d):
        """Sets the mocked package analyzer date."""
        if self._date_patcher is not None:
            self._date_patcher.stop()
        self._date_patcher = patch('pw_fortifier.package_analyzer.DATE', d)
        self._date_patcher.start()

    def test_release_based_tier1(self):
        """Test release-based freshness for tier 1 packages."""
        # Timeline from user:
        # Day 0: 1.0.0 (2026-06-01)
        # Day 10: 1.1.0 (2026-06-11)
        # Day 40: 1.0.0 expires.
        # Day 60: 2.0.0 (2026-08-01)
        # Day 70: 1.2.0 (2026-08-11)
        # Day 100: 1.1.0 expires.
        # Day 110: 2.1.0 (2026-09-20)
        # Day 140: 2.0.0 expires.
        # Day 150: 1.2.0 expires.

        d0 = date(2026, 6, 1)
        d10 = date(2026, 6, 11)
        d40 = date(2026, 7, 11)
        d60 = date(2026, 8, 1)
        d70 = date(2026, 8, 11)
        d100 = date(2026, 9, 10)
        d110 = date(2026, 9, 20)
        d140 = date(2026, 10, 20)
        d150 = date(2026, 10, 30)

        v1_0 = PackageVersion('1.0.0', d0)
        v1_1 = PackageVersion('1.1.0', d10)
        v1_2 = PackageVersion('1.2.0', d70)
        v2_0 = PackageVersion('2.0.0', d60)
        v2_1 = PackageVersion('2.1.0', d110)

        all_versions = [v1_0, v1_1, v1_2, v2_0, v2_1]

        # Day 0: 1.0.0 is fresh (only version)
        self.set_date(d0)
        self.assertEqual(
            self.analyzer.find_lowest(TIER1_TOOLCHAIN, v1_0, [v1_0]), v1_0
        )

        # Day 20: 1.0.0 is still fresh (expires Day 40)
        self.set_date(date(2026, 6, 21))
        self.assertEqual(
            self.analyzer.find_lowest(TIER1_TOOLCHAIN, v1_0, [v1_0, v1_1]), v1_0
        )

        # Day 40: 1.0.0 is stale.
        self.set_date(d40)
        self.assertEqual(
            self.analyzer.find_lowest(TIER1_TOOLCHAIN, v1_0, [v1_0, v1_1]), v1_1
        )

        # Day 60: 2.0.0 released. current 1.1.0 is fresh (expires Day 150).
        self.set_date(d60)
        self.assertEqual(
            self.analyzer.find_lowest(
                TIER1_TOOLCHAIN, v1_1, [v1_0, v1_1, v2_0]
            ),
            v1_1,
        )

        # Day 70: 1.2.0 released. current 1.1.0 is fresh (expires Day 100).
        self.set_date(d70)
        self.assertEqual(
            self.analyzer.find_lowest(TIER1_TOOLCHAIN, v1_1, all_versions), v1_1
        )

        # Day 100: 1.1.0 expires. lowest fresh >= 1.1.0 is 1.2.0.
        self.set_date(d100)
        self.assertEqual(
            self.analyzer.find_lowest(TIER1_TOOLCHAIN, v1_1, all_versions), v1_2
        )

        # Day 110: 2.1.0 released. current 1.2.0 is fresh (expires Day 150).
        self.set_date(d110)
        self.assertEqual(
            self.analyzer.find_lowest(TIER1_TOOLCHAIN, v1_2, all_versions), v1_2
        )

        # Day 140: 2.0.0 expires. lowest fresh >= 2.0.0 is 2.1.0.
        self.set_date(d140)
        self.assertEqual(
            self.analyzer.find_lowest(TIER1_TOOLCHAIN, v2_0, all_versions), v2_1
        )
        # current 1.2.0 is still fresh (expires Day 150)
        self.assertEqual(
            self.analyzer.find_lowest(TIER1_TOOLCHAIN, v1_2, all_versions), v1_2
        )

        # Day 150: 1.2.0 expires. lowest fresh >= 1.2.0 is 2.1.0.
        self.set_date(d150)
        self.assertEqual(
            self.analyzer.find_lowest(TIER1_TOOLCHAIN, v1_2, all_versions), v2_1
        )

    def test_revision_based_tier0(self):
        """Test revision-based freshness for tier 0 packages."""
        d0 = date(2026, 6, 1)
        d10 = date(2026, 6, 11)
        d40 = date(2026, 7, 11)

        v1 = PackageVersion('hash1', d0)
        v2 = PackageVersion('hash2', d10)
        versions = [v1, v2]

        # Day 30: v1 is fresh (expires Day 40)
        self.set_date(date(2026, 7, 1))
        self.assertEqual(
            self.analyzer.find_lowest(TIER0_ON_DEVICE, v1, versions), v1
        )

        # Day 40: v1 is stale.
        self.set_date(d40)
        self.assertEqual(
            self.analyzer.find_lowest(TIER0_ON_DEVICE, v1, versions), v2
        )

    def test_tie_breaking_same_semver_different_suffix(self):
        """Test tie-breaking for same semver with different suffix."""
        # Today is 2026-06-10
        self.set_date(date(2026, 6, 10))

        # Case 1: Newer suffix is fresh (< 30 days old), so older suffix
        # is also fresh.
        d_old = date(2026, 6, 1)
        d_new = date(2026, 6, 5)
        v_android = PackageVersion('1.0.9-android', d_old)
        v_jre = PackageVersion('1.0.9-jre', d_new)
        versions = [v_android, v_jre]

        self.assertEqual(
            self.analyzer.find_lowest(TIER1_TOOLCHAIN, v_android, versions),
            v_android,
        )

        # Case 2: Newer suffix is old (>= 30 days old), making older
        # suffix stale.
        d_old = date(2026, 4, 1)
        d_new = date(2026, 5, 1)
        v_android = PackageVersion('1.0.9-android', d_old)
        v_jre = PackageVersion('1.0.9-jre', d_new)
        versions = [v_android, v_jre]

        self.assertEqual(
            self.analyzer.find_lowest(TIER1_TOOLCHAIN, v_android, versions),
            v_jre,
        )

    def test_semver_comparison_numeric_patch(self):
        """Test semver comparison with numeric patch version."""
        # Today is 2026-06-10
        self.set_date(date(2026, 6, 10))

        # 1.0.9 is older, 1.0.27 is newer and makes 1.0.9 stale
        d_old = date(2026, 2, 1)
        d_new = date(2026, 5, 1)
        v_9 = PackageVersion('1.0.9', d_old)
        v_27 = PackageVersion('1.0.27', d_new)
        versions = [v_9, v_27]

        self.assertEqual(
            self.analyzer.find_lowest(TIER1_TOOLCHAIN, v_9, versions), v_27
        )

    def test_invalid_current_version_with_releases_raises_error(self):
        """Test find_lowest with invalid current version."""
        d0 = date(2026, 6, 1)
        v1_0 = PackageVersion('1.0.0', d0)
        current = PackageVersion('invalid_version', d0)
        with self.assertRaises(ValueError):
            self.analyzer.find_lowest(TIER1_TOOLCHAIN, current, [v1_0])

    def test_find_lowest_ignores_prereleases(self):
        """Test find_lowest ignores pre-releases when loose_semver is False."""
        self.analyzer.loose_semver = False
        d0 = date(2026, 6, 1)
        d10 = date(2026, 6, 11)
        v_pre = PackageVersion('1.0.0-alpha', d0)
        v_rel = PackageVersion('1.0.0', d10)
        self.assertEqual(
            self.analyzer.find_lowest(TIER1_TOOLCHAIN, v_pre, [v_pre, v_rel]),
            v_rel,
        )

    def test_find_lowest_all_prereleases(self):
        """Test find_lowest includes pre-releases if all are pre-releases."""
        self.analyzer.loose_semver = False
        d0 = date(2026, 6, 1)
        d10 = date(2026, 6, 11)
        v_pre1 = PackageVersion('4.0.0-alpha.60', d0)
        v_pre2 = PackageVersion('4.0.0-alpha.61', d10)
        self.set_date(date(2026, 6, 15))
        self.assertEqual(
            self.analyzer.find_lowest(
                TIER1_TOOLCHAIN, v_pre1, [v_pre1, v_pre2]
            ),
            v_pre1,
        )


class TestSemverHelpers(unittest.TestCase):
    """Tests for semver helper functions."""

    def setUp(self):
        self.analyzer = PackageAnalyzerStub()

    def test_parse_semver(self):
        """Test parse_semver helper."""
        self.assertEqual(self.analyzer.parse_semver('1.2.3'), SemVer(1, 2, 3))
        self.assertEqual(
            self.analyzer.parse_semver('10.5.1.bcr.4'),
            SemVer(10, 5, 1, extra='.bcr.4'),
        )
        self.assertIsNone(self.analyzer.parse_semver('invalid'))

    def test_loose_semver(self):
        """Test loose_semver behavior on PackageAnalyzer."""
        self.analyzer.loose_semver = True
        self.assertEqual(
            self.analyzer.parse_semver('31.1-jre'),
            SemVer(31, 1, 0, extra='-jre'),
        )
        self.analyzer.loose_semver = False
        self.assertEqual(
            self.analyzer.parse_semver('31.1-jre'),
            SemVer(31, 1, 0, prerelease='jre'),
        )

    def test_clean_version(self):
        """Test clean_version helper."""
        self.assertEqual(self.analyzer.clean_version('2@13.3.rel1.1'), '13.3.0')
        self.assertEqual(self.analyzer.clean_version('31.1-jre'), '31.1.0')
        self.assertEqual(self.analyzer.clean_version('1.2.3'), '1.2.3')
        self.assertEqual(self.analyzer.clean_version('invalid'), 'invalid')

    def test_major_version(self):
        """Test major_version helper."""
        self.assertEqual(self.analyzer.major_version('2@13.3.rel1.1'), '13')
        self.assertEqual(self.analyzer.major_version('31.1-jre'), '31')
        self.assertEqual(self.analyzer.major_version('1.2.3'), '1')
        self.assertIsNone(self.analyzer.major_version('invalid'))


if __name__ == '__main__':
    unittest.main()
