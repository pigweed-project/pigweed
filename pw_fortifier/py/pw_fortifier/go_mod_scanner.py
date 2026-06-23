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

from datetime import date, datetime
import json
import os
import re
import subprocess
from typing import Iterator


from pw_fortifier.package_scanner import (
    FreshnessScanResult,
    PackageScanner,
    PackageVersion,
    TIER2_DEVHOST,
)


def _parse_go_time(time_str: str) -> date | None:
    """Parses Go module timestamp (RFC3339) into a date."""
    if time_str.endswith('Z'):
        time_str = time_str[:-1] + '+00:00'
    try:
        dt = datetime.fromisoformat(time_str)
        return dt.date()
    except ValueError:
        return None


class GoModScanner(PackageScanner):
    """Scans Go modules in go.mod for freshness."""

    def __init__(self, root: str | os.PathLike[str] | None = None):
        super().__init__(root)
        self.strict_semver = True

    @staticmethod
    def _run_go(
        args: list[str], cwd: str | os.PathLike[str]
    ) -> subprocess.CompletedProcess:
        """Runs go command."""
        cmd = ['go'] + args
        try:
            result = subprocess.run(
                cmd,
                cwd=cwd,
                capture_output=True,
                text=True,
            )
            if result.returncode != 0:
                raise RuntimeError(
                    f'go failed: {result.stderr}. '
                    'You may need to activate the Pigweed environment.'
                )
            return result
        except FileNotFoundError as e:
            raise RuntimeError(
                'go command not found. Please ensure go is in your PATH.'
            ) from e

    def _run_go_list(
        self, args: list[str], cwd: str | os.PathLike[str]
    ) -> subprocess.CompletedProcess:
        """Runs go list command with json and module flags."""
        return self._run_go(['list', '-json', '-m'] + args, cwd)

    def _get_all_versions(
        self,
        pkg_path: str,
        versions_list: list[str],
        cwd: str | os.PathLike[str],
    ) -> list[PackageVersion]:
        """Queries go tool for details of all versions in versions_list."""
        all_versions = []
        for v in versions_list:
            try:
                v_res = self._run_go_list([f'{pkg_path}@{v}'], cwd)
                v_data = json.loads(v_res.stdout)
                v_time_str = v_data.get('Time')
                if v_time_str:
                    v_date = _parse_go_time(v_time_str)
                    if v_date:
                        all_versions.append(
                            PackageVersion(version=v, timestamp=v_date)
                        )
            except (json.JSONDecodeError, KeyError, RuntimeError):
                continue
        return all_versions

    def pre_scan(self) -> Iterator[FreshnessScanResult]:
        """Examines Go modules for freshness."""
        go_mod_path = os.path.join(self.root, 'go.mod')
        cwd = os.path.dirname(go_mod_path)
        rel_path = 'go.mod'

        try:
            with open(go_mod_path, 'r'):
                pass
        except IOError:
            return

        try:
            result = self._run_go_list(['all'], cwd)
            output = result.stdout
        except RuntimeError:
            return

        decoder = json.JSONDecoder()
        pos = 0
        while pos < len(output):
            # Skip leading whitespace
            while pos < len(output) and output[pos].isspace():
                pos += 1
            if pos >= len(output):
                break

            try:
                obj, index = decoder.raw_decode(output, pos)
            except json.JSONDecodeError:
                break
            pos = index

            if obj.get('Indirect', False) or obj.get('Main', False):
                continue

            pkg_path = obj.get('Path')
            version = obj.get('Version')
            time_str = obj.get('Time')
            if not pkg_path or not version or not time_str:
                continue

            key = f'{pkg_path}:{version}'
            if key in self._scanned_packages:
                continue
            self._scanned_packages.add(key)

            current_time = _parse_go_time(time_str)
            if not current_time:
                continue
            current = PackageVersion(version=version, timestamp=current_time)

            if current.version.startswith('v0.0.0-'):
                earliest = current
            else:
                try:
                    versions_res = self._run_go_list(
                        ['-versions', pkg_path], cwd
                    )
                    versions_list = json.loads(versions_res.stdout).get(
                        'Versions', []
                    )
                except (json.JSONDecodeError, KeyError, RuntimeError):
                    continue

                all_versions = self._get_all_versions(
                    pkg_path, versions_list, cwd
                )
                if not all_versions:
                    continue

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

            # Find owner
            pattern = re.compile(
                r'(?:^|\s)' + re.escape(pkg_path) + r'(?:\s|$)'
            )
            owner = self._find_owners(go_mod_path, pattern)

            yield FreshnessScanResult(
                package=pkg_path,
                source=rel_path,
                pkg_type='go_mod',
                current=current,
                earliest=earliest,
                tier=TIER2_DEVHOST,
                owner=owner,
            )
