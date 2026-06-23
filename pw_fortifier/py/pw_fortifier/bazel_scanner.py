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

import os
import subprocess

from pw_fortifier.package_scanner import (
    PackageScanner,
)


def get_attr_str(attributes: list[dict], name: str) -> str:
    """Finds attribute and returns stringValue or empty string."""
    for attr in attributes:
        if attr.get('name') == name:
            return attr.get('stringValue', '')
    return ''


def get_attr_str_list(attributes: list[dict], name: str) -> list[str]:
    """Finds attribute and returns stringListValue or empty list."""
    for attr in attributes:
        if attr.get('name') == name:
            return attr.get('stringListValue', [])
    return []


def get_attr_str_dict(attributes: list[dict], name: str) -> dict[str, str]:
    """Finds attribute and returns stringDictValue as python dict."""
    for attr in attributes:
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


class BazelScanner(PackageScanner):
    """Base class for Bazel package scanners."""

    @staticmethod
    def _run_bazelisk(
        args: list[str], cwd: str | os.PathLike[str]
    ) -> subprocess.CompletedProcess:
        """Runs bazelisk command."""
        cmd = ['bazelisk'] + args
        try:
            return subprocess.run(
                cmd,
                cwd=cwd,
                capture_output=True,
                text=True,
            )
        except FileNotFoundError as e:
            raise RuntimeError(
                'bazelisk command not found. '
                'Please ensure bazelisk is in your PATH.'
            ) from e
