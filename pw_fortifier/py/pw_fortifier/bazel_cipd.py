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
"""Package scanner for CIPD repositories in Bazel."""

import argparse
import os

from pw_fortifier.async_path import AsyncPath
from pw_fortifier.bazelisk_utils import BazelRepo
from pw_fortifier.cipd_utils import CipdPackageSet
from pw_fortifier.freshness_result import TIER1_TOOLCHAIN
from pw_fortifier.package_analyzer import PackageAnalyzer


PKG_TYPE: str = 'bazel_cipd'


class BazelCipdAnalyzer(PackageAnalyzer):
    """Scans CIPD repositories in MODULE.bazel for freshness."""

    PKG_TYPE = PKG_TYPE

    def __init__(self) -> None:
        super().__init__()
        self.loose_semver = True
        self._cipd_pkgs: CipdPackageSet | None = None

    async def configure(self, args: argparse.Namespace) -> None:
        """Configures the analyzer and determines if setup can be skipped."""
        await super().configure(args)
        if args.files is None:
            return
        if 'MODULE.bazel' in args.files:
            return
        self.skip_setup = True

    async def _add_cipd_repository(self, repo: BazelRepo) -> None:
        """Processes cipd_repository attributes and updates _packages_dict."""
        pkg = repo.get_attr_str('path')
        if not pkg:
            pkg = repo.get_attr_str('package')
        assert pkg

        version = repo.get_attr_str('tag')
        assert version

        assert self._cipd_pkgs is not None
        await self._cipd_pkgs.add(
            pkg, version, TIER1_TOOLCHAIN, location=repo.location
        )

    async def _add_package_repo(self, repo: BazelRepo) -> None:
        """Processes package_repo attributes and updates _packages_dict."""
        packages = repo.get_attr_str_dict('packages')
        assert self._cipd_pkgs is not None
        for pkg, version in packages.items():
            await self._cipd_pkgs.add(
                pkg, version, TIER1_TOOLCHAIN, location=repo.location
            )

    async def _set_up(self) -> None:
        """Examines CIPD repositories for freshness."""
        rel_path = 'MODULE.bazel'
        self._cipd_pkgs = CipdPackageSet(rel_path, self.PKG_TYPE)

        module_bazel_path = os.path.join(self.root, rel_path)
        async for repo in BazelRepo.load(module_bazel_path):
            if repo.rule_name == 'cipd_repository':
                await self._add_cipd_repository(repo)
            elif repo.rule_name == 'package_repo':
                await self._add_package_repo(repo)

        # Generate results
        async for res in self._cipd_pkgs.generate_results(
            self._scanned_packages
        ):
            await self._send_result(res)

        self.input_queue.put_nowait(None)

    async def _process_one(self, path: AsyncPath) -> None:
        pass
