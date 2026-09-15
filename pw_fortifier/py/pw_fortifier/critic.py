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
"""Defines the Critic base class for validating defect findings."""

from pw_fortifier.async_path import AsyncPath
from pw_fortifier.defect import Defect
from pw_fortifier.pipeline_stage import PipelineStage


class Critic(PipelineStage):
    """Base class for stages that validate and challenge defect findings."""

    async def _process_one(self, path: AsyncPath) -> None:
        """Processes a single finding, forwarding it only if it is validated."""
        issue = await Defect.load(path)
        validated_issue = await self._criticize(issue)
        if validated_issue is not None:
            await validated_issue.save(path)
            await self._forward_one(path)

    async def _criticize(self, issue: Defect) -> Defect | None:
        """Challenges and validates a single defect finding."""
        raise NotImplementedError


################################################################################
# Test support


class CriticStub(Critic):
    """A stub implementation of Critic for testing."""

    def __init__(self, criticize_val: bool = True) -> None:
        """Initializes the stub with a configurable validation return value.

        Args:
            criticize_val: If True, validate and return defect.
        """
        super().__init__()
        self.criticize_val = criticize_val
        self.checked_defects: list[Defect] = []

    async def _criticize(self, issue: Defect) -> Defect | None:
        """Records the issue and returns the stubbed validation value."""
        self.checked_defects.append(issue)
        return issue if self.criticize_val else None
