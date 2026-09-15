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
"""Defines issue tracker adapter classes for defect issues."""

from abc import ABC, abstractmethod

from pw_fortifier.async_path import AsyncPath
from pw_fortifier.defect import Defect
from pw_fortifier.issue import Issue
from pw_fortifier.issue_tracker import IssueReader, IssueTracker, IssueWriter


class DefectSummarizer(ABC):
    """Abstract base class for defect summaries and vulnerability codes."""

    @abstractmethod
    def summarize(self, defect: Defect) -> tuple[str, str]:
        """Returns a vulnerability code and brief summary for a defect.

        Args:
            defect: The defect finding to summarize.

        Returns:
            A tuple of (vuln_code, summary).
        """
        raise NotImplementedError


class DefectReader(IssueReader):
    """Stage that reads defect reports from an issue tracker."""

    ISSUE_TYPE: type[Issue] = Defect


class DefectWriter(IssueWriter):
    """Stage that writes defect reports to an issue tracker."""

    def __init__(
        self, issue_tracker: IssueTracker, summarizer: DefectSummarizer
    ) -> None:
        """Initializes the defect writer.

        Args:
            issue_tracker: Issue tracker client used to write issues.
            summarizer: DefectSummarizer used to format defect titles.
        """
        super().__init__(issue_tracker)
        self._summarizer = summarizer

    async def _load(self, path: AsyncPath) -> Issue:
        """Loads a defect report from disk and populates its title.

        Args:
            path: Path to the defect JSON file.

        Returns:
            The loaded Defect issue instance.
        """
        defect = await Defect.load(path)
        defect.title = self.make_title(defect)
        return defect

    def make_title(self, defect: Defect) -> str:
        """Generates a title string for a defect.

        Args:
            defect: The defect finding to create a title for.

        Returns:
            Formatted title string.
        """
        vuln_code, summary = self._summarizer.summarize(defect)
        return f'{vuln_code} - {defect.filename}: {summary}'
