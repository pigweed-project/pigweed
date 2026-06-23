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
"""Package scanner for Copybara packages."""

import ast
import os
import re
from typing import Iterator

from pw_fortifier.find_core_owners import CoreOwnerFinder
from pw_fortifier.git_repo_scanner import GitRepoScanner
from pw_fortifier.package_scanner import (
    FreshnessScanResult,
    TIER0_ON_DEVICE,
)


def _parse_copybara_url(filepath: str | os.PathLike[str]) -> str | None:
    """Parses copy.bara.sky and returns the git.origin URL."""
    try:
        with open(filepath, 'r') as f:
            tree = ast.parse(f.read(), filename=str(filepath))
    except (IOError, SyntaxError):
        return None

    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue

        func = node.func
        if not (
            isinstance(func, ast.Attribute)
            and isinstance(func.value, ast.Name)
            and func.value.id == 'core'
            and func.attr == 'workflow'
        ):
            continue

        for kw in node.keywords:
            if kw.arg != 'origin':
                continue

            origin_val = kw.value
            if not isinstance(origin_val, ast.Call):
                continue

            origin_func = origin_val.func
            if not (
                isinstance(origin_func, ast.Attribute)
                and isinstance(origin_func.value, ast.Name)
                and origin_func.value.id == 'git'
                and origin_func.attr == 'origin'
            ):
                continue

            for origin_kw in origin_val.keywords:
                if origin_kw.arg != 'url':
                    continue

                url_val = origin_kw.value
                if isinstance(url_val, ast.Constant) and isinstance(
                    url_val.value, str
                ):
                    return url_val.value

    return None


class CopybaraScanner(GitRepoScanner):
    """Scans Copybara packages for freshness."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)

    def target(self) -> str:
        return 'copy.bara.sky'

    def scan(
        self, pathname: str | os.PathLike[str]
    ) -> Iterator[FreshnessScanResult]:
        """Examines Copybara packages for freshness."""
        copy_bara_sky_path = os.path.join(self.root, pathname)
        rel_path = os.fspath(pathname)

        # Extract project name from path
        normalized_rel_path = rel_path.replace(os.sep, '/')
        match = re.match(
            r'^(?:(.*)/)?([^/]+)/copy\.bara\.sky$', normalized_rel_path
        )
        if not match:
            return
        project_dir = match.group(1)
        project = match.group(2)

        src_url = _parse_copybara_url(copy_bara_sky_path)

        if not src_url:
            return

        # Run git log on the local repo to find the last imported version
        if project_dir:
            repo_rel_path = f'{project_dir}/{project}/repo'
        else:
            repo_rel_path = f'{project}/repo'
        local_versions = list(
            self.get_versions(
                num=1,
                pattern='GitOrigin-RevId',
                scope=repo_rel_path,
            )
        )
        if not local_versions:
            return
        current = local_versions[0]

        key = f'{project}:{current.version}'
        if key in self._scanned_packages:
            return
        self._scanned_packages.add(key)

        # Clone upstream and get versions
        try:
            upstream_scanner = GitRepoScanner.clone(src_url, current.timestamp)
            versions_list = list(upstream_scanner.get_versions())
        except RuntimeError:
            return

        if not versions_list:
            return

        earliest = self.find_lowest(TIER0_ON_DEVICE, current, versions_list)

        # Find owner using CoreOwnerFinder on copy.bara.sky
        finder = CoreOwnerFinder(root=self.root)
        finder.add(copy_bara_sky_path)
        owner = finder.find()

        yield FreshnessScanResult(
            package=project,
            source=rel_path,
            pkg_type='copybara',
            current=current,
            earliest=earliest,
            tier=TIER0_ON_DEVICE,
            owner=owner,
        )
