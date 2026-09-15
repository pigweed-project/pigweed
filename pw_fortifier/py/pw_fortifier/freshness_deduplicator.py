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
"""Defines FreshnessDeduplicator for deduplicating freshness results."""

from pw_fortifier.deduplicator import Deduplicator
from pw_fortifier.freshness_result import (
    FreshnessResult,
    get_display_version,
)
from pw_fortifier.freshness_result_tracker import FreshnessResultWriter
from pw_fortifier.issue import Issue
from pw_fortifier.issue_tracker import IssueTracker


def _versions_match(v1: str, v2: str) -> bool:
    """Checks if two version strings match semantically."""
    if v1 == v2:
        return True
    if len(v1) >= 9 and len(v2) >= 9:
        return v1[:9] == v2[:9] or v1.startswith(v2) or v2.startswith(v1)
    return False


class FreshnessDeduplicator(Deduplicator):
    """Deduplicator implementation for assessing freshness results."""

    ISSUE_TYPE: type[Issue] = FreshnessResult

    def __init__(self) -> None:
        """Initializes the freshness deduplicator."""
        super().__init__()
        self._issue_tracker: IssueTracker | None = None
        self._results: list[Issue] | None = None

    @property
    def issue_tracker(self) -> IssueTracker:
        """The issue tracker client."""
        assert self._issue_tracker is not None
        return self._issue_tracker

    @issue_tracker.setter
    def issue_tracker(self, issue_tracker: IssueTracker) -> None:
        self._issue_tracker = issue_tracker
        self._results = None

    async def _is_duplicate(self, issue: Issue) -> int | None:
        """Checks if finding is a duplicate of a previously seen defect.

        Args:
            issue: The freshness finding to check.

        Returns:
            The duplicate issue ID if found, or None otherwise.
        """
        assert isinstance(issue, FreshnessResult)
        assert self._issue_tracker is not None

        if self._results is None:
            if self._issue_tracker.primary_hotlist_id is not None:
                self._results = [
                    res
                    async for res in self._issue_tracker.read_hotlist(
                        self._issue_tracker.primary_hotlist_id
                    )
                ]
            else:
                self._results = []

        target_ver = get_display_version(issue.current)
        title = FreshnessResultWriter.make_title(issue)

        for result in self._results:
            converted = self.ISSUE_TYPE.from_issue(result)
            if (
                isinstance(converted, FreshnessResult)
                and converted.issue_id is not None
            ):
                if converted.package == issue.package:
                    c_ver = get_display_version(converted.current)
                    if _versions_match(c_ver, target_ver):
                        return converted.issue_id
            elif result.issue_id is not None and result.title == title:
                return result.issue_id

        return None
