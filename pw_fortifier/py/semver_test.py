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
"""Tests for semver module."""

import unittest

from pw_fortifier.semver import SemVer, parse_semver


class TestSemVer(unittest.TestCase):
    """Tests for SemVer named tuple and parse_semver."""

    def test_parse_standard_semver(self):
        """Test parsing standard semantic versions."""
        self.assertEqual(
            parse_semver('1.2.3'),
            SemVer(1, 2, 3),
        )
        self.assertEqual(
            parse_semver('1.2'),
            SemVer(1, 2, 0),
        )
        self.assertEqual(
            parse_semver('1'),
            SemVer(1, 0, 0),
        )
        self.assertEqual(
            parse_semver('v1.2.3'),
            SemVer(1, 2, 3),
        )
        self.assertEqual(
            parse_semver('^1.2.3'),
            SemVer(1, 2, 3),
        )
        self.assertEqual(
            parse_semver('~1.2.3'),
            SemVer(1, 2, 3),
        )

    def test_parse_prerelease_and_build(self):
        """Test parsing pre-release and build metadata."""
        self.assertEqual(
            parse_semver('1.2.3-alpha.1'),
            SemVer(1, 2, 3, prerelease='alpha.1'),
        )
        self.assertEqual(
            parse_semver('1.2.3+build.1'),
            SemVer(1, 2, 3, build='build.1'),
        )
        self.assertEqual(
            parse_semver('1.2.3-alpha.1+build.1'),
            SemVer(1, 2, 3, prerelease='alpha.1', build='build.1'),
        )
        self.assertEqual(
            parse_semver('31.1-jre'),
            SemVer(31, 1, 0, prerelease='jre'),
        )

    def test_parse_extra_labels(self):
        """Test parsing dot-separated extra labels."""
        self.assertEqual(
            parse_semver('10.5.1.bcr.4'),
            SemVer(10, 5, 1, extra='.bcr.4'),
        )
        self.assertEqual(
            parse_semver('11.1.0.bcr.3'),
            SemVer(11, 1, 0, extra='.bcr.3'),
        )
        self.assertEqual(
            parse_semver('0.4.9.1.bcr.3'),
            SemVer(0, 4, 9, extra='.1.bcr.3'),
        )
        self.assertEqual(
            parse_semver('2@13.3.rel1.1'),
            SemVer(13, 3, 0, extra='.rel1.1'),
        )
        self.assertEqual(
            parse_semver('3@3.31.12.chromium.8'),
            SemVer(3, 31, 12, extra='.chromium.8'),
        )

    def test_loose_semver(self):
        """Test loose_semver treats pre-release labels as extra labels."""
        self.assertEqual(
            parse_semver('1.2.3-alpha.1', loose_semver=True),
            SemVer(1, 2, 3, extra='-alpha.1'),
        )
        self.assertEqual(
            parse_semver('31.1-jre', loose_semver=True),
            SemVer(31, 1, 0, extra='-jre'),
        )
        self.assertEqual(
            parse_semver('31.1.0-jre', loose_semver=True),
            SemVer(31, 1, 0, extra='-jre'),
        )
        self.assertEqual(
            parse_semver('0.1.0-rc2.bcr.1', loose_semver=True),
            SemVer(0, 1, 0, extra='-rc2.bcr.1'),
        )
        self.assertEqual(
            parse_semver('2.3.4-1', loose_semver=True),
            SemVer(2, 3, 4, extra='-1'),
        )
        self.assertEqual(
            parse_semver('1.2.3-alpha.1+build.1', loose_semver=True),
            SemVer(1, 2, 3, extra='-alpha.1', build='build.1'),
        )

    def test_invalid_versions(self):
        """Test that invalid version strings return None."""
        self.assertIsNone(parse_semver('invalid'))
        self.assertIsNone(
            parse_semver('094063176456395e02ab2108f858ff0eb46487ce')
        )

    def test_str(self):
        """Test SemVer string conversion."""
        self.assertEqual(str(SemVer(1, 2, 3)), '1.2.3')
        self.assertEqual(str(SemVer(1, 2, 0)), '1.2.0')
        self.assertEqual(str(SemVer(1, 0, 0)), '1.0.0')
        self.assertEqual(
            str(SemVer(1, 2, 3, prerelease='alpha.1')),
            '1.2.3-alpha.1',
        )
        self.assertEqual(
            str(SemVer(1, 2, 3, build='build.1')),
            '1.2.3+build.1',
        )
        self.assertEqual(
            str(SemVer(1, 2, 3, prerelease='alpha.1', build='build.1')),
            '1.2.3-alpha.1+build.1',
        )
        self.assertEqual(
            str(SemVer(10, 5, 1, extra='.bcr.4')),
            '10.5.1.bcr.4',
        )
        self.assertEqual(
            str(SemVer(2, 3, 4, extra='-1')),
            '2.3.4-1',
        )
        for v in (
            '1.2.3',
            '1.2.3-alpha.1',
            '1.2.3+build.1',
            '1.2.3-alpha.1+build.1',
            '10.5.1.bcr.4',
        ):
            self.assertEqual(str(parse_semver(v)), v)

        self.assertEqual(
            str(parse_semver('2.3.4-1', loose_semver=True)),
            '2.3.4-1',
        )
        self.assertEqual(
            str(parse_semver('0.1.0-rc2.bcr.1', loose_semver=True)),
            '0.1.0-rc2.bcr.1',
        )

    def test_comparisons(self):
        """Test SemVer rich comparisons."""
        # Core version comparison
        self.assertLess(SemVer(1, 0, 0), SemVer(2, 0, 0))
        self.assertLess(SemVer(1, 1, 0), SemVer(1, 2, 0))
        self.assertLess(SemVer(1, 0, 9), SemVer(1, 0, 27))

        # Extra label lexicographical comparison
        self.assertLess(
            SemVer(1, 0, 0, extra='.bcr.1'),
            SemVer(1, 0, 0, extra='.bcr.2'),
        )
        self.assertLess(
            SemVer(2, 3, 4),
            SemVer(2, 3, 4, extra='-1'),
        )
        self.assertLess(
            SemVer(1, 0, 0),
            SemVer(1, 0, 0, extra='.bcr.1'),
        )

        # Pre-release comparison
        self.assertLess(
            SemVer(1, 0, 0, prerelease='alpha'),
            SemVer(1, 0, 0),
        )
        self.assertLess(
            SemVer(1, 0, 0, prerelease='alpha.1'),
            SemVer(1, 0, 0, prerelease='alpha.2'),
        )

        # Equality and hashing
        v1 = SemVer(1, 2, 3, extra='.bcr.1')
        v2 = SemVer(1, 2, 3, extra='.bcr.1')
        self.assertEqual(v1, v2)
        self.assertEqual(hash(v1), hash(v2))
        self.assertIn(v1, {v2})


if __name__ == '__main__':
    unittest.main()
