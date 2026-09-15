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
"""Defines the FreshnessScanner for orchestrating package freshness scans."""

import argparse

from pw_fortifier.freshness_collector import FreshnessResultCollector
from pw_fortifier.freshness_deduplicator import FreshnessDeduplicator
from pw_fortifier.freshness_result_tracker import (
    FreshnessResultReader,
    FreshnessResultWriter,
)
from pw_fortifier.freshness_triager import FreshnessTriager
from pw_fortifier.issue_tracker import IssueTracker
from pw_fortifier.package_analyzer import PackageAnalyzer
from pw_fortifier.pipeline_stage import (
    PipelineConsumerMixin,
    PipelineDemux,
    PipelineMux,
    PipelineProducerMixin,
    PipelineStage,
)
from pw_fortifier.package_updater import PackageUpdater
from pw_fortifier.roll_generator import RollGenerator
from pw_fortifier.scanner import Scanner


class FreshnessScanner(Scanner):
    """Scanner that orchestrates third-party package freshness scans."""

    DESC = (
        'Scans third-party packages for freshness according to '
        'go/pigweed-3p-freshness'
    )

    def __init__(self, name: str) -> None:
        """Initializes the FreshnessScanner with mux and demux stages.

        Args:
            name: Program name string.
        """
        super().__init__(name)
        self._mux = PipelineMux()
        self._demux = PipelineDemux()
        self._issue_tracker: IssueTracker | None = None
        self._deduplicator = FreshnessDeduplicator()
        self._triager = FreshnessTriager()
        self._code_generator = RollGenerator()
        self._collector = FreshnessResultCollector()

    @property
    def issue_tracker(self) -> IssueTracker:
        """The issue tracker client."""
        assert self._issue_tracker is not None
        return self._issue_tracker

    @issue_tracker.setter
    def issue_tracker(self, issue_tracker: IssueTracker) -> None:
        assert self._deduplicator is not None
        assert isinstance(self._deduplicator, FreshnessDeduplicator)
        self._deduplicator.issue_tracker = issue_tracker
        self._issue_writer = FreshnessResultWriter(issue_tracker)
        self._issue_reader = FreshnessResultReader(issue_tracker)
        self._issue_tracker = issue_tracker

    def register(
        self,
        analyzer: PackageAnalyzer,
        updater: PackageUpdater | None = None,
    ) -> None:
        """Registers the given analyzer and optional package updater.

        Args:
            analyzer: Produces zero or more freshness scan results.
            updater: Optional PackageUpdater to register with RollGenerator.
        """
        self._add_stage(analyzer)
        self._demux.add_stage(analyzer)

        target = type(analyzer).TARGET
        if target is not None:
            self._mux.add_stage(target, analyzer)

        if updater is not None:
            assert isinstance(self._code_generator, RollGenerator)
            self._code_generator.register(updater)

    def _add_generator_stages(
        self,
    ) -> tuple[PipelineConsumerMixin, PipelineProducerMixin]:
        """Instantiates pipeline stages used to create issues."""
        self._add_stage(self._mux)
        self._add_stage(self._demux)
        return (self._mux, self._demux)

    async def _configure(self, args: argparse.Namespace) -> None:
        """Configures the scanner and matches analyzer targets."""
        await super()._configure(args)
        assert self._emitter.enumerator is not None
        for target in self._mux.targets:
            self._emitter.enumerator.match(target)
            self._emitter.enumerator.match(f'*/{target}')

    @property
    def code_generator(self) -> PipelineStage:
        """The code generator pipeline stage."""
        assert self._code_generator is not None
        return self._code_generator

    @code_generator.setter
    def code_generator(self, code_generator: PipelineStage) -> None:
        self._code_generator = code_generator
