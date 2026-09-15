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
"""Package scanner for CIPD setup JSON files in pw_env_setup."""

import argparse
import json
import os

from pw_fortifier.async_path import AsyncPath
from pw_fortifier.code_snippet import CodeSnippet
from pw_fortifier.cipd_utils import CipdPackageSet
from pw_fortifier.package_analyzer import PackageAnalyzer
from pw_fortifier.freshness_result import (
    TIER1_TOOLCHAIN,
    TIER2_DEVHOST,
    TIER3_UPSTREAM,
)


PIGWEED_BUILD_TOOLS = ['gn', 'make', 'ninja', 'clang', 'mingw64']

PKG_TYPE: str = 'cipd_setup'


class CipdSetupAnalyzer(PackageAnalyzer):
    """Scans CIPD setup JSON files in pw_env_setup for freshness."""

    PKG_TYPE = PKG_TYPE

    def __init__(self) -> None:
        super().__init__()
        self.loose_semver = True
        self._pending_files = 0
        self._file_tiers: dict[str, int] = {}

    async def configure(self, args: argparse.Namespace) -> None:
        """Configures the analyzer and determines if setup can be skipped."""
        await super().configure(args)
        if args.files is None:
            return
        if 'pigweed.json' in args.files:
            return
        self.skip_setup = True

    async def _set_up(self) -> None:
        """Examines CIPD setup JSON files in pw_env_setup for freshness."""
        pigweed_json_path = AsyncPath(self.root) / 'pigweed.json'
        content = await pigweed_json_path.read_text()
        pigweed_json = json.loads(content)

        cipd_package_files = pigweed_json['pw']['pw_env_setup'][
            'cipd_package_files'
        ]

        # 1. Walk the graphs DFS to compute the final tier for each file.
        self._file_tiers = {}
        process_order = []

        async def walk(rel_path: str, current_tier: int):
            filename = os.path.basename(rel_path)
            if filename == 'default.json':
                current_tier = TIER2_DEVHOST
            elif filename == 'upstream.json':
                current_tier = TIER3_UPSTREAM

            norm_path = os.path.normpath(rel_path)

            if norm_path in self._file_tiers:
                prev_tier = self._file_tiers[norm_path]
                if (
                    prev_tier == TIER3_UPSTREAM
                    and current_tier == TIER2_DEVHOST
                ):
                    self._file_tiers[norm_path] = current_tier
                    # Re-traverse children because tier updated from 3 to 2.
                else:
                    return
            else:
                self._file_tiers[norm_path] = current_tier
                process_order.append(norm_path)

            abs_path = AsyncPath(self.root) / norm_path
            content = await abs_path.read_text()
            obj = json.loads(content)

            dirname = os.path.dirname(norm_path)
            included_files = obj.get('included_files', [])
            for inc in included_files:
                inc_path = os.path.normpath(os.path.join(dirname, inc))
                await walk(inc_path, current_tier)

        for path in cipd_package_files:
            await walk(path, TIER2_DEVHOST)

        # To prioritize TIER2 over TIER3 (so that if a package is reachable
        # via both, it is processed and yielded with TIER2 first, and the
        # TIER3 scan is skipped by self._scanned_packages), we sort the
        # files by tier.
        process_order.sort(key=lambda p: self._file_tiers[p])

        self._pending_files = len(process_order)
        if self._pending_files == 0:
            self.input_queue.put_nowait(None)
            return

        for path in process_order:
            self.input_queue.put_nowait(AsyncPath(self.root) / path)

    async def _process_one(self, path: AsyncPath) -> None:
        abs_path = path
        rel_path = os.path.relpath(abs_path.path, self.root)
        norm_path = os.path.normpath(rel_path)
        filename = abs_path.name

        # Use the pre-calculated tier
        file_tier = self._file_tiers[norm_path]

        content = await abs_path.read_text()
        obj = json.loads(content)

        packages = obj.get('packages', [])
        if packages:
            cipd_pkgs = CipdPackageSet(rel_path, self.PKG_TYPE)
            location = CodeSnippet(file=rel_path)
            for pkg_data in packages:
                pkg = pkg_data.get('path')
                assert isinstance(pkg, str)

                tags = pkg_data.get('tags')
                assert isinstance(tags, list)
                assert isinstance(tags[0], str)
                version = tags[0]

                tier = file_tier
                if filename == 'pigweed.json':
                    if any(tool in pkg for tool in PIGWEED_BUILD_TOOLS):
                        tier = TIER1_TOOLCHAIN

                platforms = pkg_data.get('platforms')
                if platforms is not None:
                    assert isinstance(platforms, list)
                    assert platforms
                    assert isinstance(platforms[0], str)

                await cipd_pkgs.add(
                    pkg, version, tier, platforms=platforms, location=location
                )

            async for res in cipd_pkgs.generate_results(self._scanned_packages):
                await self._send_result(res)

        self._file_processed()

    def _file_processed(self) -> None:
        self._pending_files -= 1
        if self._pending_files == 0:
            self.input_queue.put_nowait(None)
