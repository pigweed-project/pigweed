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

from datetime import date
from email.utils import parsedate_to_datetime
import json
import logging
import os
from typing import Iterator
import xml.etree.ElementTree as ET
import requests

from pw_fortifier.bazel_repo_scanner import BazelRepoScanner
from pw_fortifier.bazel_scanner import get_attr_str_list
from pw_fortifier.package_scanner import (
    FreshnessScanResult,
    PackageVersion,
    TIER2_DEVHOST,
)

_LOG = logging.getLogger(__name__)


class BazelMavenScanner(BazelRepoScanner):
    """Scans Maven dependencies in MODULE.bazel for freshness."""

    @staticmethod
    def _get_timestamp(
        base_url: str, artifact: str, version: str
    ) -> date | None:
        """Gets the timestamp from the Last-Modified header of the POM file."""
        url = (
            f'{base_url.rstrip("/")}/{artifact}/{version}/'
            f'{artifact}-{version}.pom'
        )
        if not (url.startswith('https://') or url.startswith('http://')):
            url = f'https://{url}'

        try:
            resp = requests.head(url, timeout=10)
            if resp.status_code != 200:
                _LOG.warning(
                    'HEAD request to %s failed with status %d',
                    url,
                    resp.status_code,
                )
                return None

            last_modified = resp.headers.get('Last-Modified')
            if last_modified:
                dt = parsedate_to_datetime(last_modified)
                return dt.date()
        except (requests.RequestException, ValueError, TypeError) as e:
            _LOG.warning('HEAD request to %s failed: %s', url, e)

        return None

    def _resolve_artifact(
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

        for repo_url in repo_urls:
            base_url = f'{repo_url.rstrip("/")}/{url_path.strip("/")}'
            metadata_url = f'{base_url}/{artifact}/maven-metadata.xml'
            if not (
                metadata_url.startswith('https://')
                or metadata_url.startswith('http://')
            ):
                metadata_url = f'https://{metadata_url}'

            try:
                resp = requests.get(metadata_url, timeout=10)
                if resp.status_code != 200:
                    continue
                root = ET.fromstring(resp.content)
            except (requests.RequestException, ET.ParseError):
                continue

            current_time = self._get_timestamp(base_url, artifact, version)
            if not current_time:
                continue
            current = PackageVersion(version=version, timestamp=current_time)

            versions_elem = root.find('versioning/versions')
            if versions_elem is None:
                continue

            versions_list = [
                v_elem.text.strip()
                for v_elem in versions_elem.findall('version')
                if v_elem.text
            ]

            all_versions = []
            for v in versions_list:
                v_time = self._get_timestamp(base_url, artifact, v)
                if v_time:
                    all_versions.append(
                        PackageVersion(version=v, timestamp=v_time)
                    )

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
            try:
                repo_obj = json.loads(repo_str)
                url = repo_obj.get('repo_url')
                if url:
                    repo_urls.append(url)
            except json.JSONDecodeError:
                pass

        # Prefer Maven Central
        preferred_repo = 'https://repo1.maven.org/maven2'
        if preferred_repo in repo_urls:
            repo_urls.remove(preferred_repo)
            repo_urls.insert(0, preferred_repo)

        return repo_urls

    def pre_scan(self) -> Iterator[FreshnessScanResult]:
        """Examines Maven dependencies for freshness."""
        module_bazel_path = os.path.join(self.root, 'MODULE.bazel')
        rel_path = 'MODULE.bazel'

        for _canonical_name, attributes in self._load_repos(module_bazel_path):
            # Retrieve repositories and artifacts attributes
            repo_urls_raw = get_attr_str_list(attributes, 'repositories')
            artifacts_raw = get_attr_str_list(attributes, 'artifacts')
            if not repo_urls_raw or not artifacts_raw:
                continue

            # Parse repo URLs
            repo_urls = self._parse_repo_urls(repo_urls_raw)
            if not repo_urls:
                continue

            # Parse artifacts mapping: artifact -> (group, version)
            artifacts_dict = {}
            for art_str in artifacts_raw:
                try:
                    art_obj = json.loads(art_str)
                    art_name = art_obj.get('artifact')
                    group = art_obj.get('group')
                    version = art_obj.get('version')
                    if art_name and group and version:
                        artifacts_dict[art_name] = (group, version)
                except json.JSONDecodeError:
                    pass

            # Process each artifact
            for artifact, (group, version) in artifacts_dict.items():
                package = f'{group}:{artifact}'
                key = f'{package}:{version}'
                if key in self._scanned_packages:
                    continue
                self._scanned_packages.add(key)

                pkg_type = 'bazel_maven'

                earliest, current = self._resolve_artifact(
                    group, artifact, version, repo_urls
                )

                if earliest is None or current is None:
                    _LOG.warning(
                        'Could not resolve metadata for artifact %s', package
                    )
                    continue

                owner = self._find_owners(module_bazel_path, package)

                yield FreshnessScanResult(
                    package=package,
                    source=rel_path,
                    pkg_type=pkg_type,
                    current=current,
                    earliest=earliest,
                    tier=TIER2_DEVHOST,
                    owner=owner,
                )
