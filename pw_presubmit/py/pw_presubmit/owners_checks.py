# Copyright 2020 The Pigweed Authors
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
#
"""OWNERS file checks."""

import argparse
import collections
import logging
import pathlib
import sys
from typing import (
    Collection,
    Iterable,
    Set,
)

from pw_presubmit import git_repo, PresubmitFailure
from pw_presubmit.format.owners import OwnersError, OwnersFile

_LOG = logging.getLogger(__name__)


class OwnersWalker(OwnersFile):
    """Walks the dependencies of a OWNERS file."""

    def __init__(self, path: pathlib.Path, file_contents: bytes) -> None:
        super().__init__(path, file_contents)

    @staticmethod
    def load_from_path(owners_file: pathlib.Path) -> 'OwnersWalker':
        return OwnersWalker(
            owners_file,
            OwnersFile._read_owners_file(owners_file),
        )

    def _complete_path(self, sub_owners_file_path) -> pathlib.Path:
        """Always return absolute path."""
        # Absolute paths start with the git/project root
        if sub_owners_file_path.startswith("/"):
            root = git_repo.find_git_repo(self.path).root()
            full_path = root / sub_owners_file_path[1:]
        else:
            # Relative paths start with owners file dir
            full_path = self.path.parent / sub_owners_file_path
        return full_path.resolve()

    def get_dependencies(self) -> list[pathlib.Path]:
        """Finds owners files this file includes."""
        dependencies = []
        # All the includes
        for include in self.sections.get(OwnersFile._LineType.INCLUDE, []):
            file_str = include.content[len("include ") :]
            dependencies.append(self._complete_path(file_str))

        # all file: rules:
        for file_rule in self.sections.get(OwnersFile._LineType.FILE_RULE, []):
            file_str = file_rule.content[len("file:") :]
            path = self._complete_path(file_str)
            if ":" in file_str and not path.is_file():
                _LOG.warning(
                    "TODO: b/254322931 - This check does not yet support "
                    "<project> or <branch> in a file: rule"
                )
                _LOG.warning(
                    "It will not check line '%s' found in %s",
                    file_rule.content,
                    self.path,
                )

            else:
                dependencies.append(path)

        # all the per-file rule includes
        for per_file in self.sections.get(OwnersFile._LineType.PER_FILE, []):
            file_str = per_file.content[len("per-file ") :]
            access_grant = file_str[file_str.index("=") + 1 :]
            if access_grant.startswith("file:"):
                dependencies.append(
                    self._complete_path(access_grant[len("file:") :])
                )

        return dependencies


def resolve_owners_tree(root_owners: pathlib.Path) -> list[OwnersWalker]:
    """Given a starting OWNERS file return it and all of it's dependencies."""
    found = []
    todo = collections.deque((root_owners,))
    checked: Set[pathlib.Path] = set()
    while todo:
        cur_file = todo.popleft()
        checked.add(cur_file)
        owners_obj = OwnersWalker.load_from_path(cur_file)
        found.append(owners_obj)
        new_dependents = owners_obj.get_dependencies()
        for new_dep in new_dependents:
            if new_dep not in checked and new_dep not in todo:
                todo.append(new_dep)
    return found


def run_owners_checks(
    list_or_path: Iterable[pathlib.Path] | pathlib.Path,
) -> dict[pathlib.Path, str]:
    """Decorator that accepts Paths or list of Paths and iterates as needed."""
    errors: dict[pathlib.Path, str] = {}
    if isinstance(list_or_path, Iterable):
        files = list_or_path
    else:
        files = (list_or_path,)

    all_owners_obj: list[OwnersWalker] = []
    for file in files:
        all_owners_obj.extend(resolve_owners_tree(file))

    checked: Set[pathlib.Path] = set()
    for current_owners in all_owners_obj:
        # Ensure we don't check the same file twice
        if current_owners.path in checked:
            continue
        checked.add(current_owners.path)
        try:
            current_owners.look_for_owners_errors()
        except OwnersError as err:
            errors[current_owners.path] = str(err)
            _LOG.error("%s: %s", current_owners.path.absolute(), err)
    return errors


def presubmit_check(files: pathlib.Path | Collection[pathlib.Path]) -> None:
    errors = run_owners_checks(files)
    if errors:
        for file in errors:
            _LOG.warning("  pw format --fix %s", file)
        _LOG.warning("will automatically fix this.")
        raise PresubmitFailure


def main() -> int:
    """Standalone test of styling."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--style", action="store_true")
    parser.add_argument("--owners_file", required=True, type=str)
    args = parser.parse_args()

    try:
        owners_obj = OwnersWalker.load_from_path(pathlib.Path(args.owners_file))
        owners_obj.look_for_owners_errors()
    except OwnersError as err:
        _LOG.error("%s", err)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
