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
"""Defines the Deduplicator base class for filtering duplicate issues."""

import argparse
from datetime import datetime
import logging

from pw_fortifier.async_path import AsyncPath
from pw_fortifier.issue import Issue
from pw_fortifier.issue_tracker import IssueTracker
from pw_fortifier.pipeline_stage import PipelineStage


class Deduplicator(PipelineStage):
    """Base class for stages that filter out duplicate defect findings."""

    ISSUE_TYPE: type[Issue] = Issue

    def __init__(self) -> None:
        super().__init__()
        self._issue_tracker: IssueTracker | None = None
        self._duplicates_dir: AsyncPath | None = None

    @property
    def issue_tracker(self) -> IssueTracker:
        """The issue tracker client."""
        assert self._issue_tracker is not None
        return self._issue_tracker

    @issue_tracker.setter
    def issue_tracker(self, issue_tracker: IssueTracker) -> None:
        self._issue_tracker = issue_tracker

    async def configure(self, args: argparse.Namespace) -> None:
        """Configures the stage and duplicates directory.

        Args:
            args: Command-line arguments namespace object.
        """
        await super().configure(args)
        working_dir = AsyncPath(args.working_dir)
        self._duplicates_dir = working_dir / 'duplicates'
        await self._duplicates_dir.mkdir(parents=True, exist_ok=True)

    async def _process_one(self, path: AsyncPath) -> None:
        """Processes a finding, forwarding only if not a duplicate."""
        issue = await self.ISSUE_TYPE.load(path)
        duplicate_issue_id = await self._is_duplicate(issue)
        if duplicate_issue_id is None:
            await self._forward_one(path)
            return

        assert self._duplicates_dir is not None
        issue_id_str = (
            str(duplicate_issue_id).replace('/', '_').replace('~', '')
        )
        timestamp = datetime.now().strftime('%Y%m%d-%H%M%S')
        dest_path = (
            self._duplicates_dir
            / f'b{issue_id_str}-{timestamp}{path.path.suffix}'
        )
        await path.rename(dest_path)
        logging.info(
            'Found duplicate for %s: b/%s', path.name, duplicate_issue_id
        )

    async def _is_duplicate(self, issue: Issue) -> int | None:
        """Checks if finding is a duplicate of a previously seen defect."""
        raise NotImplementedError


################################################################################
# Test support


class DeduplicatorStub(Deduplicator):
    """A stub implementation of Deduplicator for testing."""

    ISSUE_TYPE: type[Issue] = Issue

    def __init__(self, is_duplicate_val: bool = False) -> None:
        """Initializes stub with configurable duplicate check return value.

        Args:
            is_duplicate_val: If True, duplicate check returns stubbed ID.
        """
        super().__init__()
        self.is_duplicate_val = is_duplicate_val
        self.checked_issues: list[Issue] = []

    async def _is_duplicate(self, issue: Issue) -> int | None:
        """Records the issue and returns the stubbed duplicate check value."""
        self.checked_issues.append(issue)
        if self.is_duplicate_val:
            return issue.issue_id or 12345
        return None
