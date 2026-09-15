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
import logging
import os

import asyncio
from packaging.requirements import Requirement
from packaging.version import InvalidVersion, Version
import requests

from pw_fortifier.async_path import AsyncPath
from pw_fortifier.code_snippet import CodeSnippet
from pw_fortifier.package_analyzer import PackageAnalyzer
from pw_fortifier.freshness_result import (
    FreshnessResult,
    PackageVersion,
    TIER2_DEVHOST,
    TIER3_UPSTREAM,
)

_LOG = logging.getLogger(__name__)

PKG_TYPE: str = 'pip'


class PipAnalyzer(PackageAnalyzer):
    """Scans pip packages for freshness."""

    PKG_TYPE = PKG_TYPE
    TARGET = 'setup.cfg'

    def __init__(self) -> None:
        super().__init__()
        self._packages: dict[str, tuple[str, int, str]] = {}

    def _add_requirement(
        self,
        req: Requirement,
        tier: int,
        source: str | os.PathLike[str],
    ) -> None:
        specs = list(req.specifier)
        if not specs:
            return

        valid_specs = [s for s in specs if s.operator in ('==', '>=', '~=')]
        if not valid_specs:
            raise ValueError(
                f"Unsupported operator '{specs[0].operator}' for {req.name}"
            )

        spec = valid_specs[0]
        source_str = os.fspath(source)

        if req.name not in self._packages:
            self._packages[req.name] = (spec.version, tier, source_str)
            return

        v1_str, _, _ = self._packages[req.name]
        v2_str = spec.version

        v1_parsed = self.parse_semver(v1_str)
        v2_parsed = self.parse_semver(v2_str)

        if v1_parsed is None:
            self._packages[req.name] = (v2_str, tier, source_str)
        elif v2_parsed is None:
            pass
        elif v2_parsed > v1_parsed:
            self._packages[req.name] = (v2_str, tier, source_str)

    async def _set_up(self) -> None:
        pigweed_json_path = AsyncPath(self.root) / 'pigweed.json'
        content = await pigweed_json_path.read_text()
        pigweed_json = json.loads(content)

        virtualenv_cfg = (
            pigweed_json.get('pw', {})
            .get('pw_env_setup', {})
            .get('virtualenv', {})
        )
        requirements_files = virtualenv_cfg.get('requirements', [])
        constraints_files = virtualenv_cfg.get('constraints', [])

        for rel_path in requirements_files + constraints_files:
            full_path = AsyncPath(self.root) / rel_path

            if 'upstream' in full_path.name:
                tier = TIER3_UPSTREAM
            else:
                tier = TIER2_DEVHOST

            content = await full_path.read_text()
            lines = content.splitlines()

            # Join lines with backslash continuations
            joined_lines = []
            current_line = ''
            for line in lines:
                line_str = line.strip()
                if line_str.endswith('\\'):
                    current_line += line_str[:-1].strip() + ' '
                else:
                    current_line += line_str
                    joined_lines.append(current_line)
                    current_line = ''
            if current_line:
                joined_lines.append(current_line)

            for line in joined_lines:
                line = line.split('#', 1)[0].strip()
                if not line or line.startswith('-'):
                    continue

                # Strip pip options starting with ' --'
                parts = line.split(' --')
                req = Requirement(parts[0].strip())
                self._add_requirement(req, tier, rel_path)

    async def _resolve_package(
        self, pkg_name: str, current_ver: str, tier: int
    ) -> tuple[PackageVersion | None, PackageVersion | None]:
        """Resolves earliest and current PackageVersion for a pip package."""
        current_clean = self.clean_version(current_ver)
        loop = asyncio.get_running_loop()
        response = await loop.run_in_executor(
            None,
            lambda: requests.get(
                f'https://pypi.org/pypi/{pkg_name}/json', timeout=10
            ),
        )
        if response.status_code != 200:
            response.raise_for_status()
            raise requests.exceptions.HTTPError(
                f'PyPI request for {pkg_name} returned status '
                f'{response.status_code}',
                response=response,
            )
        data = response.json()

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
            _LOG.error('Invalid response data from PyPI for %s', pkg_name)
            return None, None

        all_versions: list[PackageVersion] = []
        for ver, ts in time_info.items():
            dt = date.fromisoformat(ts[:10])
            all_versions.append(PackageVersion(ver, dt))

        if not all_versions:
            _LOG.error('No versions found in PyPI data for %s', pkg_name)
            return None, None

        current_ts = time_info.get(current_clean) or time_info.get(current_ver)
        if not current_ts:
            try:
                target_ver = Version(current_ver)
                for ver_str, ts in time_info.items():
                    try:
                        if Version(ver_str) == target_ver:
                            current_ts = ts
                            current_ver = ver_str
                            break
                    except InvalidVersion:
                        continue
            except InvalidVersion:
                pass

        if not current_ts:
            _LOG.error(
                'Current version %s of %s not found in PyPI releases',
                current_ver,
                pkg_name,
            )
            return None, None

        current_date = date.fromisoformat(current_ts[:10])
        current = PackageVersion(version=current_ver, timestamp=current_date)

        earliest = self.find_lowest(tier, current, all_versions)
        return earliest, current

    async def _process_one(self, path: AsyncPath) -> None:
        """Examines pip packages for freshness."""
        config = configparser.ConfigParser()
        content = await path.read_text()
        config.read_string(content)

        install_requires = config.get(
            'options', 'install_requires', fallback=''
        )
        packages = [
            line.strip()
            for line in install_requires.splitlines()
            if line.strip()
        ]

        rel_path = os.path.relpath(path.path, self.root)

        for line in packages:
            line = line.split('#', 1)[0].strip()
            if not line or line.startswith('-'):
                continue
            parts = line.split(' --')
            req = Requirement(parts[0].strip())
            self._add_requirement(req, TIER2_DEVHOST, rel_path)

    async def _tear_down(self) -> None:
        for pkg_name, (version, tier, source) in self._packages.items():
            earliest, current = await self._resolve_package(
                pkg_name, version, tier
            )
            if earliest is None or current is None:
                continue

            location = CodeSnippet(file=str(source))

            res = FreshnessResult(
                package=pkg_name,
                location=location,
                pkg_type=self.PKG_TYPE,
                current=current,
                earliest=earliest,
                tier=tier,
            )
            await self._send_result(res)

        await super()._tear_down()
