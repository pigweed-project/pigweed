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
from typing import Iterator

from pw_fortifier.package_scanner import (
    FreshnessScanResult,
    PackageScanner,
    PackageVersion,
    TIER0_ON_DEVICE,
    TIER2_DEVHOST,
)


class NpmScanner(PackageScanner):
    """Scans npm packages for freshness."""

    def __init__(self, root: str | os.PathLike[str] | None = None):
        super().__init__(root)
        self._versions: dict[str, tuple[str, int]] = {}
        self._on_device_modules: list[str] = []
        self.strict_semver = True

    def add_on_device_module(self, module: str) -> None:
        """Adds a module to be treated as TIER0_ON_DEVICE."""
        self._on_device_modules.append(module)

    def target(self) -> str:
        return 'package.json'

    def _get_resolved_versions(
        self, package_lock_json_path: str | os.PathLike[str]
    ) -> None:
        """Parses package-lock.json to build a mapping of package names to
        resolved versions and tiers.
        """
        self._versions.clear()
        try:
            with open(package_lock_json_path, 'r') as f:
                data = json.load(f)
        except (IOError, json.JSONDecodeError):
            return

        packages_block = data.get('packages', {})
        root_pkg = packages_block.get('', {})

        # dependencies
        deps = root_pkg.get('dependencies', {})
        if isinstance(deps, dict):
            dep_tier = TIER2_DEVHOST
            path_parts = os.path.normpath(package_lock_json_path).split(os.sep)
            if any(m in path_parts for m in self._on_device_modules):
                dep_tier = TIER0_ON_DEVICE
            for k, v in deps.items():
                if isinstance(v, str):
                    self._versions[k] = (self.clean_version(v), dep_tier)

        # devDependencies
        dev_deps = root_pkg.get('devDependencies', {})
        if isinstance(dev_deps, dict):
            for k, v in dev_deps.items():
                if isinstance(v, str):
                    self._versions[k] = (self.clean_version(v), TIER2_DEVHOST)

    @staticmethod
    def _run_npm(
        args: list[str], cwd: str | os.PathLike[str]
    ) -> subprocess.CompletedProcess:
        """Runs an npm command, raising a helpful error if npm is not found."""
        cmd = ['npm'] + args
        try:
            return subprocess.run(
                cmd,
                cwd=cwd,
                capture_output=True,
                text=True,
            )
        except FileNotFoundError as e:
            raise RuntimeError(
                "npm command not found. "
                "Please 'source activate.sh' and try again."
            ) from e

    def scan(
        self, pathname: str | os.PathLike[str]
    ) -> Iterator[FreshnessScanResult]:
        """Examines npm packages for freshness."""
        package_json_path = os.path.join(self.root, pathname)
        if 'node_modules' in os.path.normpath(package_json_path).split(os.sep):
            return
        cwd = os.path.dirname(package_json_path)

        package_lock_json_path = (
            os.path.splitext(package_json_path)[0] + '-lock.json'
        )
        self._get_resolved_versions(package_lock_json_path)

        if not self._versions:
            return

        rel_package_json = os.fspath(pathname)

        for pkg_name, (current_ver, tier) in self._versions.items():
            current_major = self.major_version(current_ver)
            if not current_major:
                continue

            key = f'{pkg_name}:{current_ver}'
            if key in self._scanned_packages:
                continue
            self._scanned_packages.add(key)

            time_info = self._get_package_times(pkg_name, cwd)
            if not time_info:
                continue

            all_versions: list[PackageVersion] = []
            for ver, ts in time_info.items():
                if ver in ('modified', 'created'):
                    continue
                try:
                    dt = date.fromisoformat(ts[:10])
                    all_versions.append(PackageVersion(ver, dt))
                except ValueError:
                    continue

            if not all_versions:
                continue

            current_ts = time_info.get(current_ver)
            current_date = None
            if current_ts:
                try:
                    current_date = date.fromisoformat(current_ts[:10])
                except ValueError:
                    pass

            if not current_date:
                continue

            current = PackageVersion(
                version=current_ver, timestamp=current_date
            )

            earliest = self.find_lowest(tier, current, all_versions)

            pattern = re.compile(r'"' + re.escape(pkg_name) + r'"\s*:')
            owner = self._find_owners(package_json_path, pattern)

            yield FreshnessScanResult(
                package=pkg_name,
                source=rel_package_json,
                pkg_type='npm',
                current=current,
                earliest=earliest,
                tier=tier,
                owner=owner,
            )

    def _get_package_times(
        self, pkg_name: str, cwd: str
    ) -> dict[str, str] | None:
        """Gets the time dictionary from npm view."""
        result = self._run_npm(['view', pkg_name, 'time', '--json'], cwd)
        if result.returncode != 0:
            return None

        try:
            return json.loads(result.stdout)
        except json.JSONDecodeError:
            return None
