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
"""Package scanner for CIPD repositories in Bazel."""

import logging
import os
from typing import Iterator

from pw_fortifier.bazel_repo_scanner import BazelRepoScanner
from pw_fortifier.bazel_scanner import get_attr_str, get_attr_str_dict
from pw_fortifier.cipd_scanner import CipdScanner
from pw_fortifier.package_scanner import (
    FreshnessScanResult,
    TIER1_TOOLCHAIN,
)

_LOG = logging.getLogger(__name__)


class BazelCipdScanner(CipdScanner, BazelRepoScanner):
    """Scans CIPD repositories in MODULE.bazel for freshness."""

    def _add_cipd_repository(
        self, attributes: list[dict], canonical_name: str, owner: str | None
    ) -> None:
        """Processes cipd_repository attributes and updates _packages_dict."""
        cipd_tag = get_attr_str(attributes, 'tag')
        if not cipd_tag:
            _LOG.warning(
                'the version of %s couldn\'t be determined', canonical_name
            )
            return

        cipd_pkg = get_attr_str(attributes, 'package')
        cipd_path = get_attr_str(attributes, 'path')

        if cipd_pkg:
            self._packages_dict[cipd_pkg] = (cipd_tag, TIER1_TOOLCHAIN)
            self._owners[cipd_pkg] = owner
        elif cipd_path:
            added_pkgs = self._add_cipd_path(
                cipd_path, cipd_tag, TIER1_TOOLCHAIN
            )
            for pkg in added_pkgs:
                self._owners[pkg] = owner
        else:
            _LOG.warning(
                'the CIPD path of %s couldn\'t be determined', canonical_name
            )

    def _add_package_repo(
        self, attributes: list[dict], _canonical_name: str, owner: str | None
    ) -> None:
        """Processes package_repo attributes and updates _packages_dict."""
        packages = get_attr_str_dict(attributes, 'packages')
        for cipd_path, cipd_tag in packages.items():
            if not cipd_path or not cipd_tag:
                continue

            added_pkgs = self._add_cipd_path(
                cipd_path, cipd_tag, TIER1_TOOLCHAIN
            )
            for pkg in added_pkgs:
                self._owners[pkg] = owner

    def pre_scan(self) -> Iterator[FreshnessScanResult]:
        """Examines CIPD repositories for freshness."""
        self._packages_dict = {}
        self._owners = {}
        module_bazel_path = os.path.join(self.root, 'MODULE.bazel')
        rel_path = 'MODULE.bazel'

        for canonical_name, attributes in self._load_repos(module_bazel_path):
            repo_rule_name, owner = self._repos[canonical_name]

            if repo_rule_name == 'cipd_repository':
                self._add_cipd_repository(attributes, canonical_name, owner)
            elif repo_rule_name == 'package_repo':
                self._add_package_repo(attributes, canonical_name, owner)

        # Generate results
        yield from self._generate_results(rel_path, 'bazel_cipd')
