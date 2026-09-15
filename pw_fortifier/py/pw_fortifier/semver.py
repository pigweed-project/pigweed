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
"""Semantic version parsing and comparison utilities."""

import re
from typing import NamedTuple

_SEMVER_BODY = (
    r'^[~^>=<]*(?:\d+@)?v?'
    r'(\d+)'
    r'(?:\.(\d+)(?:\.(\d+))?)?'
    r'(\.[0-9a-zA-Z.]+)?'
    r'(?:-([0-9a-zA-Z.-]+))?'
    r'(?:\+([0-9a-zA-Z.-]+))?$'
)
_SEMVER_SEARCH_RE = re.compile(_SEMVER_BODY)


class SemVer(NamedTuple):
    """Semantic version representation.

    Attributes:
        major: Major version number.
        minor: Minor version number, defaulting to 0.
        patch: Patch version number, defaulting to 0.
        extra: Extra packaging or distribution label (e.g. '.bcr.4', '-1').
        prerelease: Pre-release label (e.g. 'alpha.1', 'rc2').
        build: Build metadata label.
    """

    major: int
    minor: int = 0
    patch: int = 0
    extra: str | None = None
    prerelease: str | None = None
    build: str | None = None

    def __str__(self) -> str:
        """Returns the string representation of this SemVer."""
        extra = self.extra or ''
        s = f'{self.major}.{self.minor}.{self.patch}{extra}'
        if self.prerelease:
            s += f'-{self.prerelease}'
        if self.build:
            s += f'+{self.build}'
        return s

    def to_str(self) -> str:
        """Returns the string representation of this SemVer."""
        return str(self)

    def _cmp_key(self) -> tuple[int, int, int, str, int, str]:
        return (
            self.major,
            self.minor,
            self.patch,
            self.extra or '',
            1 if self.prerelease is None else 0,
            self.prerelease or '',
        )

    def __lt__(self, other: object) -> bool:
        if not isinstance(other, SemVer):
            return NotImplemented
        return self._cmp_key() < other._cmp_key()

    def __le__(self, other: object) -> bool:
        if not isinstance(other, SemVer):
            return NotImplemented
        return self._cmp_key() <= other._cmp_key()

    def __gt__(self, other: object) -> bool:
        if not isinstance(other, SemVer):
            return NotImplemented
        return self._cmp_key() > other._cmp_key()

    def __ge__(self, other: object) -> bool:
        if not isinstance(other, SemVer):
            return NotImplemented
        return self._cmp_key() >= other._cmp_key()

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, SemVer):
            return NotImplemented
        return self._cmp_key() == other._cmp_key()

    def __ne__(self, other: object) -> bool:
        if not isinstance(other, SemVer):
            return NotImplemented
        return self._cmp_key() != other._cmp_key()

    def __hash__(self) -> int:
        return hash(self._cmp_key())


def parse_semver(ver_str: str, loose_semver: bool = False) -> SemVer | None:
    """Parses a version string into a SemVer named tuple.

    Args:
        ver_str: Version string to parse.
        loose_semver: If True, treat pre-release labels as extra labels.

    Returns:
        SemVer instance if parsed, else None.
    """
    match = _SEMVER_SEARCH_RE.search(ver_str)
    if not match:
        return None

    major = int(match.group(1))
    minor = int(match.group(2)) if match.group(2) is not None else 0
    patch = int(match.group(3)) if match.group(3) is not None else 0
    extra = match.group(4) or None
    prerelease = match.group(5) or None
    build = match.group(6) or None

    if loose_semver and prerelease:
        if extra:
            extra = f'{extra}-{prerelease}'
        else:
            extra = f'-{prerelease}'
        prerelease = None

    return SemVer(major, minor, patch, extra, prerelease, build)
