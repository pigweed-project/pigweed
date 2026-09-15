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
"""Defines the CodeSnippet data structure for representing code locations."""

import os
import re
from typing import NamedTuple


class CodeSnippet(NamedTuple):
    """Represents a file or range of lines in a file containing code."""

    file: str | os.PathLike[str]
    lines: tuple[int, int] | None = None


def _matches_all(line: str, patterns: tuple[str | re.Pattern, ...]) -> bool:
    """Returns True if line matches all provided patterns."""
    for arg in patterns:
        if isinstance(arg, re.Pattern):
            if not arg.search(line):
                return False
        elif arg not in line:
            return False
    return True


def find_location(
    root_path: str | os.PathLike[str],
    file_path: str | os.PathLike[str],
    *args: str | re.Pattern,
) -> CodeSnippet:
    """Creates a CodeSnippet for file_path with relative path and lines.

    Args:
        root_path: Path to the repository root.
        file_path: Path to the target file (relative or absolute).
        *args: Substrings or Patterns to search for in lines.

    Returns:
        CodeSnippet instance.
    """
    if os.path.isabs(file_path):
        rel_path = os.path.relpath(file_path, root_path)
    else:
        rel_path = str(file_path)

    lines = None
    if args:
        abs_path = os.path.normpath(os.path.join(root_path, rel_path))
        try:
            with open(abs_path, 'r') as f:
                file_lines = f.readlines()
            for idx, line in enumerate(file_lines):
                if _matches_all(line, args):
                    lines = (idx + 1, idx + 1)
                    break
        except IOError:
            pass

    return CodeSnippet(file=rel_path, lines=lines)
