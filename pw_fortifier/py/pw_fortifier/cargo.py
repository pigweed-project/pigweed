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
"""Package scanner for cargo packages."""

from datetime import date
import os
import re
import asyncio
import requests

# tomllib is new in Python 3.11
try:
    import tomllib as tomli  # type: ignore
except ImportError:
    import tomli  # type: ignore

from pw_fortifier.async_path import AsyncPath
from pw_fortifier.freshness_result import (
    FreshnessResult,
    PackageVersion,
    TIER0_ON_DEVICE,
    TIER2_DEVHOST,
)
from pw_fortifier.code_snippet import find_location
from pw_fortifier.package_analyzer import PackageAnalyzer


PKG_TYPE: str = 'cargo'


class CargoAnalyzer(PackageAnalyzer):
    """Scans cargo packages for freshness."""

    PKG_TYPE = PKG_TYPE
    TARGET = 'Cargo.toml'

    def __init__(self) -> None:
        super().__init__()
        self._versions: dict[str, str] = {}

    async def _get_resolved_versions(self, cargo_lock_path: AsyncPath) -> None:
        """Parses Cargo.lock to map package names to resolved versions."""
        self._versions.clear()
        if not await cargo_lock_path.exists():
            return
        content = await cargo_lock_path.read_text()
        data = tomli.loads(content)

        packages = data.get('package', [])
        if isinstance(packages, list):
            for pkg in packages:
                if isinstance(pkg, dict) and 'name' in pkg and 'version' in pkg:
                    self._versions[pkg['name']] = pkg['version']

    @staticmethod
    async def _get_crates(
        cargo_toml_path: AsyncPath,
        rel_cargo_toml: str,
    ) -> dict[str, int]:
        """Parses Cargo.toml to extract dependencies and their tiers."""
        content = await cargo_toml_path.read_text()
        data = tomli.loads(content)

        is_no_std = 'no_std' in rel_cargo_toml
        crates: dict[str, int] = {}

        if 'dependencies' in data and isinstance(data['dependencies'], dict):
            tier = TIER0_ON_DEVICE if is_no_std else TIER2_DEVHOST
            for name in data['dependencies'].keys():
                crates[name] = tier

        if 'dev-dependencies' in data and isinstance(
            data['dev-dependencies'], dict
        ):
            for name in data['dev-dependencies'].keys():
                if name not in crates:
                    crates[name] = TIER2_DEVHOST

        return crates

    async def _scan_crate(
        self,
        pkg_name: str,
        tier: int,
        rel_cargo_toml: AsyncPath,
    ) -> FreshnessResult | None:
        """Scans a single cargo package for freshness."""
        current_ver = self._versions[pkg_name]

        key = f'{pkg_name}:{current_ver}'
        if key in self._scanned_packages:
            return None
        self._scanned_packages.add(key)

        # Fetch from crates.io
        headers = {
            'User-Agent': ('cargo-freshness (fortifier-scanner@google.com)')
        }
        url = f'https://crates.io/api/v1/crates/{pkg_name}'
        loop = asyncio.get_running_loop()
        response = await loop.run_in_executor(
            None,
            lambda: requests.get(url, headers=headers, timeout=10),
        )
        if response.status_code != 200:
            return None
        resp_data = response.json()

        # Parse times
        time_info = {}
        versions_data = resp_data.get('versions', [])
        all_versions: list[PackageVersion] = []
        for v_data in versions_data:
            ver = v_data.get('num')
            created_at = v_data.get('created_at')
            if ver and created_at:
                if self.parse_semver(ver) is None:
                    continue
                time_info[ver] = created_at
                all_versions.append(
                    PackageVersion(ver, date.fromisoformat(created_at[:10]))
                )

        # Get dates
        current_ts = time_info.get(self.clean_version(current_ver))
        current_date = None
        if current_ts:
            current_date = date.fromisoformat(current_ts[:10])

        if not current_date:
            return None

        current = PackageVersion(version=current_ver, timestamp=current_date)
        earliest = self.find_lowest(tier, current, all_versions)

        # Find location in Cargo.toml
        combined_pattern = re.compile(
            r'^\s*'
            + re.escape(pkg_name)
            + r'\s*=|'
            + r'\[(dependencies|dev-dependencies|build-dependencies)\.'
            + re.escape(pkg_name)
            + r'\]'
        )
        location = find_location(
            self.root, rel_cargo_toml.path, combined_pattern
        )

        return FreshnessResult(
            package=pkg_name,
            location=location,
            pkg_type=self.PKG_TYPE,
            current=current,
            earliest=earliest,
            tier=tier,
        )

    async def _process_one(self, path: AsyncPath) -> None:
        """Examines cargo packages in Cargo.toml for freshness."""
        cargo_lock_path = path.parent / 'Cargo.lock'
        if not await cargo_lock_path.exists():
            return

        rel_cargo_toml = os.path.relpath(path.path, self.root)

        crates = await self._get_crates(path, rel_cargo_toml)

        await self._get_resolved_versions(cargo_lock_path)
        if crates and not self._versions:
            raise RuntimeError(
                f"No resolved versions found in '{cargo_lock_path}'"
            )

        for pkg_name, tier in crates.items():
            res = await self._scan_crate(
                pkg_name, tier, AsyncPath(rel_cargo_toml)
            )
            await self._send_result(res)
