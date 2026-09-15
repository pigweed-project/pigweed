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
"""Package scanner for Go modules in go.mod."""

import argparse
import asyncio
from datetime import date, datetime
import json
import logging
import os
import re
import subprocess

from pw_fortifier.async_path import AsyncPath
from pw_fortifier.freshness_result import (
    FreshnessResult,
    PackageVersion,
    TIER2_DEVHOST,
)
from pw_fortifier.code_snippet import find_location
from pw_fortifier.package_analyzer import PackageAnalyzer

_LOG = logging.getLogger(__name__)


def _parse_go_time(time_str: str) -> date:
    """Parses Go module timestamp (RFC3339) into a date."""
    if time_str.endswith('Z'):
        time_str = time_str[:-1] + '+00:00'
    dt = datetime.fromisoformat(time_str)
    return dt.date()


PKG_TYPE: str = 'go_mod'


class GoModAnalyzer(PackageAnalyzer):
    """Scans Go modules in go.mod for freshness."""

    PKG_TYPE = PKG_TYPE

    async def configure(self, args: argparse.Namespace) -> None:
        """Configures the analyzer and determines if setup can be skipped."""
        await super().configure(args)
        if args.files is None:
            return
        if 'go.mod' in args.files:
            return
        self.skip_setup = True

    @staticmethod
    async def _run_go(
        args: list[str], cwd: str | os.PathLike[str]
    ) -> subprocess.CompletedProcess:
        """Runs go command."""
        cmd = ['go'] + args
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
                '`go` command not found. '
                'You may need to activate the Pigweed environment.'
            ) from e

    async def _run_go_list(
        self, args: list[str], cwd: str | os.PathLike[str]
    ) -> subprocess.CompletedProcess:
        """Runs go list command with json and module flags."""
        return await self._run_go(['list', '-json', '-m'] + args, cwd)

    async def _get_all_versions(
        self,
        pkg_path: str,
        versions_list: list[str],
        cwd: str | os.PathLike[str],
    ) -> list[PackageVersion]:
        """Queries go tool for details of all versions in versions_list."""
        all_versions = []
        for v in versions_list:
            v_res = await self._run_go_list([f'{pkg_path}@{v}'], cwd)
            v_data = json.loads(v_res.stdout)
            v_time_str = v_data.get('Time')
            if v_time_str:
                v_date = _parse_go_time(v_time_str)
                all_versions.append(PackageVersion(version=v, timestamp=v_date))
        return all_versions

    async def _set_up(self) -> None:
        """Examines Go modules for freshness."""
        go_mod_path = AsyncPath(self.root) / 'go.mod'
        cwd = go_mod_path.parent.path
        rel_path = 'go.mod'

        if not await go_mod_path.exists():
            self.input_queue.put_nowait(None)
            return

        result = await self._run_go_list(['all'], cwd)
        output = result.stdout

        decoder = json.JSONDecoder()
        pos = 0
        while pos < len(output):
            # Skip leading whitespace
            while pos < len(output) and output[pos].isspace():
                pos += 1
            if pos >= len(output):
                break

            obj, index = decoder.raw_decode(output, pos)
            pos = index

            if obj.get('Indirect', False) or obj.get('Main', False):
                continue

            pkg_path = obj.get('Path')
            version = obj.get('Version')
            time_str = obj.get('Time')
            if not pkg_path or not version or not time_str:
                _LOG.warning(
                    'Skipping local or unversioned Go module: %s',
                    pkg_path or obj,
                )
                continue

            key = f'{pkg_path}:{version}'
            if key in self._scanned_packages:
                continue
            self._scanned_packages.add(key)

            current_time = _parse_go_time(time_str)
            current = PackageVersion(version=version, timestamp=current_time)

            if current.version.startswith('v0.0.0-'):
                earliest = current
            else:
                versions_res = await self._run_go_list(
                    ['-versions', pkg_path], cwd
                )
                versions_list = json.loads(versions_res.stdout).get(
                    'Versions', []
                )

                all_versions = await self._get_all_versions(
                    pkg_path, versions_list, cwd
                )
                if not all_versions:
                    raise RuntimeError(
                        f'Failed to resolve versions for direct Go module '
                        f'{pkg_path}'
                    )

                earliest = self.find_lowest(
                    TIER2_DEVHOST,
                    PackageVersion(
                        self.clean_version(current.version), current.timestamp
                    ),
                    [
                        PackageVersion(
                            self.clean_version(v.version), v.timestamp
                        )
                        for v in all_versions
                    ],
                )
                if current.version.startswith(
                    'v'
                ) and not earliest.version.startswith('v'):
                    earliest = PackageVersion(
                        f'v{earliest.version}', earliest.timestamp
                    )

            # Find location
            pattern = re.compile(
                r'(?:^|\s)' + re.escape(pkg_path) + r'(?:\s|$)'
            )
            location = find_location(self.root, rel_path, pattern)

            res = FreshnessResult(
                package=pkg_path,
                location=location,
                pkg_type=self.PKG_TYPE,
                current=current,
                earliest=earliest,
                tier=TIER2_DEVHOST,
            )
            await self._send_result(res)

        self.input_queue.put_nowait(None)

    async def _process_one(self, path: AsyncPath) -> None:
        pass
