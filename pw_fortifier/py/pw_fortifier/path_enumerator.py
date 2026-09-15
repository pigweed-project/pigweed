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
"""Defines the PathEnumerator types for enumerating paths in a project."""

import fnmatch
import os
from pathlib import Path
from typing import Iterator

import pathspec

from pw_fortifier.git_utils import ReadOnlyGitWorkspace


class PathEnumerator:
    """Enumerates files in a Git repository matching one or more patterns."""

    def __init__(
        self,
        src_repo: ReadOnlyGitWorkspace,
        include_files: bool = True,
        include_dirs: bool = False,
    ) -> None:
        """Initializes the enumerator with a project repository workspace.

        Args:
            src_repo: The ReadOnlyGitWorkspace of the project repository.
            include_files: If True, enumerated paths will include files.
            include_dirs: If True, enumerated paths will include directories.
        """
        assert include_files or include_dirs
        self._src_repo = src_repo
        self._include_files = include_files
        self._include_dirs = include_dirs
        self._match_patterns: list[str] = []
        self._gitignore_spec: pathspec.PathSpec | None = None

        gitignore_path = self._src_repo.project_dir / '.gitignore'
        if gitignore_path.exists():
            with open(gitignore_path, 'r', encoding='utf-8') as f:
                self._gitignore_spec = pathspec.PathSpec.from_lines(
                    'gitignore', f
                )

    @property
    def src_repo(self) -> ReadOnlyGitWorkspace:
        """Returns the project repository workspace.

        Returns:
            The ReadOnlyGitWorkspace instance.
        """
        return self._src_repo

    def match(self, pattern: str | os.PathLike[str]) -> None:
        """Adds the given pattern to those that will be yielded.

        Args:
            pattern: Root-relative path, may contain wildcards.
        """
        root = self._src_repo.project_dir
        norm_pattern = os.path.normpath(pattern)
        path = Path(norm_pattern)
        if path.is_absolute() and path.is_relative_to(root):
            path = path.relative_to(root)
        self._match_patterns.append(os.fspath(path))

    @staticmethod
    def _match_any(path: Path, patterns: list[str]) -> bool:
        """Checks if a root-relative path matches any pattern in a list."""
        path_str = str(path)
        for pattern in patterns:
            if fnmatch.fnmatch(path_str, pattern):
                return True
        return False

    def _pruned(self, path: Path, is_dir: bool = False) -> bool:
        """Returns True if the path should be pruned."""
        if '.git' in path.parts:
            return True
        if self._gitignore_spec is None:
            return False
        posix_path = path.as_posix()
        if self._gitignore_spec.match_file(posix_path):
            return True
        if is_dir and self._gitignore_spec.match_file(f'{posix_path}/'):
            return True
        return False

    def _matches(self, path: Path) -> bool:
        """Returns whether the path should be yielded if not pruned."""
        return not self._match_patterns or self._match_any(
            path, self._match_patterns
        )

    def __iter__(self) -> Iterator[Path]:
        """Walks the project yielding matching root-relative paths.

        Yields:
            Matching root-relative Path instances.
        """
        root = self._src_repo.project_dir
        for cwd, dirs, files in os.walk(root, followlinks=False):
            surviving_dirs = []
            for d in dirs:
                rel_d = Path(cwd, d).relative_to(root)
                if not self._pruned(rel_d, is_dir=True):
                    surviving_dirs.append(d)
            dirs[:] = surviving_dirs

            paths: list[Path] = []
            if self._include_dirs:
                paths = [Path(cwd, d).relative_to(root) for d in dirs]

            if self._include_files:
                for f in files:
                    rel_f = Path(cwd, f).relative_to(root)
                    if not self._pruned(rel_f, is_dir=False):
                        paths.append(rel_f)

            for p in paths:
                if self._matches(p):
                    yield p


################################################################################
# Test support


class FakePathEnumerator(PathEnumerator):
    """A fake PathEnumerator that returns predefined paths."""

    def __init__(
        self,
        files: list[Path],
        src_repo: ReadOnlyGitWorkspace | None = None,
    ) -> None:
        """Initializes the fake enumerator with a list of files.

        Args:
            files: List of Path objects to return.
            src_repo: Optional ReadOnlyGitWorkspace instance.
        """
        super().__init__(
            src_repo=src_repo or ReadOnlyGitWorkspace(Path('.').resolve())
        )
        self._files = files
        self.patterns: list[str] = []

    def match(self, pattern: str | os.PathLike[str]) -> None:
        """Adds a target pattern to match.

        Args:
            pattern: Pattern string or Path object.
        """
        self.patterns.append(os.fspath(pattern))

    def __iter__(self) -> Iterator[Path]:
        """Iterates over the predefined files.

        Yields:
            Predefined Path instances.
        """
        yield from self._files
