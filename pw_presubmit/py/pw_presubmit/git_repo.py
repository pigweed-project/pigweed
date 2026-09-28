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
"""Helpful commands for working with a Git repository."""

from pathlib import Path
import subprocess
from typing import Collection, Pattern

from pw_cli import git_repo
from pw_cli.decorators import deprecated
from pw_presubmit.tools import PresubmitToolRunner

# Moved to pw_cli.git_repo.
TRACKING_BRANCH_ALIAS = git_repo.TRACKING_BRANCH_ALIAS


class LoggingGitRepo(git_repo.GitRepo):
    """A version of GitRepo that defaults to using a PresubmitToolRunner."""

    def __init__(self, repo_root: Path):
        super().__init__(repo_root, PresubmitToolRunner())


@deprecated('Extend GitRepo to expose needed functionality.')
def git_stdout(
    *args: Path | str, show_stderr=False, repo: Path | str = '.'
) -> str:
    """Runs a git command in the specified repo."""
    tool_runner = PresubmitToolRunner()
    return (
        tool_runner(
            'git',
            ['-C', str(repo), *args],
            stderr=None if show_stderr else subprocess.DEVNULL,
            check=True,
            pw_presubmit_ignore_dry_run=True,
        )
        .stdout.decode()
        .strip()
    )


def find_git_repo(path_in_repo: Path) -> git_repo.GitRepo:
    """Tries to find the root of the git repo that owns path_in_repo."""
    return git_repo.find_git_repo(path_in_repo, PresubmitToolRunner())


@deprecated('Use GitRepo().tracking_branch().')
def tracking_branch(
    repo_path: Path | None = None,
    fallback: str | None = None,
) -> str | None:
    """Returns the tracking branch of the current branch.

    Since most callers of this function can safely handle a return value of
    None, suppress exceptions and return None if there is no tracking branch.

    Args:
      repo_path: repo path from which to run commands; defaults to Path.cwd()

    Raises:
      ValueError: if repo_path is not in a Git repository

    Returns:
      the remote tracking branch name or None if there is none
    """
    repo = repo_path if repo_path is not None else Path.cwd()
    return find_git_repo(repo).tracking_branch(fallback)


@deprecated('Use GitRepo().list_files().')
def list_files(
    commit: str | None = None,
    pathspecs: Collection[Path | str] = (),
    repo_path: Path | None = None,
) -> list[Path]:
    """Lists files with git ls-files or git diff --name-only.

    Args:
      commit: commit to use as a base for git diff
      pathspecs: Git pathspecs to use in git ls-files or diff
      repo_path: repo path from which to run commands; defaults to Path.cwd()

    Returns:
      A sorted list of absolute paths
    """
    repo = repo_path if repo_path is not None else Path.cwd()
    return find_git_repo(repo).list_files(commit, pathspecs)


@deprecated('Use GitRepo.has_uncommitted_changes().')
def has_uncommitted_changes(repo: Path | None = None) -> bool:
    """Returns True if the Git repo has uncommitted changes in it.

    This does not check for untracked files.
    """
    return LoggingGitRepo(
        repo if repo is not None else Path.cwd()
    ).has_uncommitted_changes()


def describe_files(
    git_root: Path,  # pylint: disable=unused-argument
    working_dir: Path,
    commit: str | None,
    pathspecs: Collection[Path | str],
    exclude: Collection[Pattern],
    project_root: Path | None = None,
) -> str:
    """Completes 'Doing something to ...' for a set of files in a Git repo."""
    return git_repo.describe_git_pattern(
        working_dir=working_dir,
        commit=commit,
        pathspecs=pathspecs,
        exclude=exclude,
        tool_runner=PresubmitToolRunner(),
        project_root=project_root,
    )


@deprecated('Use find_git_repo().root().')
def root(repo_path: Path | str = '.') -> Path:
    """Returns the repository root as an absolute path.

    Raises:
      FileNotFoundError: the path does not exist
      subprocess.CalledProcessError: the path is not in a Git repo
    """
    return find_git_repo(Path(repo_path)).root()


def within_repo(repo_path: Path | str = '.') -> Path | None:
    """Similar to root(repo_path), returns None if the path is not in a repo."""
    try:
        return find_git_repo(Path(repo_path)).root()
    except git_repo.GitError:
        return None


def is_repo(repo_path: Path | str = '.') -> bool:
    """True if the path is tracked by a Git repo."""
    return within_repo(repo_path) is not None


def path(
    repo_path: Path | str,
    *additional_repo_paths: Path | str,
    repo: Path | str = '.',
) -> Path:
    """Returns a path relative to a Git repository's root."""
    return (
        find_git_repo(Path(repo))
        .root()
        .joinpath(repo_path, *additional_repo_paths)
    )


@deprecated('Use GitRepo.commit_message().')
def commit_message(commit: str = 'HEAD', repo: Path | str = '.') -> str:
    """Returns the commit message of the revision."""
    return LoggingGitRepo(Path(repo)).commit_message(commit)


@deprecated('Use GitRepo.commit_author().')
def commit_author(commit: str = 'HEAD', repo: Path | str = '.') -> str:
    """Returns the commit author of the revision."""
    return LoggingGitRepo(Path(repo)).commit_author(commit)


@deprecated('Use GitRepo.commit_hash().')
def commit_hash(
    rev: str = 'HEAD', short: bool = True, repo: Path | str = '.'
) -> str:
    """Returns the commit hash of the revision."""
    return LoggingGitRepo(Path(repo)).commit_hash(rev, short)


@deprecated('Use GitRepo.list_submodules().')
def discover_submodules(
    superproject_dir: Path, excluded_paths: Collection[Pattern | str] = ()
) -> list[Path]:
    """Query git and return a list of submodules in the current project.

    Args:
        superproject_dir: Path object to directory under which we are looking
                          for submodules. This will also be included in list
                          returned unless excluded.
        excluded_paths: Pattern or string that match submodules that should not
                        be returned. All matches are done on posix style paths.

    Returns:
        List of "Path"s which were found but not excluded, this includes
        superproject_dir unless excluded.
    """
    return LoggingGitRepo(superproject_dir).list_submodules(excluded_paths) + [
        superproject_dir.resolve()
    ]
