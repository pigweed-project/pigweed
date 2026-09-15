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
"""Defines the FreshnessResult and PackageVersion classes."""

from dataclasses import dataclass
from datetime import date
import logging
import re
from typing import NamedTuple, Any

from pw_fortifier.code_snippet import CodeSnippet
from pw_fortifier.issue import Issue

_LOG = logging.getLogger(__name__)

TIER0_ON_DEVICE = 0
TIER1_TOOLCHAIN = 1
TIER2_DEVHOST = 2
TIER3_UPSTREAM = 3

_TITLE_PATTERN = re.compile(
    r'^Version\s+'
    r'(?:(?:git_revision|git_revisions|g3-revision|g3_revision|version):)?'
    r'(?P<version>.+?)\s+of\s+(?P<package>.+?)\s+'
    r'is\s+stale\s+and\s+needs\s+to\s+be\s+updated$'
)
_POLICY_MARKER = 'http://go/pigweed-3p-freshness-policy'
_TIER_PATTERN = re.compile(
    r'dependency that is (?P<tier_desc>on device|used to build|dev tooling|'
    r'upstream-only) is stale'
)
_CURRENT_PATTERN = re.compile(
    r'has a current version of\s+(?P<version>[^\n\r]+?)\s+from\s+'
    r'(?P<ts>\d{4}-\d{2}-\d{2})'
)
_EARLIEST_PATTERN = re.compile(
    r'The earliest version that is still considered fresh according to '
    r'the policy is\s+(?P<version>[^,\n]+?)'
    r'(?:,\s*from\s*(?P<ts>\d{4}-\d{2}-\d{2}))?\.\s*(?:\n|$)'
)
_SOURCE_PATTERN = re.compile(
    r"This\s+(?:(?P<pkg_type>\w+)\s+)?dependency's\s+version\s+was\s+"
    r'determined\s+from\s+(?P<source>[^\n\r]+?)\.\s*(?:\n|$)'
)
_SHA1_PATTERN = re.compile(r'^[0-9a-fA-F]{9,40}$')

_TIER_BY_DESCRIPTION = {
    'on device': TIER0_ON_DEVICE,
    'used to build': TIER1_TOOLCHAIN,
    'dev tooling': TIER2_DEVHOST,
    'upstream-only': TIER3_UPSTREAM,
}


class PackageVersion(NamedTuple):
    """Represents a specific version of a package with its release timestamp."""

    version: str
    """The version string (e.g., '1.2.3', 'git_revision:abc')."""

    timestamp: date
    """The release date of this version."""


def get_display_version(version: str | PackageVersion) -> str:
    """Returns a human-readable display string for a version.

    Strips prefixes like 'git_revisions:', 'git_revision:', 'g3-revision:',
    and 'version:', and abbreviates 40-character commit hashes to 9 characters.

    Args:
        version: PackageVersion or raw version string.

    Returns:
        Human-readable abbreviated version string.
    """
    v_str = (
        version.version if isinstance(version, PackageVersion) else str(version)
    )
    if v_str.startswith('git_revisions:'):
        v_str = v_str.removeprefix('git_revisions:')
        v_str = v_str.split(',')[0].strip()
    elif v_str.startswith('git_revision:'):
        v_str = v_str.removeprefix('git_revision:')
    elif v_str.startswith('g3-revision:'):
        v_str = v_str.removeprefix('g3-revision:')
    elif v_str.startswith('g3_revision:'):
        v_str = v_str.removeprefix('g3_revision:')
    elif v_str.startswith('version:'):
        v_str = v_str.removeprefix('version:')

    if _SHA1_PATTERN.match(v_str):
        return v_str[:9]
    return v_str


@dataclass(kw_only=True)
class FreshnessResult(Issue):
    """The result of scanning a single package dependency for updates."""

    package: str
    pkg_type: str
    current: PackageVersion
    earliest: PackageVersion
    tier: int

    @property
    def source(self) -> str:
        """Convenience property returning the manifest file path."""
        return str(self.location.file) if self.location else ''

    @classmethod
    def _parse_dict(cls, data: dict[str, Any]) -> dict[str, Any]:
        """Parses and normalizes result fields from a dictionary."""
        kwargs = super()._parse_dict(data)

        current_data = data['current']
        kwargs['current'] = PackageVersion(
            version=current_data['version'],
            timestamp=date.fromisoformat(current_data['timestamp']),
        )

        earliest_data = data['earliest']
        kwargs['earliest'] = PackageVersion(
            version=earliest_data['version'],
            timestamp=date.fromisoformat(earliest_data['timestamp']),
        )

        kwargs['package'] = data['package']
        kwargs['pkg_type'] = data['pkg_type']
        kwargs['tier'] = data['tier']

        return kwargs

    def _to_dict(self) -> dict[str, Any]:
        """Converts result to a dictionary for serialization."""
        data = super()._to_dict()
        data['current'] = {
            'version': self.current.version,
            'timestamp': self.current.timestamp.isoformat(),
        }
        data['earliest'] = {
            'version': self.earliest.version,
            'timestamp': self.earliest.timestamp.isoformat(),
        }
        return data

    @classmethod
    def from_issue(cls, issue: Issue) -> 'FreshnessResult | None':
        """Converts an Issue from the tracker into a FreshnessResult.

        Args:
            issue: The Issue report fetched from the tracker.

        Returns:
            The converted FreshnessResult instance, or None if invalid.
        """
        if not issue.title:
            return None

        title_match = _TITLE_PATTERN.match(issue.title.strip())
        if not title_match:
            return None

        package = title_match.group('package')

        corpus_texts = []
        if issue.description:
            corpus_texts.append(issue.description)
        corpus_texts.extend(issue.comments)
        combined_text = '\n'.join(corpus_texts)

        if _POLICY_MARKER not in combined_text:
            return None

        tier_match = _TIER_PATTERN.search(combined_text)
        if tier_match:
            tier = _TIER_BY_DESCRIPTION[tier_match.group('tier_desc')]
        elif issue.priority == 1 or issue.severity == 2:
            tier = TIER0_ON_DEVICE
        else:
            tier = TIER2_DEVHOST

        current_match = _CURRENT_PATTERN.search(combined_text)
        if not current_match:
            _LOG.warning(
                'Malformed freshness issue %s: missing current version pattern',
                issue.issue_id,
            )
            return None

        current_version = current_match.group('version').strip()
        try:
            current_ts = date.fromisoformat(current_match.group('ts'))
        except ValueError as e:
            _LOG.warning(
                'Malformed freshness issue %s: invalid current date %r: %s',
                issue.issue_id,
                current_match.group('ts'),
                e,
            )
            return None

        earliest_version = current_version
        earliest_ts = date.min
        earliest_match = _EARLIEST_PATTERN.search(combined_text)
        if earliest_match:
            earliest_version = earliest_match.group('version').strip()
            if earliest_match.group('ts'):
                try:
                    earliest_ts = date.fromisoformat(earliest_match.group('ts'))
                except ValueError as e:
                    _LOG.warning(
                        'Malformed freshness issue %s: invalid earliest date '
                        '%r: %s',
                        issue.issue_id,
                        earliest_match.group('ts'),
                        e,
                    )
                    return None

        location = CodeSnippet(file='unknown')
        pkg_type = 'unknown'
        source_match = _SOURCE_PATTERN.search(combined_text)
        if source_match:
            source_raw = source_match.group('source').strip()
            if source_match.group('pkg_type'):
                pkg_type = source_match.group('pkg_type').strip()
            if ':' in source_raw:
                file_part, line_part = source_raw.rsplit(':', 1)
                try:
                    if '-' in line_part:
                        start_s, end_s = line_part.split('-', 1)
                        lines = (int(start_s), int(end_s))
                    else:
                        line_num = int(line_part)
                        lines = (line_num, line_num)
                    location = CodeSnippet(file=file_part, lines=lines)
                except ValueError:
                    location = CodeSnippet(file=source_raw)
            else:
                location = CodeSnippet(file=source_raw)

        if issue.issue_id is None or issue.issue_id == 0:
            _LOG.warning(
                'Malformed freshness issue for package %s: missing or invalid '
                'issue_id',
                package,
            )
            return None

        return FreshnessResult(
            issue_id=issue.issue_id,
            title=issue.title,
            description=issue.description,
            comments=list(issue.comments),
            priority=issue.priority,
            severity=issue.severity,
            assignee=issue.assignee,
            cl_num=issue.cl_num,
            package=package,
            location=location,
            pkg_type=pkg_type,
            current=PackageVersion(current_version, current_ts),
            earliest=PackageVersion(earliest_version, earliest_ts),
            tier=tier,
        )


def get_tier_description(tier: int) -> str:
    """Returns a descriptive string for a dependency classification tier.

    Args:
        tier: Integer tier level (0-3).

    Returns:
        Human-readable description string of the tier.
    """
    if tier == TIER0_ON_DEVICE:
        return 'on device'
    if tier == TIER1_TOOLCHAIN:
        return 'used to build'
    if tier == TIER2_DEVHOST:
        return 'dev tooling'
    if tier == TIER3_UPSTREAM:
        return 'upstream-only'
    return 'unknown'
