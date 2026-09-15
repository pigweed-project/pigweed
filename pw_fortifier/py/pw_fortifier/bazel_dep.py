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
"""Package scanner for Bazel dependencies."""

import argparse
import asyncio
import json
import os
import requests

from pw_fortifier.async_path import AsyncPath
from pw_fortifier.bazelisk_utils import run_bazelisk
from pw_fortifier.git_utils import ReadOnlyGitWorkspace
from pw_fortifier.code_snippet import find_location
from pw_fortifier.package_analyzer import PackageAnalyzer
from pw_fortifier.freshness_result import (
    FreshnessResult,
    PackageVersion,
    TIER0_ON_DEVICE,
    TIER1_TOOLCHAIN,
    TIER2_DEVHOST,
    TIER3_UPSTREAM,
)


BCR_REPO_URL = 'https://github.com/bazelbuild/bazel-central-registry.git'


async def _get_dependencies(
    cwd: str | os.PathLike[str],
) -> tuple[set[str], list[dict[str, str]]]:
    """Determines prod modules and all dependencies."""
    # 1. Build "prod modules" set
    result = await run_bazelisk(
        [
            'mod',
            'graph',
            '--lockfile_mode=off',
            '--ignore_dev_dependency',
            '--output=json',
        ],
        cwd,
    )
    prod_data = json.loads(result.stdout)

    prod_dependencies = prod_data.get('dependencies', [])
    prod_modules = {dep['name'] for dep in prod_dependencies if 'name' in dep}

    # 2. Run without ignore_dev_dependency
    result = await run_bazelisk(
        ['mod', 'graph', '--lockfile_mode=off', '--output=json'], cwd
    )
    all_data = json.loads(result.stdout)

    all_dependencies = all_data.get('dependencies', [])

    return prod_modules, all_dependencies


class BazelDepAnalyzer(PackageAnalyzer):
    """Scans Bazel dependencies in MODULE.bazel for freshness."""

    PKG_TYPE: str = 'bazel_dep'

    def __init__(self) -> None:
        super().__init__()
        self.loose_semver = True

    async def configure(self, args: argparse.Namespace) -> None:
        """Configures the analyzer and determines if setup can be skipped."""
        await super().configure(args)
        if args.files is None:
            return
        if 'MODULE.bazel' in args.files:
            return
        self.skip_setup = True

    async def _scan_dependency(
        self,
        dep: dict[str, str],
        prod_modules: set[str],
        bcr_repo: ReadOnlyGitWorkspace,
        rel_path: str,
    ) -> FreshnessResult | None:
        """Scans a single dependency for freshness."""
        module = dep.get('name')
        current_ver = dep.get('version')
        if not module or not current_ver:
            return None

        # Check against previously scanned modules.
        key = f'{module}:{current_ver}'
        if key in self._scanned_packages:
            return None
        self._scanned_packages.add(key)

        # Determine Tier
        if module not in prod_modules:
            tier = TIER3_UPSTREAM
        elif 'tool' in module or 'test' in module:
            tier = TIER2_DEVHOST
        elif 'rules' in module:
            tier = TIER1_TOOLCHAIN
        else:
            tier = TIER0_ON_DEVICE

        # Query BCR for metadata
        bcr_url = f'https://bcr.bazel.build/modules/{module}/metadata.json'
        loop = asyncio.get_running_loop()
        response = await loop.run_in_executor(
            None,
            lambda: requests.get(bcr_url, timeout=10),
        )

        # Some 'modules' in MODULE.bazel are placeholders to allow overrides.
        if response.status_code == 404:
            return None

        if response.status_code != 200:
            response.raise_for_status()
            raise requests.exceptions.HTTPError(
                f'BCR request for {module} returned status '
                f'{response.status_code}',
                response=response,
            )
        bcr_data = response.json()

        versions = bcr_data.get('versions', [])

        # Get the most recent commit that modified the current version.
        current_versions = [
            PackageVersion(v, t)
            for v, t in bcr_repo.get_versions(
                num=1, scope=f'modules/{module}/{current_ver}'
            )
        ]
        if not current_versions:
            return None
        current_date = current_versions[0].timestamp

        current = PackageVersion(version=current_ver, timestamp=current_date)

        # For each version, get the most recent commit that modified it.
        all_versions = []
        for v in versions:
            v_versions = [
                PackageVersion(ver, t)
                for ver, t in bcr_repo.get_versions(
                    num=1, scope=f'modules/{module}/{v}'
                )
            ]
            if v_versions:
                all_versions.append(
                    PackageVersion(version=v, timestamp=v_versions[0].timestamp)
                )

        if not all_versions:
            return None

        # Find the earliest valid version.
        earliest = self.find_lowest(tier, current, all_versions)

        # Locate where the current version is specified
        location = find_location(self.root, rel_path, self.PKG_TYPE, module)

        return FreshnessResult(
            package=module,
            location=location,
            pkg_type=self.PKG_TYPE,
            current=current,
            earliest=earliest,
            tier=tier,
        )

    async def _set_up(self) -> None:
        """Examines Bazel dependencies for freshness."""
        module_bazel_path = os.path.join(self.root, 'MODULE.bazel')
        cwd = os.path.dirname(module_bazel_path)
        rel_path = 'MODULE.bazel'

        prod_modules, all_dependencies = await _get_dependencies(cwd)

        bcr_repo = await ReadOnlyGitWorkspace.clone(
            BCR_REPO_URL,
            no_checkout=True,
            git_filter='blob:none',
            depth=None,
        )

        for dep in all_dependencies:
            res = await self._scan_dependency(
                dep, prod_modules, bcr_repo, rel_path
            )
            await self._send_result(res)

        self.input_queue.put_nowait(None)

    async def _process_one(self, path: AsyncPath) -> None:
        pass
