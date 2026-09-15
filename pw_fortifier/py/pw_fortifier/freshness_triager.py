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
"""Defines the FreshnessTriager class for assessing freshness results."""

from pw_fortifier.freshness_result import FreshnessResult
from pw_fortifier.issue import Issue
from pw_fortifier.triager import Triager


class FreshnessTriager(Triager):
    """Triager implementation for assessing freshness results."""

    ISSUE_TYPE: type[Issue] = FreshnessResult

    async def _triage(self, issue: Issue) -> None:
        """Assesses and modifies a freshness result in place."""
        assert isinstance(issue, FreshnessResult)

        issue.priority = issue.tier // 2 + 1  # Tiers 0,1 => P1, Tiers 2,3 => P2
        issue.severity = issue.tier // 2 + 2  # Tiers 0,1 => S2, Tiers 2,3 => S3
