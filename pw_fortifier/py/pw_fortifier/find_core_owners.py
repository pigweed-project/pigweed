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

import subprocess
import os
import re
import sys
import argparse
from typing import NamedTuple


def parse_owners_file(path: str | os.PathLike[str]) -> list[str]:
    """Parses an OWNERS file and returns the list of emails found."""
    members = []
    with open(path, "r") as f:
        for line in f:
            line = line.strip()
            if (
                not line
                or line.startswith("#")
                or line.startswith("include")
                or line.startswith("per-file")
            ):
                continue

            # Parse member: email #{annotation}
            parts = line.split('#', 1)
            email_part = parts[0].strip()
            if email_part and '@' in email_part:
                members.append(email_part)
    return members


def run_git(args: list[str], cwd: str | os.PathLike[str]) -> list[str]:
    """Runs a git command in the specified directory.

    Args:
        args: Arguments to the git command, starting with the subcommand.
        cwd: Current working directory for the command.

    Returns:
        A list of stripped output lines, or an empty list on error.
    """
    try:
        cmd = ["git"] + args
        result = subprocess.run(
            cmd, cwd=cwd, capture_output=True, text=True, check=True
        )
        return [line.strip() for line in result.stdout.splitlines()]
    except (subprocess.CalledProcessError, FileNotFoundError):
        return []


class CoreOwnerFinder:
    """Finds the core owner for a set of code snippets."""

    class CodeSnippet(NamedTuple):
        """Represents a file or range of lines in a file containing code."""

        file: str | os.PathLike[str]
        lines: tuple[int, int] | None

    def __init__(self, root: str | os.PathLike[str] = "."):
        self.root = os.path.abspath(root)
        self.owners_path = os.path.join(self.root, 'OWNERS')
        self._snippets: list[CoreOwnerFinder.CodeSnippet] = []

    def core_members(self) -> set[str]:
        """Gets core team members from the OWNERS file in the repo root."""
        return set(parse_owners_file(self.owners_path))

    def add(
        self, file: str | os.PathLike[str], lines: tuple[int, int] | None = None
    ) -> None:
        """Adds a file or file range to be examined."""
        self._snippets.append(self.CodeSnippet(file=file, lines=lines))

    def find(self, any_owner: bool = False) -> str | None:
        """Finds the core team member who most modified the added snippets."""
        cores = self.core_members() if not any_owner else None
        author_counts: dict[str, int] = {}

        for snippet in self._snippets:
            try:
                abs_file = os.path.abspath(snippet.file)
                rel_file = os.path.relpath(abs_file, self.root)
            except ValueError:
                continue

            revision_line = self._find_nonlsc_revision(rel_file)
            if not revision_line:
                continue
            commit_hash = revision_line.split()[0]

            args = ["blame", "-e"]
            if snippet.lines:
                args += ["-L", f"{snippet.lines[0]},{snippet.lines[1]}"]
            args += [commit_hash, "--", rel_file]

            blame_lines = run_git(args, cwd=self.root)

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
            owner = self._find_local_owner(snippet.file, cores)
            if owner:
                return owner

        return None

    def _find_local_owner(
        self, file: str | os.PathLike[str], cores: set[str] | None
    ) -> str | None:
        """Searches for an owner in local OWNERS files up to self.root."""
        abs_file = os.path.abspath(file)
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
        return run_git(
            ["log", "--follow", "--oneline", "-n", "20", "--", rel_target_file],
            cwd=self.root,
        )

    def _files_changed(self, commit_hash: str) -> list[str]:
        """Runs git show to get files changed in a commit."""
        return run_git(
            ["show", "--name-only", "--pretty=format:", commit_hash],
            cwd=self.root,
        )


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
    if ":" in arg:
        path, range_str = arg.rsplit(":", 1)
        match = re.match(r"^(\d+)-(\d+)$", range_str)
        if match:
            start = int(match.group(1))
            end = int(match.group(2))
            return path, (start, end)
        return arg, None
    return arg, None


def main(argv: list[str] | None = None) -> int:
    """Finds core owners for code snippets."""
    parser = argparse.ArgumentParser(
        description="Finds core owners for code snippets in the repository.",
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
        "snippets",
        nargs="+",
        help="Code snippets to examine, in the form 'file[:start-end]'",
    )
    parser.add_argument(
        "-a",
        "--any",
        action="store_true",
        help="Allow any owner, not just core team members",
    )
    parser.add_argument(
        "-r",
        "--root",
        default=".",
        help="Root directory of the repository (defaults to current directory)",
    )

    args = parser.parse_args(argv)

    for arg in args.snippets:
        file, lines = _parse_snippet_arg(arg)
        finder = CoreOwnerFinder(root=args.root)
        finder.add(file, lines)
        owner = finder.find(any_owner=args.any)
        print(f"{arg}: {owner}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
