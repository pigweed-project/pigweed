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
"""Defines the PackageUpdater base class and stub."""

from abc import ABC, abstractmethod

from pw_fortifier.freshness_result import FreshnessResult
from pw_fortifier.git_utils import WritableGitWorkspace


class PackageUpdater(ABC):
    """Abstract base class for package-type specific roll updaters."""

    PKG_TYPE: str | None = None
    TARGET: str | None = None

    @abstractmethod
    async def update(
        self,
        dst_repo: WritableGitWorkspace,
        result: FreshnessResult,
    ) -> bool:
        """Updates a dependency in dst_repo based on freshness result.

        Args:
            dst_repo: The writable git workspace to update.
            result: The freshness result finding.

        Returns:
            True if update succeeded and passed presubmit; False otherwise.
        """
        raise NotImplementedError


################################################################################
# Test support


class PackageUpdaterStub(PackageUpdater):
    """Stub package updater for testing."""

    PKG_TYPE: str | None = 'fake'
    TARGET: str | None = None

    def __init__(
        self,
        update_return_value: bool = True,
    ) -> None:
        self.update_return_value = update_return_value
        self.updated_results: list[
            tuple[WritableGitWorkspace, FreshnessResult]
        ] = []

    async def update(
        self,
        dst_repo: WritableGitWorkspace,
        result: FreshnessResult,
    ) -> bool:
        """Simulates package update."""
        self.updated_results.append((dst_repo, result))
        return self.update_return_value
