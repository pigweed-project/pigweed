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
"""Tests for package_scanner."""
# pylint: disable=protected-access

from datetime import date
import os
import shutil
import tempfile
from typing import Iterator
import unittest

from pw_fortifier.package_scanner import (
    FreshnessScanResult,
    PackageScanner,
    PackageScannerRegistry,
    PackageVersion,
    TIER0_ON_DEVICE,
    TIER1_TOOLCHAIN,
)


class FakePackageScanner(PackageScanner):
    """Fake package scanner for testing registry."""

    def __init__(self, name: str, target_str: str):
        """Initialize fake scanner."""
        super().__init__()
        self.name = name
        self.target_str = target_str
        self.scanned_paths: list[str] = []

    def target(self) -> str:
        """Return target."""
        return self.target_str

    def scan(
        self, pathname: str | os.PathLike[str]
    ) -> Iterator[FreshnessScanResult]:
        """Scan file."""
        abs_path = os.path.normpath(os.path.join(self.root, pathname))
        self.scanned_paths.append(abs_path)
        yield FreshnessScanResult(
            package=self.name,
            source=os.path.basename(pathname),
            pkg_type="fake",
            current=PackageVersion("1.0", date(2026, 1, 1)),
            earliest=PackageVersion("1.0", date(2026, 1, 1)),
            tier=1,
            owner="me",
        )


class LifecyclePackageScanner(PackageScanner):
    """Package scanner to test pre_scan and post_scan lifecycle."""

    def __init__(self):
        super().__init__()
        self.prescan_called = False
        self.postscan_called = False
        self.scan_called = False

    def target(self) -> str:
        return "foo.json"

    def pre_scan(self) -> Iterator[FreshnessScanResult]:
        self.prescan_called = True
        yield FreshnessScanResult(
            package="setup-pkg",
            source="setup",
            pkg_type="lifecycle",
            current=PackageVersion("1.0", date(2026, 1, 1)),
            earliest=PackageVersion("1.0", date(2026, 1, 1)),
            tier=1,
            owner="me",
        )

    def scan(
        self, pathname: str | os.PathLike[str]
    ) -> Iterator[FreshnessScanResult]:
        self.scan_called = True
        yield FreshnessScanResult(
            package="scan-pkg",
            source=os.path.basename(pathname),
            pkg_type="lifecycle",
            current=PackageVersion("1.0", date(2026, 1, 1)),
            earliest=PackageVersion("1.0", date(2026, 1, 1)),
            tier=1,
            owner="me",
        )

    def post_scan(self) -> Iterator[FreshnessScanResult]:
        self.postscan_called = True
        yield FreshnessScanResult(
            package="teardown-pkg",
            source="teardown",
            pkg_type="lifecycle",
            current=PackageVersion("1.0", date(2026, 1, 1)),
            earliest=PackageVersion("1.0", date(2026, 1, 1)),
            tier=1,
            owner="me",
        )


class NoneTargetPackageScanner(PackageScanner):
    """Scanner with None target, only runs pre_scan and post_scan."""

    def __init__(self):
        super().__init__()
        self.prescan_called = False
        self.postscan_called = False
        self.scan_called = False

    def pre_scan(self) -> Iterator[FreshnessScanResult]:
        self.prescan_called = True
        yield FreshnessScanResult(
            package="none-pattern-setup",
            source="setup",
            pkg_type="lifecycle",
            current=PackageVersion("1.0", date(2026, 1, 1)),
            earliest=PackageVersion("1.0", date(2026, 1, 1)),
            tier=1,
            owner="me",
        )

    def scan(
        self, pathname: str | os.PathLike[str]
    ) -> Iterator[FreshnessScanResult]:
        self.scan_called = True
        yield from ()

    def post_scan(self) -> Iterator[FreshnessScanResult]:
        self.postscan_called = True
        yield FreshnessScanResult(
            package="none-pattern-teardown",
            source="teardown",
            pkg_type="lifecycle",
            current=PackageVersion("1.0", date(2026, 1, 1)),
            earliest=PackageVersion("1.0", date(2026, 1, 1)),
            tier=1,
            owner="me",
        )


class TestPackageScannerRegistry(unittest.TestCase):
    """Tests for PackageScannerRegistry."""

    def setUp(self):
        """Set up test environment."""
        self.test_dir = tempfile.mkdtemp()

        # Create structure:
        # temp_dir/
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

        os.makedirs(os.path.join(self.test_dir, "out"))
        os.makedirs(os.path.join(self.test_dir, "nested", "pruned"))
        os.makedirs(os.path.join(self.test_dir, "nested", "other"))

        self.write_file(os.path.join(self.test_dir, "foo.json"), "{}")
        self.write_file(os.path.join(self.test_dir, "bar.py"), "# python")
        self.write_file(os.path.join(self.test_dir, "out", "baz.json"), "{}")
        self.write_file(os.path.join(self.test_dir, "nested", "qux.json"), "{}")
        self.write_file(
            os.path.join(self.test_dir, "nested", "pruned", "ignored.json"),
            "{}",
        )
        self.write_file(
            os.path.join(self.test_dir, "nested", "other", "deep.json"), "{}"
        )

    def tearDown(self):
        """Tear down test environment."""
        shutil.rmtree(self.test_dir)

    @staticmethod
    def write_file(path, content):
        """Helper to write file."""
        with open(path, "w") as f:
            f.write(content)

    def test_default_no_prune(self):
        """Test registry has no pruned paths by default."""
        registry = PackageScannerRegistry(self.test_dir)
        scanners = [
            FakePackageScanner("foo", "foo.json"),
            FakePackageScanner("baz", "baz.json"),
            FakePackageScanner("qux", "qux.json"),
            FakePackageScanner("ignored", "ignored.json"),
            FakePackageScanner("deep", "deep.json"),
        ]
        for s in scanners:
            registry.register(s)

        results = list(registry.scan_all())

        # Should find everything including out/baz.json
        expected_paths = {
            os.path.normpath(os.path.join(self.test_dir, "foo.json")),
            os.path.normpath(os.path.join(self.test_dir, "out", "baz.json")),
            os.path.normpath(os.path.join(self.test_dir, "nested", "qux.json")),
            os.path.normpath(
                os.path.join(self.test_dir, "nested", "pruned", "ignored.json")
            ),
            os.path.normpath(
                os.path.join(self.test_dir, "nested", "other", "deep.json")
            ),
        }
        scanned_paths = set()
        for s in scanners:
            scanned_paths.update(s.scanned_paths)
        self.assertEqual(scanned_paths, expected_paths)
        self.assertEqual(len(results), 5)

    def test_custom_prune(self):
        """Test registry with custom pruned path."""
        registry = PackageScannerRegistry(self.test_dir)
        registry.prune("nested/pruned")
        scanners = [
            FakePackageScanner("foo", "foo.json"),
            FakePackageScanner("qux", "qux.json"),
            FakePackageScanner("ignored", "ignored.json"),
            FakePackageScanner("deep", "deep.json"),
        ]
        for s in scanners:
            registry.register(s)

        _results = list(registry.scan_all())

        # Should find foo.json, nested/qux.json, nested/other/deep.json
        # Should NOT find nested/pruned/ignored.json because it was pruned.
        expected_paths = {
            os.path.normpath(os.path.join(self.test_dir, "foo.json")),
            os.path.normpath(os.path.join(self.test_dir, "nested", "qux.json")),
            os.path.normpath(
                os.path.join(self.test_dir, "nested", "other", "deep.json")
            ),
        }
        scanned_paths = set()
        for s in scanners:
            scanned_paths.update(s.scanned_paths)
        self.assertEqual(scanned_paths, expected_paths)

    def test_absolute_path_prune(self):
        """Test pruning using absolute path."""
        registry = PackageScannerRegistry(self.test_dir)
        # Prune using absolute path
        registry.prune(os.path.join(self.test_dir, "nested", "pruned"))
        scanners = [
            FakePackageScanner("foo", "foo.json"),
            FakePackageScanner("qux", "qux.json"),
            FakePackageScanner("ignored", "ignored.json"),
            FakePackageScanner("deep", "deep.json"),
        ]
        for s in scanners:
            registry.register(s)

        list(registry.scan_all())

        # Should find foo.json, nested/qux.json, nested/other/deep.json
        # Should NOT find nested/pruned/ignored.json because it was pruned.
        expected_paths = {
            os.path.normpath(os.path.join(self.test_dir, "foo.json")),
            os.path.normpath(os.path.join(self.test_dir, "nested", "qux.json")),
            os.path.normpath(
                os.path.join(self.test_dir, "nested", "other", "deep.json")
            ),
        }
        scanned_paths = set()
        for s in scanners:
            scanned_paths.update(s.scanned_paths)
        self.assertEqual(scanned_paths, expected_paths)

    def test_lifecycle_methods(self):
        """Test that pre_scan and post_scan are called and yield results."""
        registry = PackageScannerRegistry(self.test_dir)
        scanner = LifecyclePackageScanner()
        registry.register(scanner)

        results = list(registry.scan_all())

        self.assertTrue(scanner.prescan_called)
        self.assertTrue(scanner.scan_called)
        self.assertTrue(scanner.postscan_called)

        self.assertEqual(len(results), 3)
        self.assertEqual(results[0].package, "setup-pkg")
        self.assertEqual(results[1].package, "scan-pkg")
        self.assertEqual(results[2].package, "teardown-pkg")

    def test_none_target_scanner(self):
        """Test scanner with None target only runs pre_scan and post_scan."""
        registry = PackageScannerRegistry(self.test_dir)
        scanner = NoneTargetPackageScanner()
        registry.register(scanner)

        results = list(registry.scan_all())

        self.assertTrue(scanner.prescan_called)
        self.assertFalse(scanner.scan_called)
        self.assertTrue(scanner.postscan_called)

        self.assertEqual(len(results), 2)
        self.assertEqual(results[0].package, "none-pattern-setup")
        self.assertEqual(results[1].package, "none-pattern-teardown")


class TestFindLowest(unittest.TestCase):
    """Tests for PackageScanner.find_lowest."""

    def setUp(self):
        """Set up test environment."""
        self.scanner = FakePackageScanner('fake', 'fake')

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

        v1_0 = PackageVersion("1.0.0", d0)
        v1_1 = PackageVersion("1.1.0", d10)
        v1_2 = PackageVersion("1.2.0", d70)
        v2_0 = PackageVersion("2.0.0", d60)
        v2_1 = PackageVersion("2.1.0", d110)

        all_versions = [v1_0, v1_1, v1_2, v2_0, v2_1]

        # Day 0: 1.0.0 is fresh (only version)
        self.scanner.date = d0
        self.assertEqual(
            self.scanner.find_lowest(TIER1_TOOLCHAIN, v1_0, [v1_0]), v1_0
        )

        # Day 20: 1.0.0 is still fresh (expires Day 40)
        self.scanner.date = date(2026, 6, 21)
        self.assertEqual(
            self.scanner.find_lowest(TIER1_TOOLCHAIN, v1_0, [v1_0, v1_1]), v1_0
        )

        # Day 40: 1.0.0 is stale.
        self.scanner.date = d40
        self.assertEqual(
            self.scanner.find_lowest(TIER1_TOOLCHAIN, v1_0, [v1_0, v1_1]), v1_1
        )

        # Day 60: 2.0.0 released. current 1.1.0 is fresh (expires Day 150).
        self.scanner.date = d60
        self.assertEqual(
            self.scanner.find_lowest(TIER1_TOOLCHAIN, v1_1, [v1_0, v1_1, v2_0]),
            v1_1,
        )

        # Day 70: 1.2.0 released. current 1.1.0 is fresh (expires Day 100).
        self.scanner.date = d70
        self.assertEqual(
            self.scanner.find_lowest(TIER1_TOOLCHAIN, v1_1, all_versions), v1_1
        )

        # Day 100: 1.1.0 expires. lowest fresh >= 1.1.0 is 1.2.0.
        self.scanner.date = d100
        self.assertEqual(
            self.scanner.find_lowest(TIER1_TOOLCHAIN, v1_1, all_versions), v1_2
        )

        # Day 110: 2.1.0 released. current 1.2.0 is fresh (expires Day 150).
        self.scanner.date = d110
        self.assertEqual(
            self.scanner.find_lowest(TIER1_TOOLCHAIN, v1_2, all_versions), v1_2
        )

        # Day 140: 2.0.0 expires. lowest fresh >= 2.0.0 is 2.1.0.
        self.scanner.date = d140
        self.assertEqual(
            self.scanner.find_lowest(TIER1_TOOLCHAIN, v2_0, all_versions), v2_1
        )
        # current 1.2.0 is still fresh (expires Day 150)
        self.assertEqual(
            self.scanner.find_lowest(TIER1_TOOLCHAIN, v1_2, all_versions), v1_2
        )

        # Day 150: 1.2.0 expires. lowest fresh >= 1.2.0 is 2.1.0.
        self.scanner.date = d150
        self.assertEqual(
            self.scanner.find_lowest(TIER1_TOOLCHAIN, v1_2, all_versions), v2_1
        )

    def test_revision_based_tier0(self):
        """Test revision-based freshness for tier 0 packages."""
        d0 = date(2026, 6, 1)
        d10 = date(2026, 6, 11)
        d40 = date(2026, 7, 11)

        v1 = PackageVersion("hash1", d0)
        v2 = PackageVersion("hash2", d10)
        versions = [v1, v2]

        # Day 30: v1 is fresh (expires Day 40)
        self.scanner.date = date(2026, 7, 1)
        self.assertEqual(
            self.scanner.find_lowest(TIER0_ON_DEVICE, v1, versions), v1
        )

        # Day 40: v1 is stale.
        self.scanner.date = d40
        self.assertEqual(
            self.scanner.find_lowest(TIER0_ON_DEVICE, v1, versions), v2
        )

    def test_tie_breaking_same_semver_different_suffix(self):
        """Test tie-breaking for same semver with different suffix."""
        # Today is 2026-06-10
        self.scanner.date = date(2026, 6, 10)

        # Case 1: Newer suffix is fresh (< 30 days old), so older suffix
        # is also fresh.
        d_old = date(2026, 6, 1)
        d_new = date(2026, 6, 5)
        v_android = PackageVersion("1.0.9-android", d_old)
        v_jre = PackageVersion("1.0.9-jre", d_new)
        versions = [v_android, v_jre]

        self.assertEqual(
            self.scanner.find_lowest(TIER1_TOOLCHAIN, v_android, versions),
            v_android,
        )

        # Case 2: Newer suffix is old (>= 30 days old), making older
        # suffix stale.
        d_old = date(2026, 4, 1)
        d_new = date(2026, 5, 1)
        v_android = PackageVersion("1.0.9-android", d_old)
        v_jre = PackageVersion("1.0.9-jre", d_new)
        versions = [v_android, v_jre]

        self.assertEqual(
            self.scanner.find_lowest(TIER1_TOOLCHAIN, v_android, versions),
            v_jre,
        )

    def test_semver_comparison_numeric_patch(self):
        """Test semver comparison with numeric patch version."""
        # Today is 2026-06-10
        self.scanner.date = date(2026, 6, 10)

        # 1.0.9 is older, 1.0.27 is newer and makes 1.0.9 stale
        d_old = date(2026, 2, 1)
        d_new = date(2026, 5, 1)
        v_9 = PackageVersion("1.0.9", d_old)
        v_27 = PackageVersion("1.0.27", d_new)
        versions = [v_9, v_27]

        self.assertEqual(
            self.scanner.find_lowest(TIER1_TOOLCHAIN, v_9, versions), v_27
        )

    def test_invalid_current_version_with_releases_raises_error(self):
        """Test find_lowest with invalid current version."""
        d0 = date(2026, 6, 1)
        v1_0 = PackageVersion("1.0.0", d0)
        current = PackageVersion("invalid_version", d0)
        with self.assertRaises(ValueError):
            self.scanner.find_lowest(TIER1_TOOLCHAIN, current, [v1_0])


class TestSemverHelpers(unittest.TestCase):
    """Tests for semver helper functions."""

    def setUp(self):
        self.scanner = FakePackageScanner('fake', 'fake')

    def test_parse_semver(self):
        """Test _parse_semver helper."""
        self.assertEqual(self.scanner._parse_semver("1.2.3-alpha.1"), (1, 2, 3))
        self.assertEqual(self.scanner._parse_semver("31.1-jre"), (31, 1, 0))
        self.assertEqual(self.scanner._parse_semver("1.2"), (1, 2, 0))
        self.assertEqual(self.scanner._parse_semver("1"), (1, 0, 0))
        self.assertEqual(self.scanner._parse_semver("31.1.0-jre"), (31, 1, 0))
        self.assertEqual(self.scanner._parse_semver("10.5.1.bcr.4"), (10, 5, 1))
        self.assertEqual(
            self.scanner._parse_semver(
                "094063176456395e02ab2108f858ff0eb46487ce"
            ),
            None,
        )
        self.assertEqual(
            self.scanner._parse_semver("2@13.3.rel1.1"), (13, 3, 0)
        )
        self.assertEqual(self.scanner._parse_semver("invalid"), None)

    def test_strict_semver(self):
        """Test strict_semver behavior."""
        self.scanner.strict_semver = True
        # Pre-releases (suffix starts with '-') should return None
        self.assertIsNone(self.scanner._parse_semver("1.2.3-alpha.1"))
        self.assertIsNone(self.scanner._parse_semver("31.1-jre"))
        self.assertIsNone(self.scanner._parse_semver("31.1.0-jre"))

        # Non-pre-releases should still work
        self.assertEqual(self.scanner._parse_semver("1.2.3"), (1, 2, 3))
        self.assertEqual(self.scanner._parse_semver("1.2.3+build.1"), (1, 2, 3))
        self.assertEqual(self.scanner._parse_semver("1.2"), (1, 2, 0))
        self.assertEqual(self.scanner._parse_semver("1"), (1, 0, 0))

    def test_clean_version(self):
        """Test clean_version helper."""
        self.assertEqual(self.scanner.clean_version("2@13.3.rel1.1"), "13.3.0")
        self.assertEqual(self.scanner.clean_version("31.1-jre"), "31.1.0")

        # With strict_semver
        self.scanner.strict_semver = True
        # Should not clean if invalid under strict
        self.assertEqual(self.scanner.clean_version("31.1-jre"), "31.1-jre")
        self.assertEqual(self.scanner.clean_version("1.2.3"), "1.2.3")


if __name__ == "__main__":
    unittest.main()
