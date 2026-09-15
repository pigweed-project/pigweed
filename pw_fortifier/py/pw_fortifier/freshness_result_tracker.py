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
"""Defines issue tracker adapter classes for freshness result issues."""

from pw_fortifier.async_path import AsyncPath
from pw_fortifier.freshness_result import (
    FreshnessResult,
    get_display_version,
    get_tier_description,
)
from pw_fortifier.issue import Issue
from pw_fortifier.issue_tracker import IssueReader, IssueWriter


class FreshnessResultReader(IssueReader):
    """Stage that reads freshness result reports from an issue tracker."""

    ISSUE_TYPE: type[Issue] = FreshnessResult


class FreshnessResultWriter(IssueWriter):
    """Stage that writes freshness result reports to an issue tracker."""

    async def _load(self, path: AsyncPath) -> Issue:
        """Loads a freshness result and populates its title and description.

        Args:
            path: Path to the freshness result JSON file.

        Returns:
            The loaded FreshnessResult issue instance.
        """
        result = await FreshnessResult.load(path)
        result.title = self.make_title(result)
        result.description = self.make_description(result)
        return result

    @classmethod
    def make_title(cls, result: FreshnessResult) -> str:
        """Generates an issue title for the stale package finding.

        Args:
            result: The FreshnessResult finding.

        Returns:
            Formatted title string describing the stale version and package.
        """
        version = get_display_version(result.current)
        return (
            f'Version {version} of {result.package} is stale '
            'and needs to be updated'
        )

    @classmethod
    def make_description(cls, result: FreshnessResult) -> str:
        """Generates an issue description for the stale package finding.

        Args:
            result: The FreshnessResult finding.

        Returns:
            Formatted description string describing the freshness finding.
        """
        loc_str = 'unknown'
        if result.location:
            if result.location.lines:
                start, end = result.location.lines
                if start == end:
                    loc_str = f'{result.location.file}:{start}'
                else:
                    loc_str = f'{result.location.file}:{start}-{end}'
            else:
                loc_str = str(result.location.file)

        description_lines = [
            (
                'pw_fortifier has detected that a third-party dependency that '
                f'is {get_tier_description(result.tier)} is stale according to '
                'http://go/pigweed-3p-freshness-policy:'
            ),
            '',
            (
                f'{result.package} has a current version of '
                f'{result.current.version} from '
                f'{result.current.timestamp.isoformat()}.'
            ),
            '',
            (
                'The earliest version that is still considered fresh according '
                f'to the policy is {result.earliest.version}, from '
                f'{result.earliest.timestamp.isoformat()}.'
            ),
            '',
            (
                f"This {result.pkg_type} dependency's version was determined "
                f'from {loc_str}.'
            ),
            '',
            (
                'Please update this package or file for an exception in '
                'accordance with the freshness policy.'
            ),
        ]
        return '\n'.join(description_lines)
