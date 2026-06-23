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
"""Utility to scan packages for freshness."""


from datetime import date
import os
import re
from typing import Iterator, NamedTuple
from pw_fortifier.find_core_owners import CoreOwnerFinder


_SEMVER_BODY = r'^[~^>=<]*(?:\d+@)?v?(\d+)(?:\.(\d+)(?:\.(\d+))?)?([-+.].*)?$'
_SEMVER_SEARCH_RE = re.compile(_SEMVER_BODY)


TIER0_ON_DEVICE = 0
TIER1_TOOLCHAIN = 1
TIER2_DEVHOST = 2
TIER3_UPSTREAM = 3


class PackageVersion(NamedTuple):
    """Represents a specific version of a package with its release timestamp.

    Attributes:
        version: The version string (e.g., '1.2.3', 'git_revision:abc').
        timestamp: The release date of this version.
    """

    version: str
    timestamp: date


class FreshnessScanResult(NamedTuple):
    """The result of scanning a single package dependency for updates.

    Attributes:
        package: The name of the package (e.g., 'protobuf',
            'com.google.guava:guava').
        source: The source file declaring the dependency (e.g.,
            'MODULE.bazel', 'package.json').
        pkg_type: The package manager type ('bazel_dep', 'bazel_maven',
            'npm', 'pip', etc.).
        current: The currently used version of the package.
        earliest: The oldest available version of the package that is
            considered fresh and is greater than or equal to current.
        tier: The classification tier of the dependency:
            - TIER0_ON_DEVICE (0): Deployed directly to target devices.
            - TIER1_TOOLCHAIN (1): Part of target build toolchains.
            - TIER2_DEVHOST (2): Host tools / developer host dependencies.
            - TIER3_UPSTREAM (3): Other upstream / external dependencies.
        owner: The email address or identifier of the team/person owning
            this dependency, or None if not found.
    """

    package: str
    source: str
    pkg_type: str
    current: PackageVersion
    earliest: PackageVersion
    tier: int
    owner: str | None


class PackageScanner:
    """Base class for package scanners."""

    def __init__(self, root: str | os.PathLike[str] | None = None) -> None:
        self._root = os.path.abspath(root) if root is not None else None
        self._scanned_packages: set[str] = set()
        self.date: date = date.today()
        self.strict_semver: bool = False

    def _parse_semver(self, ver_str: str) -> tuple[int, int, int] | None:
        """Parses a version string into a (major, minor, patch) tuple.

        Uses _SEMVER_SEARCH_RE to match and defaults missing minor/patch to 0.
        """
        match = _SEMVER_SEARCH_RE.search(ver_str)
        if match:
            suffix = match.group(4)
            if self.strict_semver and suffix and suffix.startswith('-'):
                return None
            major = int(match.group(1))
            minor = int(match.group(2)) if match.group(2) is not None else 0
            patch = int(match.group(3)) if match.group(3) is not None else 0
            return (major, minor, patch)
        return None

    def clean_version(self, ver_str: str) -> str:
        """Extracts a clean X.Y.Z version from a semver range."""
        parsed = self._parse_semver(ver_str)
        if parsed:
            major, minor, patch = parsed
            return f'{major}.{minor}.{patch}'
        return ver_str

    def major_version(self, ver_str: str) -> str | None:
        """Extracts the major version from a version string."""
        cleaned = self.clean_version(ver_str)
        parsed = self._parse_semver(cleaned)
        return str(parsed[0]) if parsed else None

    @property
    def root(self) -> str:
        """Returns the root directory. Raises ValueError if not set."""
        if self._root is None:
            raise ValueError("root directory must be set before scanning")
        return self._root

    @root.setter
    def root(self, value: str | os.PathLike[str]) -> None:
        self._root = os.path.abspath(value)

    def find_lowest_release(
        self,
        tier: int,
        current: PackageVersion,
        versions: list[PackageVersion],
    ) -> PackageVersion | None:
        """Attempts to find the lowest fresh release version.

        Returns None if no versions can be parsed as SemVer.
        """
        parsed_versions = []
        for v in versions:
            parsed = self._parse_semver(v.version)
            if parsed is not None:
                parsed_versions.append((parsed, v))

        if not parsed_versions:
            return None

        # Sort by semver and then by timestamp
        parsed_versions.sort(key=lambda x: (x[0], x[1].timestamp))

        current_parsed = self._parse_semver(current.version)
        if current_parsed is None:
            raise ValueError(
                f"Current version '{current.version}' is not a valid semver, "
                "but release versions were found: "
                f"{[v.version for v in versions]}"
            )

        # Filter candidates >= current
        candidates = [
            (p, v)
            for p, v in parsed_versions
            if (p, v.timestamp) >= (current_parsed, current.timestamp)
        ]

        for p_cand, v_cand in candidates:
            is_stale = False
            # Newer versions are those with (parsed, timestamp) >
            # (p_cand, v_cand.timestamp).
            for p_newer, v_newer in parsed_versions:
                if (p_newer, v_newer.timestamp) > (p_cand, v_cand.timestamp):
                    age = self.date - v_newer.timestamp
                    if tier in (TIER0_ON_DEVICE, TIER1_TOOLCHAIN):
                        if age.days >= 90:
                            is_stale = True
                            break
                        if p_newer[0] == p_cand[0] and age.days >= 30:
                            is_stale = True
                            break
                    else:  # Tier 2/3
                        if age.days >= 365:
                            is_stale = True
                            break
                        if p_newer[0] == p_cand[0] and age.days >= 90:
                            is_stale = True
                            break
            if not is_stale:
                return v_cand

        # If no candidate is fresh, fallback to latest
        return parsed_versions[-1][1]

    def find_lowest_revision(
        self,
        tier: int,
        current: PackageVersion,
        versions: list[PackageVersion],
    ) -> PackageVersion:
        """Finds the lowest fresh revision-based version."""
        # Revision-based. Sort by timestamp.
        sorted_versions = sorted(versions, key=lambda x: x.timestamp)

        # Filter candidates >= current
        candidates = [
            v for v in sorted_versions if v.timestamp >= current.timestamp
        ]

        for v_cand in candidates:
            is_stale = False
            # Newer versions are those with timestamp > v_cand.timestamp
            for v_newer in sorted_versions:
                if v_newer.timestamp > v_cand.timestamp:
                    age = self.date - v_newer.timestamp
                    if tier in (TIER0_ON_DEVICE, TIER1_TOOLCHAIN):
                        if age.days >= 30:
                            is_stale = True
                            break
                    else:  # Tier 2/3
                        if age.days >= 90:
                            is_stale = True
                            break
            if not is_stale:
                return v_cand

        # If no candidate is fresh, fallback to latest
        return sorted_versions[-1]

    def find_lowest(
        self,
        tier: int,
        current: PackageVersion,
        versions: list[PackageVersion],
    ) -> PackageVersion:
        """Returns the lowest version that is >= current and is fresh."""
        if not versions:
            return current

        lowest_release = self.find_lowest_release(tier, current, versions)
        if lowest_release is not None:
            return lowest_release

        return self.find_lowest_revision(tier, current, versions)

    def _find_owners(
        self,
        file_path: str | os.PathLike[str],
        *args: str | re.Pattern,
    ) -> str | None:
        """Finds the owner of the first line containing all args (or matching
        regexes).
        """
        abs_path = os.path.normpath(os.path.join(self.root, file_path))
        try:
            with open(abs_path, 'r') as f:
                lines = f.readlines()
        except IOError:
            return None

        for idx, line in enumerate(lines):
            match = True
            for arg in args:
                if isinstance(arg, re.Pattern):
                    if not arg.search(line):
                        match = False
                        break
                else:
                    if arg not in line:
                        match = False
                        break
            if match:
                line_num = idx + 1
                finder = CoreOwnerFinder(root=self.root)
                finder.add(abs_path, (line_num, line_num))
                return finder.find()
        return None

    # pylint: disable=no-self-use
    def pre_scan(self) -> Iterator[FreshnessScanResult]:
        """Performs setup operations before scanning starts.

        Yields:
            Any initial freshness scan results.
        """
        yield from ()

    def target(self) -> str | None:
        """Returns the target filename this scanner should be called on, or
        None if it does not scan files during the walk phase.
        """
        return None

    # pylint: disable=unused-argument
    def scan(
        self, pathname: str | os.PathLike[str]
    ) -> Iterator[FreshnessScanResult]:
        """Examines packages described by the given path for freshness.

        The `root` attribute must be set before this method is called,
        otherwise a ValueError will be thrown.

        Args:
          pathname: File or directory path relative to the root directory
            that the scanner can use to find packages.
        """
        yield from ()

    # pylint: disable=no-self-use
    def post_scan(self) -> Iterator[FreshnessScanResult]:
        """Performs teardown operations after scanning finishes.

        Yields:
            Any final freshness scan results.
        """
        yield from ()


class PackageScannerRegistry:
    """Registry for package scanners.

    Supports registering scanners with target filenames.
    """

    def __init__(self, root: str | os.PathLike[str]):
        self.root = os.path.abspath(root)
        self._all_scanners: list[PackageScanner] = []
        self._scanners: dict[str, PackageScanner] = {}
        self._pruned_dirs: set[str] = set()

    def register(self, scanner: PackageScanner) -> None:
        """Registers the given scanner.

        The scanner is registered to handle files matching the target returned
        by its `target()` method.

        Args:
            scanner: Produces zero or more freshness scan results.
        """
        scanner.root = self.root
        if scanner not in self._all_scanners:
            self._all_scanners.append(scanner)

        target = scanner.target()
        if target is None:
            return

        self._scanners[target] = scanner

    def prune(self, dir_name: str | os.PathLike[str]) -> None:
        """Adds the given directory to those that will not be scanned.

        Args:
          dir_name: Directory path relative to the repository root.
        """
        path = os.path.normpath(dir_name)
        if os.path.isabs(path):
            if os.path.normcase(path).startswith(os.path.normcase(self.root)):
                path = os.path.relpath(path, self.root)
        self._pruned_dirs.add(path)

    def walk(self) -> Iterator[str]:
        """Walks the directory tree yielding relative paths to files and
        directories.
        """
        for root, dirs, files in os.walk(self.root):
            rel_root = os.path.relpath(root, self.root)

            # Prune directories in-place to prevent os.walk from descending
            # into them.
            dirs_to_keep = []
            for d in dirs:
                if rel_root == '.':
                    rel_dir = d
                else:
                    rel_dir = os.path.normpath(os.path.join(rel_root, d))

                if rel_dir in self._pruned_dirs:
                    continue
                dirs_to_keep.append(d)
            dirs[:] = dirs_to_keep

            # Check files in the current directory
            for f in files:
                if rel_root == '.':
                    rel_file = f
                else:
                    rel_file = os.path.normpath(os.path.join(rel_root, f))
                yield rel_file

    def scan_all(self) -> Iterator[FreshnessScanResult]:
        """Generates scan results by passing files to registered scanners.

        This function walks all directories in the root directory except
        those that have been pruned. It checks files against its registered
        targets, and passes those that match to the corresponding scanner.
        It aggregates and returns the results.

        Returns:
            An iterator over the freshness scan results.
        """
        for scanner in self._all_scanners:
            yield from scanner.pre_scan()

        for rel_path in self.walk():
            basename = os.path.basename(rel_path)
            active_scanner = self._scanners.get(basename)
            if active_scanner:
                yield from active_scanner.scan(rel_path)

        for scanner in self._all_scanners:
            yield from scanner.post_scan()
