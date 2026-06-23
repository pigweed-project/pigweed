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

import json
import os
from typing import Iterator
import requests

from pw_fortifier.bazel_scanner import BazelScanner
from pw_fortifier.git_repo_scanner import GitRepoScanner
from pw_fortifier.package_scanner import (
    FreshnessScanResult,
    PackageVersion,
    TIER1_TOOLCHAIN,
    TIER2_DEVHOST,
    TIER3_UPSTREAM,
)


class BazelDepScanner(BazelScanner):
    """Scans Bazel dependencies in MODULE.bazel for freshness."""

    def _get_dependencies(
        self, cwd: str | os.PathLike[str]
    ) -> tuple[set[str], list[dict[str, str]]] | None:
        """Determines prod modules and all dependencies."""
        # 1. Build "prod modules" set
        try:
            result = self._run_bazelisk(
                ['mod', 'graph', '--ignore_dev_dependency', '--output=json'],
                cwd,
            )
            if result.returncode != 0:
                return None
            prod_data = json.loads(result.stdout)
        except (RuntimeError, json.JSONDecodeError):
            return None

        prod_dependencies = prod_data.get('dependencies', [])
        prod_modules = {
            dep['name'] for dep in prod_dependencies if 'name' in dep
        }

        # 2. Run without ignore_dev_dependency
        try:
            result = self._run_bazelisk(['mod', 'graph', '--output=json'], cwd)
            if result.returncode != 0:
                return None
            all_data = json.loads(result.stdout)
        except (RuntimeError, json.JSONDecodeError):
            return None

        all_dependencies = all_data.get('dependencies', [])
        if not all_dependencies:
            return None

        return prod_modules, all_dependencies

    def _scan_dependency(
        self,
        dep: dict[str, str],
        prod_modules: set[str],
        bcr_repo: GitRepoScanner,
        module_bazel_path: str | os.PathLike[str],
        rel_path: str,
    ) -> FreshnessScanResult | None:
        """Scans a single dependency for freshness."""
        module = dep.get('name')
        current_ver = dep.get('version')
        if not module or not current_ver:
            return None

        key = f'{module}:{current_ver}'
        if key in self._scanned_packages:
            return None
        self._scanned_packages.add(key)

        # Determine Tier
        if module in prod_modules:
            if 'tool' not in module and 'test' not in module:
                tier = TIER1_TOOLCHAIN
            else:
                tier = TIER2_DEVHOST
        else:
            tier = TIER3_UPSTREAM

        # Query BCR for metadata
        bcr_url = f'https://bcr.bazel.build/modules/{module}/metadata.json'
        try:
            response = requests.get(bcr_url, timeout=10)
            if response.status_code != 200:
                return None
            bcr_data = response.json()
        except (requests.RequestException, ValueError):
            return None

        versions = bcr_data.get('versions', [])

        current_versions = list(
            bcr_repo.get_versions(
                num=1, scope=f'modules/{module}/{current_ver}'
            )
        )
        if not current_versions:
            return None
        current_date = current_versions[0].timestamp

        current = PackageVersion(version=current_ver, timestamp=current_date)

        all_versions = []
        for v in versions:
            v_versions = list(
                bcr_repo.get_versions(num=1, scope=f'modules/{module}/{v}')
            )
            if v_versions:
                all_versions.append(
                    PackageVersion(version=v, timestamp=v_versions[0].timestamp)
                )

        if not all_versions:
            return None

        earliest = self.find_lowest(tier, current, all_versions)

        # Find owner
        owner = self._find_owners(module_bazel_path, 'bazel_dep', module)

        return FreshnessScanResult(
            package=module,
            source=rel_path,
            pkg_type='bazel_dep',
            current=current,
            earliest=earliest,
            tier=tier,
            owner=owner,
        )

    def pre_scan(self) -> Iterator[FreshnessScanResult]:
        """Examines Bazel dependencies for freshness."""
        module_bazel_path = os.path.join(self.root, 'MODULE.bazel')
        cwd = os.path.dirname(module_bazel_path)
        rel_path = 'MODULE.bazel'

        deps = self._get_dependencies(cwd)
        if not deps:
            return
        prod_modules, all_dependencies = deps

        try:
            bcr_repo = GitRepoScanner.clone(
                'https://github.com/bazelbuild/bazel-central-registry.git'
            )
        except RuntimeError:
            return

        for dep in all_dependencies:
            res = self._scan_dependency(
                dep, prod_modules, bcr_repo, module_bazel_path, rel_path
            )
            if res:
                yield res
