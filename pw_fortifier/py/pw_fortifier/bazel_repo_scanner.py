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
"""Base package scanner for Bazel repository dependencies (Bzlmod repos)."""

import json
import os
from typing import Iterator

from pw_fortifier.bazel_scanner import BazelScanner


class BazelRepoScanner(BazelScanner):
    """Base class for Bazel repository package scanners."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._repos = {}  # canonical_name -> (repo_rule_name, owner)

    def _load_repos(
        self, module_bazel_path: str | os.PathLike[str]
    ) -> Iterator[tuple[str, list[dict]]]:
        """Loads repository mappings and attributes from MODULE.bazel."""
        self._repos = {}
        cwd = os.path.dirname(module_bazel_path)

        # 1. Run dump_repo_mapping
        try:
            result = self._run_bazelisk(['mod', 'dump_repo_mapping', ''], cwd)
            if result.returncode != 0:
                return
            mapping = json.loads(result.stdout)
        except (RuntimeError, json.JSONDecodeError):
            return

        root_canonical_name = mapping.get('')
        repo_names = []
        for k, v in mapping.items():
            if k == '' or v == root_canonical_name:
                continue
            repo_names.append(f'@{k}')

        if not repo_names:
            return

        # 2. Run show_repo in bulk
        try:
            result = self._run_bazelisk(
                ['mod', 'show_repo']
                + repo_names
                + ['--output=streamed_jsonproto'],
                cwd,
            )
            if result.returncode != 0:
                return
            show_repo_output = result.stdout
        except RuntimeError:
            return

        # Parse streamed JSON
        for line in show_repo_output.splitlines():
            if not line.strip():
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue

            canonical_name = obj.get('canonicalName', 'unknown')
            original_name = obj.get('originalName')
            if not original_name:
                continue

            owner = self._find_owners(
                module_bazel_path, 'use_repo', original_name
            )

            attributes = obj.get('attribute', [])
            repo_rule_name = obj.get('repoRuleName')

            self._repos[canonical_name] = (repo_rule_name, owner)
            yield canonical_name, attributes
