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
"""Defines IssueReader base class for reading issue reports."""

import argparse
import logging
from abc import ABC, abstractmethod
from typing import AsyncIterator

from pw_fortifier.async_path import AsyncPath
from pw_fortifier.issue import Issue
from pw_fortifier.pipeline_stage import PipelineProducerStage, PipelineStage


class IssueTracker(ABC):
    """Abstract base class for interacting with an issue tracker."""

    ISSUE_TYPE: type[Issue] = Issue

    def __init__(self) -> None:
        self._component_id: int = 0
        self._ccs: list[str] = []
        self._primary_hotlist_id: int | None = None
        self._extra_hotlist_ids: list[int] = []
        self._default_assignee: str | None = None

    @property
    def component_id(self) -> int:
        """The Buganizer component ID."""
        return self._component_id

    @component_id.setter
    def component_id(self, component_id: int) -> None:
        self._component_id = component_id

    @property
    def default_assignee(self) -> str | None:
        """Default assignee email address if no owner is found."""
        return self._default_assignee

    @default_assignee.setter
    def default_assignee(self, default_assignee: str | None) -> None:
        self._default_assignee = default_assignee

    @property
    def ccs(self) -> list[str]:
        """List of default CC email addresses."""
        return self._ccs

    @ccs.setter
    def ccs(self, ccs: list[str]) -> None:
        self._ccs = ccs

    @property
    def primary_hotlist_id(self) -> int | None:
        """Primary hotlist ID for deduplication and tracking."""
        return self._primary_hotlist_id

    @primary_hotlist_id.setter
    def primary_hotlist_id(self, hotlist_id: int | None) -> None:
        self._primary_hotlist_id = hotlist_id

    @property
    def extra_hotlist_ids(self) -> list[int]:
        """Additional hotlist IDs attached upon filing."""
        return self._extra_hotlist_ids

    @extra_hotlist_ids.setter
    def extra_hotlist_ids(self, hotlist_ids: list[int]) -> None:
        self._extra_hotlist_ids = hotlist_ids

    @property
    def hotlist_ids(self) -> list[int]:
        """All configured hotlist IDs (primary + extra)."""
        hotlists = []
        if (
            self._primary_hotlist_id is not None
            and self._primary_hotlist_id != 0
        ):
            hotlists.append(self._primary_hotlist_id)
        hotlists.extend(self._extra_hotlist_ids)
        return hotlists

    @abstractmethod
    async def create(self, issue: Issue) -> Issue:
        """Creates a new issue report in Buganizer.

        Args:
            issue: The Issue finding to file.

        Returns:
            The created Issue with populated issue ID.
        """
        raise NotImplementedError

    @abstractmethod
    async def read(self, issue_id: int) -> Issue:
        """Reads an issue by ID from Buganizer.

        Args:
            issue_id: The ID of the issue to read.

        Returns:
            The loaded Issue.
        """
        raise NotImplementedError

    @abstractmethod
    def read_hotlist(self, hotlist_id: int) -> AsyncIterator[Issue]:
        """Yields issues associated with a Buganizer hotlist ID.

        Args:
            hotlist_id: The hotlist ID to query.

        Yields:
            Issue instances matching the hotlist query.
        """
        raise NotImplementedError


class IssueReader(PipelineProducerStage):
    """Base class for stages that read issue reports from an issue tracker."""

    ISSUE_TYPE: type[Issue] = Issue

    def __init__(self, issue_tracker: IssueTracker) -> None:
        super().__init__()
        self._issue_tracker = issue_tracker
        self._issue_ids: list[int] = []
        self._hotlist_ids: list[int] = []

    async def configure(self, args: argparse.Namespace) -> None:
        """Configures the stage with issue IDs and hotlist IDs from args.

        Args:
            args: Command-line arguments namespace object.
        """
        await super().configure(args)
        self._issue_ids = args.issues
        self._hotlist_ids = args.hotlists
        args.issues = []
        args.hotlists = []

    async def _produce_all(self) -> None:
        """Runs the reader by fetching configured issues and hotlists."""
        for issue_id in self._issue_ids:
            issue = await self._issue_tracker.read(issue_id)
            await self._send_issue(issue)

        for hotlist_id in self._hotlist_ids:
            async for issue in self._issue_tracker.read_hotlist(hotlist_id):
                await self._send_issue(issue)

    async def _send_issue(self, issue: Issue) -> None:
        """Saves an issue to be processed locally."""
        converted = self.ISSUE_TYPE.from_issue(issue)
        assert converted is not None
        assert self._out_dir is not None
        out_path = AsyncPath(self._out_dir, f'b{converted.issue_id}.json')
        await converted.save(out_path)
        self.send(out_path)


class IssueWriter(PipelineStage):
    """Base class for stages that write issue reports."""

    def __init__(self, issue_tracker: IssueTracker) -> None:
        super().__init__()
        self._issue_tracker = issue_tracker
        self._create_bugs = False

    async def configure(self, args: argparse.Namespace) -> None:
        """Configures the stage.

        Args:
            args: Command-line arguments namespace object.
        """
        await super().configure(args)
        self._create_bugs = bool(args.create_bugs)

    async def _process_one(self, path: AsyncPath) -> None:
        """Writes issue reports to Buganizer."""
        issue = await self._load(path)
        if not issue:
            raise ValueError(f"Failed to load issue from '{path}'")
        if (
            issue.assignee is None
            and self._issue_tracker.default_assignee is not None
        ):
            issue.assignee = self._issue_tracker.default_assignee

        if self._verbose:
            print(f'Title: {issue.title}')
            print(f'Description: {issue.description}')

        if self._create_bugs:
            issue = await self._issue_tracker.create(issue)
        else:
            logging.info('DRY RUN: Filing bug for issue: %s', issue.title)

        await issue.save(path)
        await self._forward_one(path)

    async def _load(self, path: AsyncPath) -> Issue:
        raise NotImplementedError


################################################################################
# Test support


class IssueTrackerStub(IssueTracker):
    """In-memory stub implementation of IssueTracker for testing."""

    def __init__(self) -> None:
        super().__init__()
        self.next_issue_id = 8675309
        self.issues: dict[int, Issue] = {}
        self.issue_hotlists: dict[int, list[int]] = {}

    def add_issue(
        self, issue: Issue, hotlist_ids: list[int] | None = None
    ) -> None:
        """Adds an existing issue to the in-memory tracker.

        Args:
            issue: The issue to store.
            hotlist_ids: Optional list of hotlist IDs associated with the issue.
        """
        assert issue.issue_id is not None
        self.issues[issue.issue_id] = issue
        if hotlist_ids is not None:
            self.issue_hotlists[issue.issue_id] = list(hotlist_ids)
        elif issue.issue_id not in self.issue_hotlists:
            self.issue_hotlists[issue.issue_id] = []

    async def create(self, issue: Issue) -> Issue:
        """Creates and stores a new issue with an auto-incremented ID.

        Args:
            issue: The issue to create.

        Returns:
            The created issue with assigned ID.
        """
        issue = issue._replace(issue_id=self.next_issue_id)
        self.next_issue_id += 1
        self.add_issue(issue, hotlist_ids=self.hotlist_ids)
        return issue

    async def read(self, issue_id: int) -> Issue:
        """Reads an issue from the in-memory store.

        Args:
            issue_id: ID of the issue to read.

        Returns:
            The retrieved Issue instance.
        """
        return self.issues[issue_id]

    # mypy/pylint PEP 525 incompatibility
    # pylint: disable=invalid-overridden-method
    async def read_hotlist(self, hotlist_id: int) -> AsyncIterator[Issue]:
        """Yields issues from the in-memory store matching the hotlist ID.

        Args:
            hotlist_id: Hotlist ID to search for.

        Yields:
            Matching Issue instances.
        """
        for issue_id, issue in self.issues.items():
            if hotlist_id in self.issue_hotlists.get(issue_id, []):
                yield issue
