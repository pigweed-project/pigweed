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
"""Package scanner for npm packages."""

from datetime import date
import json
import os
import re
import subprocess
import asyncio
from pw_fortifier.async_path import AsyncPath

from pw_fortifier.code_snippet import find_location
from pw_fortifier.package_analyzer import PackageAnalyzer
from pw_fortifier.freshness_result import (
    FreshnessResult,
    PackageVersion,
    TIER0_ON_DEVICE,
    TIER2_DEVHOST,
)
from pw_fortifier.semver import SemVer


PKG_TYPE: str = 'npm'


class NpmAnalyzer(PackageAnalyzer):
    """Scans npm packages for freshness."""

    PKG_TYPE = PKG_TYPE
    TARGET = 'package.json'

    def __init__(self) -> None:
        super().__init__()
        self._versions: dict[str, tuple[SemVer, int]] = {}
        self._on_device_modules: list[str] = []

    def add_on_device_module(self, module: str) -> None:
        """Adds a module to be treated as TIER0_ON_DEVICE.

        Args:
            module: The module name to treat as tier 0 on-device.
        """
        self._on_device_modules.append(module)

    async def _get_resolved_versions(
        self, package_lock_json_path: AsyncPath
    ) -> None:
        """Parses package-lock.json to map package names to versions/tiers."""
        self._versions.clear()
        content = await package_lock_json_path.read_text()
        data = json.loads(content)

        packages_block = data.get('packages', {})
        root_pkg = packages_block.get('', {})

        # dependencies
        deps = root_pkg.get('dependencies', {})
        if isinstance(deps, dict):
            dep_tier = TIER2_DEVHOST
            path_parts = package_lock_json_path.path.parts
            if any(m in path_parts for m in self._on_device_modules):
                dep_tier = TIER0_ON_DEVICE
            for k, v in deps.items():
                if isinstance(v, str):
                    parsed_ver = self.parse_semver(v)
                    if not parsed_ver:
                        raise ValueError(
                            f"Failed to parse semver for '{k}': '{v}'"
                        )
                    self._versions[k] = (parsed_ver, dep_tier)

        # devDependencies
        dev_deps = root_pkg.get('devDependencies', {})
        if isinstance(dev_deps, dict):
            for k, v in dev_deps.items():
                if isinstance(v, str):
                    parsed_ver = self.parse_semver(v)
                    if not parsed_ver:
                        raise ValueError(
                            f"Failed to parse semver for '{k}': '{v}'"
                        )
                    self._versions[k] = (parsed_ver, TIER2_DEVHOST)

    @staticmethod
    async def _run_npm(
        args: list[str], cwd: str | os.PathLike[str]
    ) -> subprocess.CompletedProcess:
        """Runs an npm command, raising a helpful error if npm is not found."""
        cmd = ['npm'] + args
        loop = asyncio.get_running_loop()

        def _run():
            return subprocess.run(
                cmd,
                cwd=cwd,
                capture_output=True,
                text=True,
            )

        try:
            result = await loop.run_in_executor(None, _run)
            if result.returncode != 0:
                raise subprocess.CalledProcessError(
                    result.returncode, cmd, result.stdout, result.stderr
                )
            return result
        except FileNotFoundError as e:
            raise FileNotFoundError(
                '`npm` command not found. '
                'You may need to activate the Pigweed environment.'
            ) from e

    async def _process_one(self, path: AsyncPath) -> None:
        """Examines npm packages for freshness."""
        if 'node_modules' in path.path.parts:
            return
        cwd = path.parent.path

        # package-lock.json is usually a sibling to package.json.
        package_lock_json_path = path.parent / (path.path.stem + '-lock.json')

        # The top-level package.json does not have a package-lock.json, as it
        # does not declare dependencies
        if not await package_lock_json_path.exists():
            return

        await self._get_resolved_versions(package_lock_json_path)
        if not self._versions:
            return

        rel_package_json = os.path.relpath(path.path, self.root)

        for pkg_name, (current_ver, tier) in self._versions.items():
            key = f'{pkg_name}:{current_ver}'
            if key in self._scanned_packages:
                continue
            self._scanned_packages.add(key)

            time_info = await self._get_package_times(pkg_name, str(cwd))
            if not time_info:
                raise RuntimeError(
                    f'No timestamps returned by npm view for {pkg_name}'
                )

            all_versions: list[PackageVersion] = []
            for ver, ts in time_info.items():
                if ver in ('modified', 'created'):
                    continue
                dt = date.fromisoformat(ts[:10])
                all_versions.append(PackageVersion(ver, dt))

            if not all_versions:
                raise RuntimeError(
                    f'No valid version dates found for {pkg_name}'
                )

            current_ver_str = str(current_ver)
            current_ts = time_info.get(current_ver_str)
            current_date = None
            if current_ts:
                current_date = date.fromisoformat(current_ts[:10])

            if not current_date:
                raise RuntimeError(
                    f'Timestamp for current version {current_ver_str} of '
                    f'{pkg_name} is missing from npm registry: {time_info}'
                )

            current = PackageVersion(
                version=current_ver_str, timestamp=current_date
            )

            earliest = self.find_lowest(tier, current, all_versions)

            pattern = re.compile(r'"' + re.escape(pkg_name) + r'"\s*:')
            location = find_location(self.root, rel_package_json, pattern)

            res = FreshnessResult(
                package=pkg_name,
                location=location,
                pkg_type=self.PKG_TYPE,
                current=current,
                earliest=earliest,
                tier=tier,
            )
            await self._send_result(res)

    async def _get_package_times(
        self, pkg_name: str, cwd: str
    ) -> dict[str, str]:
        """Gets the time dictionary from npm view."""
        result = await self._run_npm(['view', pkg_name, 'time', '--json'], cwd)
        return json.loads(result.stdout)
