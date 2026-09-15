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
"""Base package scanner for Bazel-based dependencies."""

import asyncio
from collections.abc import AsyncIterator
import json
import logging
import os
import subprocess
from typing import NamedTuple

from pw_fortifier.code_snippet import CodeSnippet, find_location

_LOG = logging.getLogger(__name__)


async def run_bazelisk(
    args: list[str], cwd: str | os.PathLike[str]
) -> subprocess.CompletedProcess:
    """Runs a bazelisk command asynchronously in an executor.

    Args:
        args: List of command-line arguments for the bazelisk command.
        cwd: Working directory where the command is executed.

    Returns:
        A subprocess.CompletedProcess instance containing execution results.
    """
    cmd = ['bazelisk'] + args
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
            'bazelisk command not found. '
            'Please ensure bazelisk is in your PATH.'
        ) from e


class BazelRepo(NamedTuple):
    """Represents a Bazel repository and its metadata from MODULE.bazel."""

    canonical_name: str
    """Canonical repository name string."""

    rule_name: str | None
    """Optional repository rule name string."""

    location: CodeSnippet | None
    """Optional location code snippet."""

    attributes: list[dict]
    """List of attribute dictionaries parsed from show_repo."""

    assignee: str | None = None
    """Optional legacy assignee field."""

    def get_attr_str(self, name: str) -> str:
        """Finds an attribute and returns stringValue or empty string.

        Args:
            name: Name of the attribute to find.

        Returns:
            The stringValue if found, or empty string.
        """
        for attr in self.attributes:
            if attr.get('name') == name:
                return attr.get('stringValue', '')
        return ''

    def get_attr_str_list(self, name: str) -> list[str]:
        """Finds an attribute and returns stringListValue or empty list.

        Args:
            name: Name of the attribute to find.

        Returns:
            The stringListValue if found, or an empty list.
        """
        for attr in self.attributes:
            if attr.get('name') == name:
                return attr.get('stringListValue', [])
        return []

    def get_attr_str_dict(self, name: str) -> dict[str, str]:
        """Finds an attribute and returns stringDictValue as a dict.

        Args:
            name: Name of the attribute to find.

        Returns:
            The dictionary of attribute keys and values.
        """
        for attr in self.attributes:
            if attr.get('name') == name:
                raw = attr.get('stringDictValue')
                if not raw:
                    return {}
                if isinstance(raw, list):
                    return {
                        entry.get('key'): entry.get('value')
                        for entry in raw
                        if 'key' in entry and 'value' in entry
                    }
                if isinstance(raw, dict):
                    return raw
        return {}

    @staticmethod
    async def load(
        module_bazel_path: str | os.PathLike[str],
    ) -> AsyncIterator['BazelRepo']:
        """Loads repository mappings and attributes from MODULE.bazel.

        Args:
            module_bazel_path: Path to the MODULE.bazel file to parse.

        Yields:
            BazelRepo instances for each discovered external repository.
        """
        cwd = os.path.dirname(module_bazel_path)

        # 1. Run dump_repo_mapping
        result = await run_bazelisk(
            ['mod', 'dump_repo_mapping', '--lockfile_mode=off', ''],
            cwd,
        )
        mapping = json.loads(result.stdout)

        root_canonical_name = mapping.get('')
        repo_names = []
        for k, v in mapping.items():
            if k == '' or v == root_canonical_name:
                continue
            repo_names.append(f'@{k}')

        if not repo_names:
            return

        # 2. Run show_repo in bulk
        result = await run_bazelisk(
            ['mod', 'show_repo', '--lockfile_mode=off']
            + repo_names
            + ['--output=streamed_jsonproto'],
            cwd,
        )
        show_repo_output = result.stdout

        # Parse streamed JSON
        for line in show_repo_output.splitlines():
            if not line.strip():
                continue
            obj = json.loads(line)

            # Canonical naames can be mapped to repo names, e.g.
            #   aspect_bazel_lib+                 => aspect_bazel_lib
            #   +_repo_rules2+bazel_clang_tidy    => bazel_clang_tidy
            #   rules_python++python+pythons_hub  => pythons_hub
            canonical_name = obj.get('canonicalName')
            assert canonical_name
            repo_name = canonical_name.rstrip('+').split('+')[-1]

            # Find where the version is set. Skip if not set by MODULE.bazel
            rule_name = obj.get('repoRuleName')
            location = find_location(cwd, module_bazel_path, repo_name)
            if not location.lines:
                continue
            attributes = obj.get('attribute', [])

            yield BazelRepo(canonical_name, rule_name, location, attributes)
