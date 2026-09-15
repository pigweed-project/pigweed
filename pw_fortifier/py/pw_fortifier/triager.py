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
"""Defines the Triager base class for assessing issue severity."""

from dataclasses import asdict

from pw_fortifier.async_path import AsyncPath
from pw_fortifier.find_core_owners import CoreOwnerFinder
from pw_fortifier.issue import Issue
from pw_fortifier.pipeline_stage import PipelineStage


class Triager(PipelineStage):
    """Base class for stages that assess the severity and details of issues."""

    ISSUE_TYPE: type[Issue] = Issue

    async def _process_one(self, path: AsyncPath) -> None:
        """Processes finding, triaging in place and forwarding result."""
        issue = await self.ISSUE_TYPE.load(path)
        if (
            issue.assignee is None
            and issue.location is not None
            and self.src_repo is not None
        ):
            finder = CoreOwnerFinder(self.src_repo)
            finder.add(issue.location)
            issue.assignee = finder.find()
        await self._triage(issue)
        await issue.save(path)
        await self._forward_one(path)

    async def _triage(self, issue: Issue) -> None:
        """Assesses severity and details of finding, modifying in place."""
        raise NotImplementedError


################################################################################
# Test support


class TriagerStub(Triager):
    """A stub implementation of Triager for testing."""

    ISSUE_TYPE: type[Issue] = Issue

    def __init__(self, stub_issue: Issue) -> None:
        """Initializes the stub with a mock Issue to apply.

        Args:
            stub_issue: Mock Issue instance containing values to apply.
        """
        super().__init__()
        self.stub_issue = stub_issue
        self.triaged_issues: list[Issue] = []

    async def _triage(self, issue: Issue) -> None:
        """Records the issue and modifies it in place with the stub issue."""
        self.triaged_issues.append(issue)
        for k, v in asdict(self.stub_issue).items():
            setattr(issue, k, v)
