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
from typing import Iterator
import requests

try:
    import tomllib as tomli  # type: ignore
except ImportError:
    import tomli  # type: ignore


from pw_fortifier.package_scanner import (
    FreshnessScanResult,
    PackageScanner,
    PackageVersion,
    TIER0_ON_DEVICE,
    TIER2_DEVHOST,
)


class CargoScanner(PackageScanner):
    """Scans cargo packages for freshness."""

    def __init__(self, root: str | os.PathLike[str] | None = None):
        super().__init__(root)
        self._versions: dict[str, str] = {}
        self.strict_semver = True

    def target(self) -> str:
        return 'Cargo.toml'

    def _get_resolved_versions(
        self, cargo_lock_path: str | os.PathLike[str]
    ) -> None:
        """Parses Cargo.lock to build a mapping of package names to resolved
        versions.
        """
        self._versions.clear()
        try:
            with open(cargo_lock_path, 'rb') as f:
                data = tomli.load(f)
        except (IOError, tomli.TOMLDecodeError):
            return

        packages = data.get('package', [])
        if isinstance(packages, list):
            for pkg in packages:
                if isinstance(pkg, dict) and 'name' in pkg and 'version' in pkg:
                    self._versions[pkg['name']] = pkg['version']

    @staticmethod
    def _get_crates(
        cargo_toml_path: str | os.PathLike[str],
        rel_cargo_toml: str,
    ) -> dict[str, int] | None:
        """Parses Cargo.toml to extract dependencies and their tiers."""
        try:
            with open(cargo_toml_path, 'rb') as f:
                data = tomli.load(f)
        except (tomli.TOMLDecodeError, IOError):
            return None

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

    def _scan_crate(
        self,
        pkg_name: str,
        tier: int,
        cargo_toml_path: str | os.PathLike[str],
        rel_cargo_toml: str,
    ) -> FreshnessScanResult | None:
        """Scans a single cargo package for freshness."""
        current_ver = self._versions[pkg_name]

        key = f'{pkg_name}:{current_ver}'
        if key in self._scanned_packages:
            return None
        self._scanned_packages.add(key)

        # Fetch from crates.io
        headers = {
            "User-Agent": ("cargo-freshness (fortifier-scanner@google.com)")
        }
        url = f"https://crates.io/api/v1/crates/{pkg_name}"
        try:
            response = requests.get(url, headers=headers, timeout=10)
            if response.status_code != 200:
                return None
            resp_data = response.json()
        except requests.RequestException:
            return None

        # Parse times
        time_info = {}
        versions_data = resp_data.get('versions', [])
        all_versions: list[PackageVersion] = []
        for v_data in versions_data:
            ver = v_data.get('num')
            created_at = v_data.get('created_at')
            if ver and created_at:
                if self._parse_semver(ver) is None:
                    continue
                time_info[ver] = created_at
                try:
                    all_versions.append(
                        PackageVersion(ver, date.fromisoformat(created_at[:10]))
                    )
                except ValueError:
                    pass

        # Get dates
        current_ts = time_info.get(self.clean_version(current_ver))
        current_date = None
        if current_ts:
            try:
                current_date = date.fromisoformat(current_ts[:10])
            except ValueError:
                pass

        if not current_date:
            return None

        current = PackageVersion(version=current_ver, timestamp=current_date)
        earliest = self.find_lowest(tier, current, all_versions)

        # Find owner in Cargo.toml
        combined_pattern = re.compile(
            r'^\s*'
            + re.escape(pkg_name)
            + r'\s*=|'
            + r'\[(dependencies|dev-dependencies|build-dependencies)\.'
            + re.escape(pkg_name)
            + r'\]'
        )
        owner = self._find_owners(cargo_toml_path, combined_pattern)

        return FreshnessScanResult(
            package=pkg_name,
            source=rel_cargo_toml,
            pkg_type='cargo',
            current=current,
            earliest=earliest,
            tier=tier,
            owner=owner,
        )

    def scan(
        self, pathname: str | os.PathLike[str]
    ) -> Iterator[FreshnessScanResult]:
        """Examines cargo packages in Cargo.toml for freshness."""
        cargo_toml_path = os.path.join(self.root, pathname)
        rel_cargo_toml = os.fspath(pathname)

        crates = self._get_crates(cargo_toml_path, rel_cargo_toml)
        if crates is None:
            return

        cargo_lock_path = os.path.splitext(cargo_toml_path)[0] + '.lock'
        self._get_resolved_versions(cargo_lock_path)

        for pkg_name, tier in crates.items():
            res = self._scan_crate(
                pkg_name, tier, cargo_toml_path, rel_cargo_toml
            )
            if res:
                yield res
