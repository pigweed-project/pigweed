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

from pw_fortifier.async_path import AsyncPath
from pw_fortifier.code_snippet import CodeSnippet
from pw_fortifier.freshness_result import (
    PackageVersion,
    FreshnessResult,
    TIER0_ON_DEVICE,
    TIER1_TOOLCHAIN,
)
from pw_fortifier.pipeline_stage import PipelineStage
from pw_fortifier.semver import SemVer, parse_semver

DATE = date.today()


def _find_lowest_release(
    tier: int,
    current: PackageVersion,
    versions: list[PackageVersion],
    loose_semver: bool = False,
) -> PackageVersion | None:
    """Attempts to find the lowest fresh release version."""
    all_parsed = []
    for v in versions:
        parsed = parse_semver(v.version, loose_semver)
        if parsed is not None:
            all_parsed.append((parsed, v))

    if not all_parsed:
        return None

    if any(p.prerelease is None for p, _ in all_parsed):
        parsed_versions = [
            (p, v) for p, v in all_parsed if p.prerelease is None
        ]
    else:
        parsed_versions = all_parsed

    parsed_versions.sort(key=lambda x: (x[0], x[1].timestamp))

    current_parsed = parse_semver(current.version, loose_semver)
    if current_parsed is None:
        raise ValueError(
            f"Current version '{current.version}' is not a valid semver, "
            'but release versions were found: '
            f'{[v.version for v in versions]}'
        )

    candidates = [
        (p, v)
        for p, v in parsed_versions
        if (p, v.timestamp) >= (current_parsed, current.timestamp)
    ]

    for p_cand, v_cand in candidates:
        is_stale = False
        for p_newer, v_newer in parsed_versions:
            if (p_newer, v_newer.timestamp) > (p_cand, v_cand.timestamp):
                age = DATE - v_newer.timestamp
                if tier in (TIER0_ON_DEVICE, TIER1_TOOLCHAIN):
                    if age.days >= 90:
                        is_stale = True
                        break
                    if p_newer[0] == p_cand[0] and age.days >= 30:
                        is_stale = True
                        break
                else:
                    if age.days >= 365:
                        is_stale = True
                        break
                    if p_newer[0] == p_cand[0] and age.days >= 90:
                        is_stale = True
                        break
        if not is_stale:
            return v_cand

    return parsed_versions[-1][1]


def _find_lowest_revision(
    tier: int,
    current: PackageVersion,
    versions: list[PackageVersion],
) -> PackageVersion:
    """Finds the lowest fresh revision-based version."""
    sorted_versions = sorted(versions, key=lambda x: x.timestamp)

    candidates = [
        v for v in sorted_versions if v.timestamp >= current.timestamp
    ]

    for v_cand in candidates:
        is_stale = False
        for v_newer in sorted_versions:
            if v_newer.timestamp > v_cand.timestamp:
                age = DATE - v_newer.timestamp
                if tier in (TIER0_ON_DEVICE, TIER1_TOOLCHAIN):
                    if age.days >= 30:
                        is_stale = True
                        break
                else:
                    if age.days >= 90:
                        is_stale = True
                        break
        if not is_stale:
            return v_cand

    return sorted_versions[-1]


def find_lowest(
    tier: int,
    current: PackageVersion,
    versions: list[PackageVersion],
    loose_semver: bool = False,
) -> PackageVersion:
    """Returns the lowest version that is >= current and is fresh.

    Args:
        tier: Classification tier integer.
        current: Currently used PackageVersion.
        versions: List of available PackageVersion candidates.
        loose_semver: True if pre-release labels should be treated as extra
            labels.

    Returns:
        The lowest fresh PackageVersion.
    """
    if not versions:
        return current

    lowest_release = _find_lowest_release(tier, current, versions, loose_semver)
    if lowest_release is not None:
        return lowest_release

    return _find_lowest_revision(tier, current, versions)


class PackageAnalyzer(PipelineStage):
    """Base class for package analyzers."""

    PKG_TYPE: str = ''
    TARGET: str | None = None

    def __init__(self) -> None:
        """Initializes the package analyzer."""
        super().__init__()
        self._scanned_packages: set[str] = set()
        self.loose_semver: bool = False
        self._preserve_inputs = True

    @property
    def skip_setup(self) -> bool:
        """Whether to skip the setup phase in run()."""
        return self._skip_setup

    @skip_setup.setter
    def skip_setup(self, value: bool) -> None:
        if value and not self._skip_setup and self.TARGET is None:
            self.input_queue.put_nowait(None)
        self._skip_setup = value

    def parse_semver(self, ver_str: str) -> SemVer | None:
        """Parses a version string into a SemVer named tuple."""
        return parse_semver(ver_str, self.loose_semver)

    def clean_version(self, ver_str: str) -> str:
        """Extracts a clean X.Y.Z version from a semver range.

        Args:
            ver_str: Raw version string to clean.

        Returns:
            Cleaned version string.
        """
        parsed = parse_semver(ver_str, self.loose_semver)
        if parsed:
            return f'{parsed.major}.{parsed.minor}.{parsed.patch}'
        return ver_str

    def major_version(self, ver_str: str) -> str | None:
        """Extracts the major version from a version string.

        Args:
            ver_str: Raw version string.

        Returns:
            Major version string or None.
        """
        cleaned = self.clean_version(ver_str)
        parsed = parse_semver(cleaned, self.loose_semver)
        return str(parsed.major) if parsed else None

    @property
    def root(self) -> str:
        """Returns the root directory.

        The `configure` method **must** be called before getting this property.

        Returns:
            Root directory path string.
        """
        assert self._src_repo is not None
        return str(self._src_repo.project_dir)

    def find_lowest(
        self,
        tier: int,
        current: PackageVersion,
        versions: list[PackageVersion],
    ) -> PackageVersion:
        """Returns the lowest version that is >= current and is fresh.

        Args:
            tier: Classification tier integer.
            current: Currently used PackageVersion.
            versions: List of available PackageVersion candidates.

        Returns:
            Lowest fresh PackageVersion.
        """
        return find_lowest(tier, current, versions, self.loose_semver)

    async def _send_result(self, result: FreshnessResult | None) -> None:
        if result is None:
            return
        result_path = await self._generate_out_path()
        await result.save(result_path)
        self.send(result_path)


################################################################################
# Test support


class PackageAnalyzerStub(PackageAnalyzer):
    """Stub package analyzer for testing registry."""

    TARGET = 'fake'

    def __init__(self) -> None:
        """Initializes fake scanner."""
        super().__init__()
        self.loose_semver = True
        self.scanned_paths: list[str] = []

    async def _process_one(self, path: AsyncPath) -> None:
        """Scans file."""
        self.scanned_paths.append(os.path.normpath(str(path)))
        result = FreshnessResult(
            package=self.name,
            location=CodeSnippet(file=path.name),
            pkg_type='fake',
            current=PackageVersion('1.0', date(2026, 1, 1)),
            earliest=PackageVersion('1.0', date(2026, 1, 1)),
            tier=1,
            assignee='me',
        )
        await self._send_result(result)
