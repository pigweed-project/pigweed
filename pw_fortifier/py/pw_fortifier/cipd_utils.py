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

import asyncio
from collections.abc import AsyncIterator
from datetime import datetime, timezone
import subprocess
from typing import NamedTuple, cast

from pw_fortifier.code_snippet import CodeSnippet
from pw_fortifier.package_analyzer import find_lowest
from pw_fortifier.freshness_result import FreshnessResult, PackageVersion


_PLATFORMS = [
    'linux-amd64',
    'linux-arm64',
    'mac-amd64',
    'mac-arm64',
    'windows-amd64',
]


def _split_platform(pkg: str) -> tuple[str, str | None]:
    parts = pkg.split('/')
    last = parts[-1]
    if last.startswith('${') or last in _PLATFORMS:
        return ('/'.join(parts[:-1]), last)
    return (pkg, None)


async def _run_cipd(args: list[str]) -> list[str]:
    """Runs cipd command, raising a helpful error if it fails."""
    cmd = ['cipd'] + args
    loop = asyncio.get_running_loop()

    def _run():
        return subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            check=True,
        )

    try:
        result = await loop.run_in_executor(None, _run)
    except FileNotFoundError as e:
        raise FileNotFoundError(
            '`cipd` command not found. '
            'You may need to activate the Pigweed environment.'
        ) from e
    except subprocess.CalledProcessError as e:
        cmd_str = ' '.join(cmd)
        raise RuntimeError(
            'cipd failed: You may need to activate the Pigweed environment or '
            f'run `cipd auth-login`. [cmd={cmd_str}]'
        ) from e

    return [l.rstrip() for l in result.stdout.splitlines() if l.strip()]


async def _run_cipd_instances(pkg: str) -> AsyncIterator[str]:
    """Runs `cipd instances` and yields instance IDs.

    Args:
        pkg: The CIPD package name.

    Returns:
        An async iterator yielding instance ID strings.
    """
    lines = await _run_cipd(['instances', pkg])
    for line in lines[2:]:
        parts = line.split()
        if not parts or parts[0] in ('...', '…') or parts[0].startswith('...'):
            continue
        yield parts[0]


async def _run_cipd_describe(
    pkg: str, version: str
) -> dict[str, str | dict[str, str]]:
    """Runs `cipd describe` and returns parsed output as a dict.

    Args:
        pkg: The CIPD package name.
        version: The version tag or instance ID reference.

    Returns:
        Dictionary containing the parsed describe fields.
    """
    parsed_data: dict[str, str | dict[str, str]] = {}
    current_dict: dict[str, str] | None = None

    for line in await _run_cipd(['describe', pkg, '--version', version]):
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


async def _has_version(pkg: str, version: str) -> bool:
    try:
        await _run_cipd_describe(pkg, version)
        return True
    except (subprocess.SubprocessError, RuntimeError):
        pass
    if ':' not in version:
        try:
            await _run_cipd_describe(pkg, f'git_revision:{version}')
            return True
        except (subprocess.SubprocessError, RuntimeError):
            pass
    return False


async def _get_package_version(pkg: str, ref: str) -> PackageVersion | None:
    """Gets the PackageVersion for a cipd package version reference."""
    desc_dict = await _run_cipd_describe(pkg, ref)
    reg_at = cast(str, desc_dict['Registered at'])

    parts = reg_at.split()
    assert len(parts) > 2
    date_str = parts[0]
    time_str = parts[1]
    offset_str = parts[2]

    # Strip fractional seconds (e.g. .092067)
    time_str_no_ms = time_str.split('.')[0]
    dt_str = f'{date_str} {time_str_no_ms} {offset_str}'
    dt = datetime.strptime(dt_str, '%Y-%m-%d %H:%M:%S %z')
    dt_utc = dt.astimezone(timezone.utc)
    timestamp = dt_utc.date()

    version = None
    for block_name in ('Tags', 'Metadata'):
        block = desc_dict.get(block_name)
        if isinstance(block, dict):
            if 'version' in block:
                version = block['version']
                break
            if 'git_revision' in block:
                version = f'git_revision:{block["git_revision"]}'
                break
            if 'git_revisions' in block:
                version = f'git_revisions:{block["git_revisions"]}'
                break
            if 'g3-revision' in block:
                version = f'g3-revision:{block["g3-revision"]}'
                break
            if 'g3_revision' in block:
                version = f'g3-revision:{block["g3_revision"]}'
                break

    if not version:
        return None

    return PackageVersion(version=version, timestamp=timestamp)


async def _get_package_versions(
    pkg: str, until: PackageVersion | None = None
) -> AsyncIterator[PackageVersion]:
    """Gets a list of PackageVersions for a CIPD package.

    Args:
        pkg: The CIPD package path.
        until: Optional PackageVersion to terminate early when reached.

    Yields:
        PackageVersion instances for the package instances.
    """
    async for instance_id in _run_cipd_instances(pkg):
        pv = await _get_package_version(pkg, instance_id)
        if pv is None:
            # Unversioned instance
            continue
        if until is not None and pv.timestamp < until.timestamp:
            break
        yield pv
        if until is not None and (pv.version == until.version or pv == until):
            break


async def _discover_platforms(base_pkg: str, version: str) -> list[str]:
    """Discovers available host platforms for a CIPD package."""
    platforms = []
    for p in await _run_cipd(['ls', base_pkg]):
        _, plat = _split_platform(p)
        if plat in _PLATFORMS and await _has_version(p, version):
            platforms.append(plat)
    return platforms


class _CipdEntry(NamedTuple):
    base_pkg: str
    platforms: list[str] | None
    version: str
    tier: int
    location: CodeSnippet | None = None


class CipdPackageSet:
    """Helper class to collect CIPD packages and produce FreshnessResults."""

    def __init__(self, rel_path: str, pkg_type: str) -> None:
        self._rel_path = rel_path
        self._pkg_type = pkg_type
        self._entries: list[_CipdEntry] = []

    async def add(
        self,
        pkg: str,
        version: str,
        tier: int,
        platforms: list[str] | None = None,
        location: CodeSnippet | None = None,
    ) -> None:
        """Adds a CIPD package entry to be analyzed.

        Args:
            pkg: CIPD package path or template string.
            version: Target version string.
            tier: Dependency tier integer.
            platforms: Optional list of explicit host platform strings.
            location: Optional CodeSnippet location for finding.
        """
        base_pkg, platform = _split_platform(pkg)

        if platform is None:
            platforms = None
        elif platform.startswith('${'):
            if platforms is None:
                platforms = await _discover_platforms(base_pkg, version)
        elif platforms is None:
            platforms = await _discover_platforms(base_pkg, version)
        else:
            base_pkg = pkg
            platforms = None

        self._entries.append(
            _CipdEntry(
                base_pkg=base_pkg,
                platforms=platforms,
                version=version,
                tier=tier,
                location=location,
            )
        )

    async def generate_results(
        self, scanned_packages: set[str]
    ) -> AsyncIterator[FreshnessResult]:
        """Generates FreshnessResult instances for gathered packages.

        Args:
            scanned_packages: Set of package keys to track duplicate packages.

        Yields:
            FreshnessResult objects for each gathered package.
        """
        for entry in self._entries:
            current: PackageVersion | None = None
            candidates: list[PackageVersion] = []
            if entry.platforms:
                pkgs = [f'{entry.base_pkg}/{plat}' for plat in entry.platforms]
                current = await _get_package_version(pkgs[0], entry.version)

                if not current:
                    print(f'FAILED to find `current` for {entry}')

                assert current
                async for v in _get_package_versions(pkgs[0], current):
                    all_platforms = True
                    for pkg in pkgs[1:]:
                        if not await _has_version(pkg, v.version):
                            all_platforms = False
                            break
                    if all_platforms:
                        candidates.append(v)
            else:
                current = await _get_package_version(
                    entry.base_pkg, entry.version
                )

                if not current:
                    print(f'FAILED to find `current` for {entry}')

                assert current
                candidates = [
                    v
                    async for v in _get_package_versions(
                        entry.base_pkg, current
                    )
                ]

            if candidates:
                earliest = find_lowest(entry.tier, current, candidates, True)
            else:
                earliest = current

            key = f'{entry.base_pkg}:{current.version}'
            if key in scanned_packages:
                continue
            scanned_packages.add(key)

            loc = entry.location or CodeSnippet(file=self._rel_path)

            yield FreshnessResult(
                package=entry.base_pkg,
                location=loc,
                pkg_type=self._pkg_type,
                current=current,
                earliest=earliest,
                tier=entry.tier,
            )
