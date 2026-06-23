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
"""Package scanner for pip packages."""

import configparser
from datetime import date
import json
import os

from typing import Iterator
from packaging.requirements import Requirement
import requests

from pw_fortifier.find_core_owners import CoreOwnerFinder
from pw_fortifier.package_scanner import (
    FreshnessScanResult,
    PackageScanner,
    PackageVersion,
    TIER2_DEVHOST,
    TIER3_UPSTREAM,
)


class PipScanner(PackageScanner):
    """Scans pip packages for freshness."""

    def __init__(self, root: str | os.PathLike[str] | None = None) -> None:
        super().__init__(root)
        self._packages: dict[str, tuple[str, int, str]] = {}
        self.strict_semver = True

    def target(self) -> str:
        return 'setup.cfg'

    def _add_requirement(
        self,
        req: Requirement,
        tier: int,
        source: str | os.PathLike[str],
    ) -> None:
        specs = list(req.specifier)
        if len(specs) != 1:
            raise ValueError(
                f"Unsupported format: multiple specifiers for {req.name}"
            )
        spec = specs[0]
        if spec.operator not in ('==', '>='):
            raise ValueError(
                f"Unsupported operator '{spec.operator}' for {req.name}"
            )

        source_str = os.fspath(source)

        if req.name not in self._packages:
            self._packages[req.name] = (spec.version, tier, source_str)
            return

        v1_str, _, _ = self._packages[req.name]
        v2_str = spec.version

        v1_parsed = self._parse_semver(v1_str)
        v2_parsed = self._parse_semver(v2_str)

        if v1_parsed is None:
            self._packages[req.name] = (v2_str, tier, source_str)
        elif v2_parsed is None:
            pass
        elif v2_parsed > v1_parsed:
            self._packages[req.name] = (v2_str, tier, source_str)

    def pre_scan(self) -> Iterator[FreshnessScanResult]:
        pigweed_json_path = os.path.join(self.root, 'pigweed.json')
        try:
            with open(pigweed_json_path, 'r') as f:
                pigweed_json = json.load(f)
        except (IOError, json.JSONDecodeError):
            return

        try:
            requirements_files = pigweed_json['pw']['pw_env_setup'][
                'virtualenv'
            ]['requirements']
        except KeyError:
            return

        for rel_path in requirements_files:
            full_path = os.path.join(self.root, rel_path)

            # Find the owner of the whole file (unused but requested)
            finder = CoreOwnerFinder(root=self.root)
            finder.add(full_path, None)
            _owner = finder.find()

            if 'upstream' in os.path.basename(rel_path):
                tier = TIER3_UPSTREAM
            else:
                tier = TIER2_DEVHOST

            try:
                with open(full_path, 'r') as f:
                    lines = f.readlines()
            except IOError:
                continue

            # Join lines with backslash continuations
            joined_lines = []
            current_line = ""
            for line in lines:
                line_str = line.strip()
                if line_str.endswith('\\'):
                    current_line += line_str[:-1].strip() + " "
                else:
                    current_line += line_str
                    joined_lines.append(current_line)
                    current_line = ""
            if current_line:
                joined_lines.append(current_line)

            for line in joined_lines:
                line = line.strip()
                if not line or line.startswith('#') or line.startswith('-'):
                    continue

                # Strip pip options starting with ' --'
                parts = line.split(' --')
                req_str = parts[0].strip()

                try:
                    req = Requirement(req_str)
                    self._add_requirement(req, tier, rel_path)
                except Exception:  # pylint: disable=broad-except
                    pass

        yield from ()

    def _resolve_package(
        self, pkg_name: str, current_ver: str, tier: int
    ) -> tuple[PackageVersion | None, PackageVersion | None]:
        """Resolves the earliest and current PackageVersion for a pip
        package.
        """
        current_clean = self.clean_version(current_ver)
        try:
            response = requests.get(
                f"https://pypi.org/pypi/{pkg_name}/json", timeout=10
            )
            if response.status_code != 200:
                return None, None
            data = response.json()
        except requests.RequestException:
            return None, None

        time_info = {}
        if 'releases' in data and isinstance(data['releases'], dict):
            for ver, releases in data['releases'].items():
                if releases and isinstance(releases, list):
                    ts = releases[0].get('upload_time_iso_8601') or releases[
                        0
                    ].get('upload_time')
                    if ts:
                        time_info[ver] = ts
        elif isinstance(data, dict):
            time_info = data
        else:
            return None, None

        all_versions: list[PackageVersion] = []
        for ver, ts in time_info.items():
            try:
                dt = date.fromisoformat(ts[:10])
                all_versions.append(PackageVersion(ver, dt))
            except (ValueError, TypeError):
                continue

        if not all_versions:
            return None, None

        current_ts = time_info.get(current_clean)
        current_date = None
        if current_ts:
            try:
                current_date = date.fromisoformat(current_ts[:10])
            except ValueError:
                pass

        if not current_date:
            return None, None

        current = PackageVersion(version=current_ver, timestamp=current_date)

        earliest = self.find_lowest(tier, current, all_versions)
        return earliest, current

    def scan(
        self, pathname: str | os.PathLike[str]
    ) -> Iterator[FreshnessScanResult]:
        """Examines pip packages for freshness."""
        full_path = os.path.join(self.root, pathname)
        config = configparser.ConfigParser()
        try:
            config.read(full_path)
        except configparser.Error:
            return

        install_requires = config.get(
            'options', 'install_requires', fallback=''
        )
        packages = [
            line.strip()
            for line in install_requires.splitlines()
            if line.strip()
        ]

        for line in packages:
            try:
                req = Requirement(line)
                self._add_requirement(req, TIER2_DEVHOST, pathname)
            except Exception:  # pylint: disable=broad-except
                pass

        yield from ()

    def post_scan(self) -> Iterator[FreshnessScanResult]:
        for pkg_name, (version, tier, source) in self._packages.items():
            full_source_path = os.path.join(self.root, source)
            finder = CoreOwnerFinder(root=self.root)
            finder.add(full_source_path, None)
            owner = finder.find()

            earliest, current = self._resolve_package(pkg_name, version, tier)
            if earliest is None or current is None:
                continue

            yield FreshnessScanResult(
                package=pkg_name,
                source=source,
                pkg_type='pip',
                current=current,
                earliest=earliest,
                tier=tier,
                owner=owner,
            )
