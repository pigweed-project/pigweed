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
"""Defines DemoIssueTracker for local issue tracking and demonstrations."""

import argparse
import asyncio
from collections.abc import Sequence
from pathlib import Path
import random
import tempfile
from typing import AsyncIterator

from pw_fortifier.async_path import AsyncPath
from pw_fortifier.issue import Issue
from pw_fortifier.issue_tracker import IssueTracker


class DemoIssueTracker(IssueTracker):
    """Local filesystem-backed issue tracker implementation for demos."""

    def __init__(self, working_dir: AsyncPath = AsyncPath()) -> None:
        """Initializes DemoIssueTracker.

        Args:
            working_dir: Working directory under which issue files are saved.
        """
        super().__init__()
        self.component_id = 1337
        self.ccs = ['fake1@fake.fake', 'fake2@fake.fake']
        self.primary_hotlist_id = 8675309
        self.extra_hotlist_ids = [1000000, 2000000, 3000000]
        self.default_assignee = 'rotation@fake.fake'

        self._issues: AsyncPath = working_dir / 'b'
        self._next_issue_id: int = 123456789

    async def create(self, issue: Issue) -> Issue:
        """Creates a new issue and writes its fields to a JSON file.

        Args:
            issue: The Issue finding to file.

        Returns:
            The created Issue with populated issue ID.
        """
        await self._issues.mkdir(parents=True, exist_ok=True)
        issue = issue._replace(issue_id=self._next_issue_id)
        await issue.save(self._issues / str(issue.issue_id))

        self._next_issue_id += random.randint(100, 1000)
        return issue

    async def read(self, issue_id: int) -> Issue:
        """Reads an issue from its JSON file by ID.

        Args:
            issue_id: The ID of the issue to read.

        Returns:
            The loaded Issue instance.
        """
        issue_path = self._issues / str(issue_id)
        assert (
            await issue_path.is_file()
        ), f'Issue file missing for ID {issue_id}: {issue_path}'
        return await Issue.load(issue_path)

    # mypy/pylint PEP 525 incompatibility
    # pylint: disable=invalid-overridden-method
    async def read_hotlist(self, hotlist_id: int) -> AsyncIterator[Issue]:
        """Yields issues associated with the given hotlist ID.

        Args:
            hotlist_id: The hotlist ID to query.

        Yields:
            Issue instances matching the hotlist query.
        """
        if hotlist_id not in self.hotlist_ids:
            return

        if not await self._issues.is_dir():
            return

        async for issue_file in self._issues.iterdir():
            if await issue_file.is_file():
                issue = await Issue.load(issue_file)
                yield issue

    @staticmethod
    def parse_args(args: Sequence[str] | None = None) -> argparse.Namespace:
        """Parses command line arguments.

        Args:
            args: Optional sequence of command line arguments.

        Returns:
            Parsed arguments namespace.
        """
        parser = argparse.ArgumentParser(
            description='Demo issue tracker for inspecting issues.'
        )
        default_working_dir = str(
            Path(tempfile.gettempdir(), 'demo_freshness_scanner')
        )
        parser.add_argument(
            '-w',
            '--working-dir',
            type=str,
            default=default_working_dir,
            help=(
                'Working directory for issue storage '
                f'(defaults to {default_working_dir}).'
            ),
        )
        parser.add_argument(
            '-i',
            '--issue',
            type=int,
            default=None,
            help='Issue ID to display.',
        )
        return parser.parse_args(args)

    def list_issues(self) -> None:
        """Enumerates and prints all the files in self._issues."""
        if not self._issues.path.is_dir():
            return
        for f in sorted(self._issues.path.iterdir()):
            if f.is_file():
                print(f.name)

    async def display_issue(self, issue_id: int) -> None:
        """Reads and pretty-prints the issue with the given ID.

        Args:
            issue_id: The ID of the issue to read and display.
        """
        issue_path = self._issues / str(issue_id)
        assert (
            await issue_path.is_file()
        ), f'Issue file missing for ID {issue_id}: {issue_path}'
        content = await issue_path.read_text()
        print(content)


def main(argv: Sequence[str] | None = None) -> None:
    """Main CLI entry point for DemoIssueTracker."""
    args = DemoIssueTracker.parse_args(argv)
    tracker = DemoIssueTracker(AsyncPath(args.working_dir))
    if args.issue is not None:
        asyncio.run(tracker.display_issue(args.issue))
    else:
        tracker.list_issues()


if __name__ == '__main__':
    main()
