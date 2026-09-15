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
"""Defines the DefectScanner for the pw_fortifier pipeline."""

from pw_fortifier.async_path import AsyncPath
from pw_fortifier.code_analyzer import CodeAnalyzer
from pw_fortifier.collector import Collector
from pw_fortifier.critic import Critic
from pw_fortifier.deduplicator import Deduplicator
from pw_fortifier.defect import Defect
from pw_fortifier.defect_tracker import (
    DefectReader,
    DefectSummarizer,
    DefectWriter,
)
from pw_fortifier.issue_tracker import IssueTracker
from pw_fortifier.pipeline_stage import (
    PipelineConsumerMixin,
    PipelineProducerMixin,
    PipelineStage,
)
from pw_fortifier.scanner import Scanner
from pw_fortifier.triager import Triager


class DefectCollector(Collector):
    """Collector implementation that formats and prints defects."""

    def __init__(self) -> None:
        """Initializes the defect collector columns."""
        super().__init__()
        self._add_field('Issue ID', 9)
        self._add_field('Severity')
        self._add_field('CL', 6)
        self._add_field('Title', -1)

    async def _print_item(self, path: AsyncPath) -> None:
        """Prints details of a single defect finding."""
        defect = await Defect.load(path)
        id_str = str(defect.issue_id) if defect.issue_id is not None else ''
        sev_str = f'S{defect.severity}' if defect.severity is not None else ''
        cl_str = str(defect.cl_num) if defect.cl_num is not None else ''
        title_str = defect.title if defect.title is not None else ''
        self._report_fields(id_str, sev_str, cl_str, title_str)


class DefectScanner(Scanner):
    """DefectScanner that parses command-line arguments and runs pipeline."""

    DESC = 'Scans for security-related defects in a repository.'

    def __init__(self, name: str) -> None:
        """Initializes the DefectScanner.

        Args:
            name: Program name string.
        """
        super().__init__(name)
        self.code_analyzer: CodeAnalyzer | None = None
        self.critic: Critic | None = None
        self._issue_tracker: IssueTracker | None = None
        self.summarizer: DefectSummarizer | None = None
        self._collector = DefectCollector()

    @property
    def deduplicator(self) -> Deduplicator:
        """The deduplicator pipeline stage."""
        assert self._deduplicator is not None
        assert isinstance(self._deduplicator, Deduplicator)
        return self._deduplicator

    @deduplicator.setter
    def deduplicator(self, deduplicator: Deduplicator) -> None:
        if self._issue_tracker is not None:
            deduplicator.issue_tracker = self._issue_tracker
        self._deduplicator = deduplicator

    @property
    def triager(self) -> Triager:
        """The triager pipeline stage."""
        assert self._triager is not None
        assert isinstance(self._triager, Triager)
        return self._triager

    @triager.setter
    def triager(self, triager: Triager) -> None:
        self._triager = triager

    @property
    def issue_tracker(self) -> IssueTracker:
        """The issue tracker client."""
        assert self._issue_tracker is not None
        return self._issue_tracker

    @issue_tracker.setter
    def issue_tracker(self, issue_tracker: IssueTracker) -> None:
        if self._deduplicator is not None:
            self._deduplicator.issue_tracker = issue_tracker

        assert self.summarizer is not None
        self._issue_writer = DefectWriter(issue_tracker, self.summarizer)
        self._issue_reader = DefectReader(issue_tracker)
        self._issue_tracker = issue_tracker

    def _add_generator_stages(
        self,
    ) -> tuple[PipelineConsumerMixin, PipelineProducerMixin]:
        """Instantiates pipeline stages used to create issues."""
        assert self.code_analyzer is not None
        self._add_stage(self.code_analyzer)

        assert self.critic is not None
        self._add_stage(self.critic)
        self.code_analyzer.connect(self.critic)

        return (self.code_analyzer, self.critic)

    @property
    def code_generator(self) -> PipelineStage:
        """The code generator pipeline stage."""
        assert self._code_generator is not None
        return self._code_generator

    @code_generator.setter
    def code_generator(self, code_generator: PipelineStage) -> None:
        self._code_generator = code_generator
