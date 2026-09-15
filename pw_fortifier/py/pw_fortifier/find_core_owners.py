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
"""Utility to find core owners (or any owners) for code snippets.

This utility analyzes git history to find the team members who have most
recently and frequently modified specific files or line ranges. It filters out
large-scale changes (LSCs) and restrict the search by default to core
team members listed in the root OWNERS file.
"""

import argparse
import os
from pathlib import Path
import re
import sys
from pw_fortifier.code_snippet import CodeSnippet, find_location
from pw_fortifier.git_utils import ReadOnlyGitWorkspace


def parse_owners_file(path: str | os.PathLike[str]) -> list[str]:
    """Parses an OWNERS file and returns the list of emails found.

    Args:
        path: Path to the OWNERS file.

    Returns:
        List of email address strings parsed from the file.
    """
    members = []
    with open(path, 'r') as f:
        for line in f:
            line = line.strip()
            if (
                not line
                or line.startswith('#')
                or line.startswith('include')
                or line.startswith('per-file')
            ):
                continue

            # Parse member: email #{annotation}
            parts = line.split('#', 1)
            email_part = parts[0].strip()
            if email_part and '@' in email_part:
                members.append(email_part)
    return members


class CoreOwnerFinder:
    """Finds the core assignee for a set of code snippets."""

    def __init__(self, repo: ReadOnlyGitWorkspace):
        self.repo = repo
        self.root = str(repo.project_dir)
        self.owners_path = os.path.join(self.root, 'OWNERS')
        self._snippets: list[CodeSnippet] = []

    def core_members(self) -> set[str]:
        """Gets core team members from the OWNERS file in the repo root.

        Returns:
            Set of email addresses of core team members.
        """
        return set(parse_owners_file(self.owners_path))

    def add(
        self,
        file: str | os.PathLike[str] | CodeSnippet,
        lines: tuple[int, int] | None = None,
    ) -> None:
        """Adds a file or file range to be examined.

        Args:
            file: Path to the file or a CodeSnippet instance.
            lines: Optional inclusive line range tuple (start, end).
        """
        if isinstance(file, CodeSnippet):
            self._snippets.append(file)
        else:
            self._snippets.append(CodeSnippet(file=file, lines=lines))

    def _abs_path(self, path: str | os.PathLike[str]) -> str:
        """Resolves path to absolute relative to repo root."""
        if os.path.isabs(path):
            return os.path.abspath(path)
        return os.path.abspath(os.path.join(self.root, path))

    def find(self, any_owner: bool = False) -> str | None:
        """Finds the core team member who most modified the added snippets.

        Args:
            any_owner: Whether to allow any author or only core team members.

        Returns:
            The chosen assignee email address, or None.
        """
        cores = self.core_members() if not any_owner else None
        author_counts: dict[str, int] = {}

        for snippet in self._snippets:
            abs_file = Path(self._abs_path(snippet.file)).resolve()
            root_path = Path(self.root).resolve()
            if not abs_file.is_relative_to(root_path):
                raise ValueError(
                    f"File '{snippet.file}' is outside repository '{self.root}'"
                )
            rel_file = str(abs_file.relative_to(root_path))

            revision_line = self._find_nonlsc_revision(rel_file)
            if not revision_line:
                continue
            commit_hash = revision_line.split()[0]

            blame_lines = self.repo.blame(
                file=rel_file, commit=commit_hash, lines=snippet.lines
            )

            for line in blame_lines:
                author = _extract_author(line)
                if author and (
                    any_owner or (cores is not None and author in cores)
                ):
                    author_counts[author] = author_counts.get(author, 0) + 1

        if author_counts:
            return max(author_counts, key=lambda k: author_counts[k])

        # Fallback to local OWNERS files
        for snippet in self._snippets:
            assignee = self._find_local_owner(snippet.file, cores)
            if assignee:
                return assignee

        return None

    def _find_local_owner(
        self, file: str | os.PathLike[str], cores: set[str] | None
    ) -> str | None:
        """Searches for an assignee in local OWNERS files up to self.root."""
        abs_file = self._abs_path(file)
        directory = os.path.dirname(abs_file)
        repo_root_prefix = self.root + os.sep

        while directory.startswith(repo_root_prefix) and directory != self.root:
            owners_file = os.path.join(directory, 'OWNERS')
            if os.path.exists(owners_file):
                local_owners = parse_owners_file(owners_file)
                if cores is not None:
                    candidates = [o for o in local_owners if o in cores]
                else:
                    candidates = local_owners

                if candidates:
                    return candidates[0]

            directory = os.path.dirname(directory)
        return None

    def _find_nonlsc_revision(self, rel_target_file: str) -> str | None:
        """Finds the most recent git revision that isn't a large-scale change.

        This function considers any git revision that affects more than 100
        files to be a large-scale change (LSC).

        Args:
            rel_target_file: Path to the target file relative to the repo root.

        Returns:
            The git revision line (from git log) or None if not found.
        """
        revisions = self._revisions(rel_target_file)

        for revision in revisions:
            parts = revision.split()
            commit_hash = parts[0]

            files_changed = self._files_changed(commit_hash)

            # Check if number of files changed is 100 or less
            if len(files_changed) <= 100:
                return revision

        return None

    def _revisions(self, rel_target_file: str) -> list[str]:
        """Runs git log to get up to 20 revisions for a file."""
        return self.repo.log_revisions(file=rel_target_file, limit=20)

    def _files_changed(self, commit_hash: str) -> list[str]:
        """Runs git show to get files changed in a commit."""
        return self.repo.show_names(commit=commit_hash)


def find_owners(
    root_path: str | os.PathLike[str],
    file_path: str | os.PathLike[str],
    *args: str | re.Pattern,
) -> str | None:
    """Finds assignee of first line containing args or matching regexes.

    Args:
        root_path: Path to the repository root.
        file_path: Path to the target file relative to root.
        *args: Substrings or Patterns to search for in lines.

    Returns:
        Assignee email string or None if not found.
    """
    loc = find_location(root_path, file_path, *args)
    if loc.lines:
        repo = ReadOnlyGitWorkspace(project_dir=root_path)
        finder = CoreOwnerFinder(repo=repo)
        finder.add(os.path.join(root_path, loc.file), loc.lines)
        return finder.find()
    return None


def _extract_author(blame_line: str) -> str | None:
    """Extracts the author email from a git blame -e line."""
    start_paren = blame_line.find('(')
    end_paren = blame_line.find(')')
    if start_paren == -1 or end_paren == -1 or start_paren >= end_paren:
        return None
    header = blame_line[start_paren + 1 : end_paren]

    start_email = header.find('<')
    end_email = header.find('>')
    if start_email == -1 or end_email == -1 or start_email >= end_email:
        return None
    return header[start_email + 1 : end_email]


def _parse_snippet_arg(arg: str) -> tuple[str, tuple[int, int] | None]:
    """Parses a command line argument of the form file[:start-end]."""
    if ':' in arg:
        path, range_str = arg.rsplit(':', 1)
        match = re.match(r'^(\d+)-(\d+)$', range_str)
        if match:
            start = int(match.group(1))
            end = int(match.group(2))
            return path, (start, end)
        return arg, None
    return arg, None


def main(argv: list[str] | None = None) -> int:
    """Finds core owners for code snippets.

    Args:
        argv: Optional list of command-line arguments.

    Returns:
        Exit code (0 on success).
    """
    parser = argparse.ArgumentParser(
        description='Finds core owners for code snippets in the repository.',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Arguments should be of the form '<file>[:start-end]', where:
  <file>       is the path to the file (absolute or relative to repo).
  start-end    is an optional 1-indexed inclusive line range (e.g. 10-20).

Examples:
  find_core_owners.py path/to/file.h
  find_core_owners.py path/to/file.h:10-20
""",
    )
    parser.add_argument(
        'snippets',
        nargs='+',
        help="Code snippets to examine, in the form 'file[:start-end]'",
    )
    parser.add_argument(
        '-a',
        '--any',
        action='store_true',
        help='Allow any assignee, not just core team members',
    )
    default_root = os.environ.get(
        'BUILD_WORKSPACE_DIRECTORY',
        os.environ.get('BUILD_WORKING_DIRECTORY', '.'),
    )
    parser.add_argument(
        '-r',
        '--root',
        default=default_root,
        help='Root directory of the repository (defaults to current directory)',
    )

    args = parser.parse_args(argv)

    repo = ReadOnlyGitWorkspace(project_dir=args.root)
    for arg in args.snippets:
        file, lines = _parse_snippet_arg(arg)
        finder = CoreOwnerFinder(repo=repo)
        finder.add(file, lines)
        assignee = finder.find(any_owner=args.any)
        print(f'{arg}: {assignee}')

    return 0


if __name__ == '__main__':
    sys.exit(main())
