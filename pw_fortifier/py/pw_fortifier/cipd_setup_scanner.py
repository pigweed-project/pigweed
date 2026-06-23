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

import json
import os
from typing import Any, Iterator

from pw_fortifier.cipd_scanner import CipdScanner
from pw_fortifier.find_core_owners import CoreOwnerFinder
from pw_fortifier.package_scanner import (
    FreshnessScanResult,
    TIER1_TOOLCHAIN,
    TIER2_DEVHOST,
    TIER3_UPSTREAM,
)


PIGWEED_BUILD_TOOLS = ['gn', 'make', 'ninja', 'clang', 'mingw64']


class CipdSetupScanner(CipdScanner):
    """Scans CIPD setup JSON files in pw_env_setup for freshness."""

    def _parse_package(
        self,
        package: dict[str, Any],
        owner: str | None,
        tier: int,
        filename: str,
    ) -> None:
        cipd_path = package.get('path')
        if not cipd_path or not isinstance(cipd_path, str):
            return
        platforms = package.get('platforms')
        if platforms is not None and not isinstance(platforms, list):
            return
        tags = package.get('tags', [])
        if not tags or not isinstance(tags, list):
            return
        cipd_tag = tags[0]
        if not isinstance(cipd_tag, str):
            return

        pkg_tier = tier
        if filename == 'pigweed.json':
            if any(tool in cipd_path for tool in PIGWEED_BUILD_TOOLS):
                pkg_tier = TIER1_TOOLCHAIN

        added_pkgs = self._add_cipd_path(
            cipd_path, cipd_tag, pkg_tier, platforms
        )
        for pkg in added_pkgs:
            self._owners[pkg] = owner

    def pre_scan(self) -> Iterator[FreshnessScanResult]:
        """Examines CIPD setup JSON files in pw_env_setup for freshness."""
        self._owners = {}
        pigweed_json_path = os.path.join(self.root, 'pigweed.json')

        try:
            with open(pigweed_json_path, 'r') as f:
                pigweed_json = json.load(f)
        except (IOError, json.JSONDecodeError):
            return

        try:
            cipd_package_files = pigweed_json['pw']['pw_env_setup'][
                'cipd_package_files'
            ]
        except KeyError:
            return

        # Queue stores (relative_path, tier)
        queue = []
        for path in cipd_package_files:
            filename = os.path.basename(path)
            if filename == 'upstream.json':
                tier = TIER3_UPSTREAM
            elif filename == 'default.json':
                tier = TIER2_DEVHOST
            else:
                tier = TIER2_DEVHOST
            queue.append((path, tier))

        while queue:
            path, tier = queue.pop(0)

            dirname = os.path.dirname(path)
            filename = os.path.basename(path)

            if filename == 'default.json':
                tier = TIER2_DEVHOST
            elif filename == 'upstream.json':
                tier = TIER3_UPSTREAM

            full_path = os.path.join(self.root, path)

            # Find owner
            finder = CoreOwnerFinder(root=self.root)
            finder.add(full_path, None)
            owner = finder.find()

            try:
                with open(full_path, 'r') as f:
                    obj = json.load(f)
            except (IOError, json.JSONDecodeError):
                continue

            packages = obj.get('packages', [])
            if packages:
                self._packages_dict = {}
                for pkg in packages:
                    self._parse_package(pkg, owner, tier, filename)
                yield from self._generate_results(path, 'cipd_setup')

            included_files = obj.get('included_files', [])
            for inc in included_files:
                inc_path = os.path.join(dirname, inc)
                inc_path = os.path.normpath(inc_path)
                queue.append((inc_path, tier))
