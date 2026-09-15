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
from pw_fortifier.async_path import AsyncPath
from pw_fortifier.code_snippet import CodeSnippet
from pw_fortifier.git_utils import ReadOnlyGitWorkspace
from pw_fortifier.package_analyzer import PackageAnalyzer, TIER0_ON_DEVICE
from pw_fortifier.freshness_result import FreshnessResult, PackageVersion


def _parse_copybara_content(
    content: str, filename: str = '<string>'
) -> str | None:
    """Parses copy.bara.sky content and returns the git.origin URL."""
    tree = ast.parse(content, filename=filename)

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


PKG_TYPE: str = 'copybara'


class CopybaraAnalyzer(PackageAnalyzer):
    """Scans Copybara packages for freshness."""

    PKG_TYPE = PKG_TYPE
    TARGET = 'copy.bara.sky'

    async def _process_one(self, path: AsyncPath) -> None:
        """Examines Copybara packages for freshness."""
        rel_path = os.path.relpath(path.path, self.root)

        # Extract project name from path
        normalized_rel_path = rel_path.replace(os.sep, '/')
        match = re.match(
            r'^(?:(.*)/)?([^/]+)/copy\.bara\.sky$', normalized_rel_path
        )
        if not match:
            raise ValueError(
                f"Cannot determine project name from copybara path '{rel_path}'"
            )
        project_dir = match.group(1)
        project = match.group(2)

        content = await path.read_text()
        src_url = _parse_copybara_content(content, filename=str(path))

        if not src_url:
            raise ValueError(f"No git origin URL found in '{path}'")

        # Run git log on the local repo to find the last imported version
        if project_dir:
            repo_rel_path = f'{project_dir}/{project}/repo'
        else:
            repo_rel_path = f'{project}/repo'

        local_repo = ReadOnlyGitWorkspace(project_dir=self.root)
        local_versions = [
            PackageVersion(v, t)
            for v, t in local_repo.get_versions(
                num=1,
                pattern='GitOrigin-RevId',
                scope=repo_rel_path,
            )
        ]
        if not local_versions:
            raise RuntimeError(
                f"No GitOrigin-RevId commits found for '{project}' "
                f"under '{repo_rel_path}'"
            )
        current = local_versions[0]

        key = f'{project}:{current.version}'
        if key in self._scanned_packages:
            return
        self._scanned_packages.add(key)

        # Clone upstream and get versions
        upstream_repo = await ReadOnlyGitWorkspace.clone(
            src_url, timestamp=current.timestamp
        )
        versions_list = [
            PackageVersion(v, t) for v, t in upstream_repo.get_versions()
        ]

        if not versions_list:
            raise RuntimeError(
                f"Failed to resolve any versions for '{project}' from "
                f"'{src_url}'"
            )

        earliest = self.find_lowest(TIER0_ON_DEVICE, current, versions_list)

        location = CodeSnippet(file=rel_path)

        res = FreshnessResult(
            package=project,
            location=location,
            pkg_type=self.PKG_TYPE,
            current=current,
            earliest=earliest,
            tier=TIER0_ON_DEVICE,
        )
        await self._send_result(res)
