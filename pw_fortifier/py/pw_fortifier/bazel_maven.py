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
"""Package scanner for Maven dependencies in Bazel (Bzlmod)."""

import argparse
import asyncio
from datetime import date
from email.utils import parsedate_to_datetime
import json
import logging
import os
import xml.etree.ElementTree as ET
import requests

from pw_fortifier.async_path import AsyncPath
from pw_fortifier.bazelisk_utils import BazelRepo
from pw_fortifier.code_snippet import find_location
from pw_fortifier.package_analyzer import PackageAnalyzer
from pw_fortifier.freshness_result import (
    FreshnessResult,
    PackageVersion,
    TIER2_DEVHOST,
)

_LOG = logging.getLogger(__name__)


def _fetch_url(url: str) -> requests.Response:
    return requests.get(url, timeout=10)


PKG_TYPE: str = 'bazel_maven'


class BazelMavenAnalyzer(PackageAnalyzer):
    """Scans Maven dependencies in MODULE.bazel for freshness."""

    PKG_TYPE = PKG_TYPE

    def __init__(self) -> None:
        super().__init__()
        self.loose_semver = True

    async def configure(self, args: argparse.Namespace) -> None:
        """Configures the analyzer and determines if setup can be skipped."""
        await super().configure(args)
        if args.files is None:
            return
        if 'MODULE.bazel' in args.files:
            return
        self.skip_setup = True

    @staticmethod
    async def _get_timestamp(
        base_url: str, artifact: str, version: str
    ) -> date:
        """Gets the timestamp from the Last-Modified header of the POM file."""
        url = (
            f'{base_url.rstrip("/")}/{artifact}/{version}/'
            f'{artifact}-{version}.pom'
        )
        if not (url.startswith('https://') or url.startswith('http://')):
            url = f'https://{url}'

        loop = asyncio.get_running_loop()
        resp = await loop.run_in_executor(
            None,
            lambda: requests.head(url, timeout=10),
        )
        if resp.status_code != 200:
            resp.raise_for_status()
            raise requests.exceptions.HTTPError(
                f'HEAD request to {url} failed with status '
                f'{resp.status_code}',
                response=resp,
            )

        last_modified = resp.headers.get('Last-Modified')
        assert last_modified

        dt = parsedate_to_datetime(last_modified)
        return dt.date()

    async def _resolve_artifact(
        self,
        group: str,
        artifact: str,
        version: str,
        repo_urls: list[str],
    ) -> tuple[PackageVersion | None, PackageVersion | None]:
        """Resolves the earliest and current PackageVersion for an artifact."""
        url_path = group.replace('.', '/')
        earliest = None
        current = None

        loop = asyncio.get_running_loop()
        for repo_url in repo_urls:
            # Fetch metadata from one of the reops.
            base_url = f'{repo_url.rstrip("/")}/{url_path.strip("/")}'
            metadata_url = f'{base_url}/{artifact}/maven-metadata.xml'
            if not (
                metadata_url.startswith('https://')
                or metadata_url.startswith('http://')
            ):
                metadata_url = f'https://{metadata_url}'

            resp = await loop.run_in_executor(
                None,
                _fetch_url,
                metadata_url,
            )
            if resp.status_code != 200:
                resp.raise_for_status()
                raise requests.exceptions.HTTPError(
                    f'GET request to {metadata_url} failed with status '
                    f'{resp.status_code}',
                    response=resp,
                )
            root = ET.fromstring(resp.content)

            # Get the current version and timestamp info for the artifact.
            current_time = await self._get_timestamp(
                base_url, artifact, version
            )
            current = PackageVersion(version=version, timestamp=current_time)

            #  Get the list of available versions.
            versions_elem = root.find('versioning/versions')
            if versions_elem is None:
                continue

            versions_list = [
                v_elem.text.strip()
                for v_elem in versions_elem.findall('version')
                if v_elem.text
            ]

            #  Get the corresponding timestamp for each version.
            all_versions = []
            for v in versions_list:
                v_time = await self._get_timestamp(base_url, artifact, v)
                all_versions.append(PackageVersion(version=v, timestamp=v_time))

            if not all_versions:
                continue

            earliest = self.find_lowest(TIER2_DEVHOST, current, all_versions)
            break  # Stop querying other repos if resolved successfully

        return earliest, current

    @staticmethod
    def _parse_repo_urls(repo_urls_raw: list[str]) -> list[str]:
        """Parses repository URLs and prioritizes Maven Central."""
        repo_urls = []
        for repo_str in repo_urls_raw:
            repo_obj = json.loads(repo_str)
            url = repo_obj.get('repo_url')
            if url:
                repo_urls.append(url)

        # Prefer Maven Central
        preferred_repo = 'https://repo1.maven.org/maven2'
        if preferred_repo in repo_urls:
            repo_urls.remove(preferred_repo)
            repo_urls.insert(0, preferred_repo)

        return repo_urls

    async def _set_up(self) -> None:
        """Examines Maven dependencies for freshness."""
        module_bazel_path = os.path.join(self.root, 'MODULE.bazel')
        rel_path = 'MODULE.bazel'

        async for repo in BazelRepo.load(module_bazel_path):
            # MAven rules have repositories and artifacts attributes
            repo_urls_raw = repo.get_attr_str_list('repositories')
            artifacts_raw = repo.get_attr_str_list('artifacts')
            if not repo_urls_raw or not artifacts_raw:
                continue

            # Parse repo URLs
            repo_urls = self._parse_repo_urls(repo_urls_raw)
            if not repo_urls:
                raise ValueError(
                    f'No repository URLs could be parsed from {repo_urls_raw}'
                )

            # Parse artifacts mapping: artifact -> (group, version)
            artifacts_dict = {}
            for art_str in artifacts_raw:
                art_obj = json.loads(art_str)
                art_name = art_obj.get('artifact')
                group = art_obj.get('group')
                version = art_obj.get('version')
                if art_name and group and version:
                    artifacts_dict[art_name] = (group, version)

            # Process each artifact
            for artifact, (group, version) in artifacts_dict.items():
                package = f'{group}:{artifact}'
                key = f'{package}:{version}'
                if key in self._scanned_packages:
                    continue
                self._scanned_packages.add(key)

                earliest, current = await self._resolve_artifact(
                    group, artifact, version, repo_urls
                )

                if earliest is None or current is None:
                    _LOG.warning(
                        'Could not resolve metadata for artifact %s', package
                    )
                    continue

                location = find_location(self.root, rel_path, package)

                res = FreshnessResult(
                    package=package,
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
