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
"""Base package scanner for CIPD-based dependencies."""

from datetime import datetime, timezone
import subprocess
from typing import Iterator, cast

from pw_fortifier.package_scanner import (
    FreshnessScanResult,
    PackageScanner,
    PackageVersion,
)


class CipdScanner(PackageScanner):
    """Base class for CIPD package scanners."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._packages_dict = {}
        self._owners = {}

    @staticmethod
    def _run_cipd(args: list[str]) -> subprocess.CompletedProcess:
        """Runs cipd command, raising a helpful error if it fails."""
        cmd = ['cipd'] + args
        try:
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
            )
            if result.returncode != 0:
                raise RuntimeError(
                    f'cipd failed: {result.stderr}. '
                    'You may need to activate the Pigweed environment or '
                    "run 'cipd auth-login'."
                )
            return result
        except FileNotFoundError as e:
            raise RuntimeError(
                'cipd command not found. '
                'You may need to activate the Pigweed environment.'
            ) from e

    def run_cipd_describe(
        self, cipd_pkg: str, ref: str
    ) -> dict[str, str | dict[str, str]]:
        """Runs `cipd describe` and returns parsed output as a dict."""
        try:
            result = self._run_cipd(['describe', cipd_pkg, '--version', ref])
            desc_lines = [
                l.rstrip() for l in result.stdout.splitlines() if l.strip()
            ]
        except RuntimeError:
            return {}

        parsed_data: dict[str, str | dict[str, str]] = {}
        current_dict: dict[str, str] | None = None

        for line in desc_lines:
            stripped = line.lstrip()
            indent = len(line) - len(stripped)

            if indent == 0:
                current_dict = None
                if ':' in line:
                    key, val = line.split(':', 1)
                    key = key.strip()
                    val = val.strip()
                    if val:
                        parsed_data[key] = val
                    else:
                        current_dict = {}
                        parsed_data[key] = current_dict
            else:
                if current_dict is not None:
                    if ':' in stripped:
                        sub_key, sub_val = stripped.split(':', 1)
                        current_dict[sub_key.strip()] = sub_val.strip()
                    else:
                        current_dict[stripped.strip()] = ''

        return parsed_data

    def run_cipd_instances(self, cipd_pkg: str) -> Iterator[str]:
        """Runs `cipd instances` and yields instance IDs."""
        try:
            instances_result = self._run_cipd(['instances', cipd_pkg])
            lines = [
                l.strip()
                for l in instances_result.stdout.splitlines()
                if l.strip()
            ]
        except RuntimeError:
            return

        for line in lines[2:]:
            parts = line.split()
            if not parts:
                continue
            yield parts[0]

    def _add_cipd_path(
        self,
        cipd_path: str,
        cipd_tag: str,
        tier: int,
        platforms: list[str] | None = None,
    ) -> list[str]:
        """Runs cipd ls and adds resulting packages to _packages_dict."""
        segments = cipd_path.split('/')
        last_segment = segments[-1] if segments else ""
        if last_segment not in ('${os}-${arch}', '${platform}'):
            cipd_pkgs = [cipd_path]
        else:
            stripped_path = '/'.join(segments[:-1])
            if platforms is None:
                try:
                    ls_result = self._run_cipd(['ls', stripped_path])
                    cipd_pkgs = [
                        l.strip()
                        for l in ls_result.stdout.splitlines()
                        if l.strip()
                    ]
                except RuntimeError as e:
                    raise e
            else:
                cipd_pkgs = [f'{stripped_path}/{plat}' for plat in platforms]

        for pkg in cipd_pkgs:
            self._packages_dict[pkg] = (cipd_tag, tier)
        return cipd_pkgs

    def _get_package_version(
        self, cipd_pkg: str, ref: str
    ) -> PackageVersion | None:
        """Gets the PackageVersion for a cipd package version reference."""
        desc_dict = self.run_cipd_describe(cipd_pkg, ref)
        if not desc_dict:
            return None

        reg_at = cast(str, desc_dict['Registered at'])

        parts = reg_at.split()
        if len(parts) < 3:
            return None
        date_str = parts[0]
        time_str = parts[1]
        offset_str = parts[2]

        # Strip fractional seconds (e.g. .092067)
        time_str_no_ms = time_str.split('.')[0]
        dt_str = f'{date_str} {time_str_no_ms} {offset_str}'
        try:
            dt = datetime.strptime(dt_str, '%Y-%m-%d %H:%M:%S %z')
            dt_utc = dt.astimezone(timezone.utc)
            timestamp = dt_utc.date()
        except ValueError:
            return None

        version = None
        for block_name in ('Tags', 'Metadata'):
            block = desc_dict.get(block_name)
            if isinstance(block, dict):
                if 'version' in block:
                    version = block['version']
                    break
                if 'git_revision' in block:
                    version = f"git_revision:{block['git_revision']}"
                    break

        if version is None:
            return None

        return PackageVersion(version=version, timestamp=timestamp)

    def _get_package_versions(self, cipd_pkg: str) -> list[PackageVersion]:
        """Gets the list of all PackageVersions for a cipd package."""
        versions = []
        for instance_id in self.run_cipd_instances(cipd_pkg):
            v = self._get_package_version(cipd_pkg, instance_id)
            if v:
                versions.append(v)
        return versions

    def _generate_results(
        self, rel_path: str, pkg_type: str
    ) -> Iterator[FreshnessScanResult]:
        """Generates FreshnessScanResult for gathered packages."""
        for cipd_pkg, (cipd_tag, tier) in self._packages_dict.items():
            current_ver = self._get_package_version(cipd_pkg, cipd_tag)
            if not current_ver:
                continue

            key = f'{cipd_pkg}:{current_ver.version}'
            if key in self._scanned_packages:
                continue
            self._scanned_packages.add(key)

            versions = self._get_package_versions(cipd_pkg)
            if not versions:
                continue

            earliest = self.find_lowest(tier, current_ver, versions)

            owner = self._owners.get(cipd_pkg)

            yield FreshnessScanResult(
                package=cipd_pkg,
                source=rel_path,
                pkg_type=pkg_type,
                current=current_ver,
                earliest=earliest,
                tier=tier,
                owner=owner,
            )
