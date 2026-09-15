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
"""Defines the FreshnessResultCollector for freshness scan results."""

from datetime import date

from pw_fortifier.async_path import AsyncPath
from pw_fortifier.collector import Collector
from pw_fortifier.freshness_result import (
    FreshnessResult,
    get_display_version,
    get_tier_description,
)


class FreshnessResultCollector(Collector):
    """Collector that formats and prints freshness results."""

    def __init__(self) -> None:
        """Initializes column specifications for freshness results."""
        super().__init__()
        self._add_field('package', min_width=36)
        self._add_field('tier', hidden=True)
        self._add_field('tier_desc', hidden=True)
        self._add_field('type', min_width=11, hidden=True)
        self._add_field('freshness')
        self._add_field('freshness_desc', hidden=True)
        self._add_field('current_ver', hidden=True)
        self._add_field('current', min_width=13)
        self._add_field('current_ts', hidden=True)
        self._add_field('earliest_ver', hidden=True)
        self._add_field('earliest', min_width=13)
        self._add_field('earliest_ts', hidden=True)
        self._add_field('earliest_age', hidden=True)
        self._add_field('assignee', min_width=11, hidden=True)
        self._add_field('source', min_width=-1, truncate_from_left=True)

    async def _print_item(self, path: AsyncPath) -> None:
        """Prints a single freshness result item."""
        result = await FreshnessResult.load(path)

        if result.current.version == result.earliest.version:
            freshness = 100
        else:
            freshness = -(
                result.earliest.timestamp - result.current.timestamp
            ).days

        if freshness == 100:
            freshness_desc = 'Fresh'
        elif freshness >= 0:
            freshness_desc = f'{freshness} days left'
        else:
            freshness_desc = f'{abs(freshness)} days overdue'

        tier_desc = get_tier_description(result.tier)

        self._report_fields(
            result.package,
            str(result.tier),
            tier_desc,
            result.pkg_type,
            str(freshness),
            freshness_desc,
            result.current.version,
            get_display_version(result.current),
            result.current.timestamp.isoformat(),
            result.earliest.version,
            get_display_version(result.earliest),
            result.earliest.timestamp.isoformat(),
            str((date.today() - result.earliest.timestamp).days),
            result.assignee or '',
            result.source,
        )
