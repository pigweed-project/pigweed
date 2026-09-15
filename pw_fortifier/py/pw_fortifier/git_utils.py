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
"""Defines Git workspace helpers for managing local Git repositories."""

import asyncio
from datetime import date, datetime
import logging
import os
import re
import subprocess
import tempfile
import textwrap
from pathlib import Path
from typing import Iterator, TypeVar

_Self = TypeVar('_Self', bound='ReadOnlyGitWorkspace')


def _git_env() -> dict[str, str]:
    """Returns a clean environment for git commands to avoid conflicts."""
    env = os.environ.copy()
    return env


def _run_git_sync(*args, **kwargs) -> subprocess.CompletedProcess:
    """Runs a git command synchronously."""
    kwargs.setdefault('check', True)
    kwargs.setdefault('capture_output', True)
    kwargs.setdefault('env', _git_env())
    return subprocess.run(['git'] + list(args), **kwargs)


def get_git_repo_url(
    path: Path | str | os.PathLike[str] | None = None,
    remote: str = 'origin',
) -> str:
    """Gets the remote repository URL for a git repository.

    Args:
        path: Optional path to the repository directory. Defaults to the
            invocation directory or current working directory.
        remote: Remote name to query (defaults to 'origin').

    Returns:
        The remote repository URL string.
    """
    if path is None:
        target_dir = os.environ.get(
            'BUILD_WORKING_DIRECTORY',
            os.environ.get('BUILD_WORKSPACE_DIRECTORY', os.getcwd()),
        )
    else:
        target_dir = str(path)

    result = _run_git_sync(
        '-C', target_dir, 'remote', 'get-url', remote, text=True
    )
    return result.stdout.strip()


def get_git_repo_root(
    path: Path | str | os.PathLike[str] | None = None,
) -> Path | None:
    """Gets the root directory of the git repository.

    Args:
        path: Optional path to a directory within the repository.
            Defaults to BUILD_WORKSPACE_DIRECTORY, BUILD_WORKING_DIRECTORY,
            or the current working directory (outside of test environments).

    Returns:
        The resolved Path to the repository root, or None if not in a
        repository.
    """
    if path is not None:
        target_dir = str(path)
    elif 'BUILD_WORKSPACE_DIRECTORY' in os.environ:
        target_dir = os.environ['BUILD_WORKSPACE_DIRECTORY']
    elif 'BUILD_WORKING_DIRECTORY' in os.environ:
        target_dir = os.environ['BUILD_WORKING_DIRECTORY']
    elif 'TEST_TARGET' in os.environ or 'TEST_SRCDIR' in os.environ:
        # Running inside a bazel test sandbox; not a real repository.
        return None
    else:
        target_dir = os.getcwd()

    result = _run_git_sync(
        '-C',
        target_dir,
        'rev-parse',
        '--show-toplevel',
        check=False,
        text=True,
    )
    if result.returncode == 0 and result.stdout.strip():
        return Path(result.stdout.strip()).resolve()
    return None


def make_git_commit_msg(
    title: str,
    description: str,
    issue_id: int | None = None,
) -> Iterator[str]:
    """Formats a git commit message from title, description, and issue ID.

    Args:
        title: The commit subject line.
        description: The commit body description.
        issue_id: Optional issue/bug tracker ID.

    Yields:
        Lines of the formatted git commit message.
    """
    yield title
    if description:
        yield ''
        yield from textwrap.wrap(description, width=72)
    if issue_id:
        yield ''
        yield f'Bug: {issue_id}'


class BasicGitWorkspace:
    """Base class for Git workspaces."""

    def __init__(
        self,
        project_dir: Path | str | os.PathLike[str],
        repo_url: str | None = None,
        tmp_dir: tempfile.TemporaryDirectory | None = None,
    ) -> None:
        """Initializes the workspace for an already cloned repository.

        Args:
          project_dir: The local directory of the git repository.
          repo_url: Optional remote repository URL.
          tmp_dir: Optional TemporaryDirectory instance that owns project_dir
            and should be cleaned up on deletion.
        """
        self._project_dir = Path(project_dir).resolve()
        self._repo_url = repo_url
        self._tmp_dir = tmp_dir

    def __del__(self) -> None:
        # Only clean up if we own a temporary directory
        if self._tmp_dir is not None:
            self._tmp_dir.cleanup()

    @property
    def project_dir(self) -> Path:
        """Returns the project directory path.

        Returns:
            The project directory Path instance.
        """
        return self._project_dir

    @property
    def repo_url(self) -> str | None:
        """Returns the remote repository URL, if known.

        Returns:
            The remote repository URL string or None.
        """
        return self._repo_url

    async def run_git(self, *args, **kwargs) -> subprocess.CompletedProcess:
        """Runs a git command in the context of the workspace directory.

        Args:
            *args: Arguments to pass to git.
            **kwargs: Keyword arguments to pass to subprocess.run.

        Returns:
            The CompletedProcess result from running the git command.
        """
        cmd = ['-C', str(self._project_dir)] + list(args)
        loop = asyncio.get_running_loop()
        return await loop.run_in_executor(
            None, lambda: _run_git_sync(*cmd, **kwargs)
        )

    def run_git_sync(self, *args, **kwargs) -> subprocess.CompletedProcess:
        """Runs a git command synchronously in the workspace directory.

        Args:
            *args: Arguments to pass to git.
            **kwargs: Keyword arguments to pass to subprocess.run.

        Returns:
            The CompletedProcess result from running the git command.
        """
        cmd = ['-C', str(self._project_dir)] + list(args)
        return _run_git_sync(*cmd, **kwargs)


class ReadOnlyGitWorkspace(BasicGitWorkspace):
    """Local Git repo interface for examining files and revision history."""

    @classmethod
    async def clone(
        cls: type[_Self],
        src_url: str,
        dst_dir: Path | str | os.PathLike[str] | None = None,
        timestamp: datetime | date | str | None = None,
        no_checkout: bool = False,
        git_filter: str | None = None,
        depth: int | None = 1,
    ) -> _Self:
        """Asynchronously clones a repository.

        Args:
            src_url: URL of the repository to clone.
            dst_dir: Optional destination directory for the clone.
            timestamp: Optional timestamp for shallow-since clones.
            no_checkout: If True, skip checking out working tree.
            git_filter: Optional git filter argument (e.g. blob:none).
            depth: Optional commit depth for shallow clone.

        Returns:
            A new workspace instance for the cloned repo.
        """
        loop = asyncio.get_running_loop()
        return await loop.run_in_executor(
            None,
            lambda: cls.clone_sync(
                src_url=src_url,
                dst_dir=dst_dir,
                timestamp=timestamp,
                no_checkout=no_checkout,
                git_filter=git_filter,
                depth=depth,
            ),
        )

    @classmethod
    def clone_sync(
        cls: type[_Self],
        src_url: str,
        dst_dir: Path | str | os.PathLike[str] | None = None,
        timestamp: datetime | date | str | None = None,
        no_checkout: bool = False,
        git_filter: str | None = None,
        depth: int | None = 1,
    ) -> _Self:
        """Synchronously clones a repository.

        Args:
            src_url: URL of the repository to clone.
            dst_dir: Optional destination directory for the clone.
            timestamp: Optional timestamp for shallow-since clones.
            no_checkout: If True, skip checking out working tree.
            git_filter: Optional git filter argument (e.g. blob:none).
            depth: Optional commit depth for shallow clone.

        Returns:
            A new ReadOnlyGitWorkspace instance for the cloned repo.
        """
        tmp_dir = None
        if dst_dir is None:
            tmp_dir = tempfile.TemporaryDirectory()
            target_path = Path(tmp_dir.name) / 'src'
        else:
            target_path = Path(dst_dir)
            target_path.parent.mkdir(parents=True, exist_ok=True)

        if target_path.is_dir() and (target_path / '.git').exists():
            ws = cls(project_dir=target_path, repo_url=src_url, tmp_dir=tmp_dir)

            # 1. Verify remote URL
            remote_url = get_git_repo_url(ws.project_dir)
            if remote_url.rstrip('/') != src_url.rstrip('/'):
                raise RuntimeError(
                    f'Existing repository at {target_path} has remote URL '
                    f'{remote_url}, expected {src_url}'
                )

            # 2. Verify no uncommitted changes
            status_res = ws.run_git_sync('status', '--porcelain', text=True)
            if status_res.stdout.strip():
                raise RuntimeError(
                    f'Existing repository at {target_path} '
                    'has uncommitted changes'
                )

            # 3. Switch to main branch if not already on it
            branch_res = ws.run_git_sync('branch', '--show-current', text=True)
            current_branch = branch_res.stdout.strip()
            if current_branch != 'main':
                ws.run_git_sync('checkout', 'main')

            # 4. Pull latest changes
            ws.run_git_sync('pull')

            return ws

        # Build git clone arguments
        clone_args = ['clone']
        if no_checkout:
            clone_args.append('--no-checkout')
        if git_filter is not None:
            clone_args.append(f'--filter={git_filter}')
        if timestamp is not None:
            ts_str = (
                timestamp.isoformat()
                if isinstance(timestamp, (date, datetime))
                else str(timestamp)
            )
            clone_args.append(f'--shallow-since={ts_str}')
        elif depth is not None:
            clone_args.extend(['--depth', str(depth)])
        clone_args.extend([src_url, str(target_path)])
        cwd = tmp_dir.name if tmp_dir else str(target_path.parent)

        _run_git_sync(*clone_args, cwd=cwd)

        if not target_path.is_dir():
            if tmp_dir:
                tmp_dir.cleanup()
            raise RuntimeError(f'Failed to clone {src_url} to {target_path}')

        return cls(project_dir=target_path, repo_url=src_url, tmp_dir=tmp_dir)

    def get_versions(
        self,
        num: int | str | None = None,
        pattern: str | None = None,
        scope: str | os.PathLike[str] | None = None,
    ) -> Iterator[tuple[str, date]]:
        """Synchronously queries repository history for versions.

        Args:
            num: Optional maximum number of log entries.
            pattern: Optional grep pattern to filter commit messages.
            scope: Optional path scope to filter log entries.

        Yields:
            Tuples of (commit_hash, commit_date).
        """
        log_args = ['log', '--format=%H,%cd', '--date=iso']
        if num is not None:
            log_args += ['-n', str(num)]
        if pattern is not None:
            log_args += [f'--grep={pattern}']
        if scope is not None:
            log_args += ['--', str(scope)]

        result = self.run_git_sync(*log_args, text=True)
        for line in result.stdout.splitlines():
            parts = line.split(',', 1)
            if len(parts) == 2:
                v_hash, v_date_str = parts
                v_date = date.fromisoformat(v_date_str[:10])
                yield v_hash, v_date

    def blame(
        self,
        file: str | os.PathLike[str],
        commit: str,
        lines: tuple[int, int] | None = None,
    ) -> list[str]:
        """Runs git blame on a file at a specific commit.

        Args:
            file: Path to the file to blame.
            commit: The commit hash to blame at.
            lines: Optional (start, end) line range to blame.

        Returns:
            A list of blame output lines.
        """
        args = ['blame', '-e']
        if lines:
            args += ['-L', f'{lines[0]},{lines[1]}']
        args += [commit, '--', str(file)]
        result = self.run_git_sync(*args, text=True)
        return result.stdout.splitlines()

    def log_revisions(
        self,
        file: str | os.PathLike[str],
        limit: int = 20,
    ) -> list[str]:
        """Runs git log to get revisions for a file.

        Args:
            file: Path to the file.
            limit: Maximum number of revisions to return.

        Returns:
            A list of log output lines (one per revision).
        """
        args = [
            'log',
            '--follow',
            '--oneline',
            '-n',
            str(limit),
            '--',
            str(file),
        ]
        result = self.run_git_sync(*args, text=True)
        return result.stdout.splitlines()

    def show_names(self, commit: str) -> list[str]:
        """Runs git show to get files changed in a commit.

        Args:
            commit: The commit hash.

        Returns:
            A list of file paths changed in the commit.
        """
        args = ['show', '--name-only', '--pretty=format:', commit]
        result = self.run_git_sync(*args, text=True)
        return result.stdout.splitlines()


class GitBranch:
    """Context-managed Git branch for staging, committing, and pushing."""

    def __init__(
        self,
        workspace: BasicGitWorkspace,
        name: str = 'tmp',
        keep: bool = False,
    ) -> None:
        self._workspace = workspace
        self._name = name
        self._pristine = True
        self._keep = keep
        self._prev: str = 'main'

    @property
    def name(self) -> str:
        """Returns the branch name.

        Returns:
            The branch name string.
        """
        return self._name

    @property
    def pristine(self) -> bool:
        """Returns True if no commits have been made on this branch yet.

        Returns:
            Boolean indicating whether the branch is pristine.
        """
        return self._pristine

    @property
    def keep(self) -> bool:
        """Returns True if the branch should be preserved on context exit.

        Returns:
            Boolean indicating whether to keep the branch on exit.
        """
        return self._keep

    @keep.setter
    def keep(self, value: bool) -> None:
        self._keep = value

    async def setup(self) -> 'GitBranch':
        """Captures active branch and checks out temporary branch."""
        result = await self._workspace.run_git(
            'rev-parse', '--abbrev-ref', 'HEAD', text=True
        )
        self._prev = result.stdout.strip() or 'main'
        await self._workspace.run_git('checkout', '-b', self._name)
        return self

    async def teardown(self) -> None:
        """Resets, switches back, and deletes branch if keep is False."""
        if self._keep:
            await self._workspace.run_git('checkout', self._prev)
            return
        await self.reset(staged=True)
        await self._workspace.run_git('checkout', self._prev)
        await self._workspace.run_git('branch', '-D', self._name)

    async def __aenter__(self) -> 'GitBranch':
        """Captures active branch and checks out temporary branch."""
        return await self.setup()

    async def __aexit__(
        self,
        exc_type: type[BaseException] | None,
        exc_val: BaseException | None,
        exc_tb: object | None,
    ) -> None:
        """Resets workspace, switches back, and deletes the branch."""
        await self.teardown()

    async def reset(self, staged: bool = False) -> None:
        """Resets the branch working tree and optionally staged changes.

        Args:
            staged: If True, removes staged changes as well, returning the
                branch to a pristine state. If False, removes working tree and
                untracked files, leaving only staged changes.
        """
        if staged:
            await self._workspace.run_git('reset', '--hard')
        else:
            await self._workspace.run_git('checkout', '--', '.')
        await self._workspace.run_git('clean', '-fd')

    async def diff(self, staged: bool = False) -> list[str]:
        """Returns the diff lines of the working tree or staging area.

        Args:
            staged: If True, returns diff of staged changes.
                    If False, returns diff of unstaged working tree changes.

        Returns:
            A list of diff output lines.
        """
        args = ['diff', '--staged'] if staged else ['diff']
        result = await self._workspace.run_git(*args, text=True)
        return result.stdout.splitlines()

    async def commit_msg(self) -> list[str]:
        """Returns the commit message lines of the current HEAD commit.

        Returns:
            A list of newline-delimited output lines from git log.
        """
        result = await self._workspace.run_git(
            'log', '-1', '--format=%B', 'HEAD', text=True
        )
        return result.stdout.splitlines()

    async def add(self, path: str | Path | os.PathLike[str] = '.') -> None:
        """Stages file or directory changes in the branch.

        Args:
          path: File or directory path to stage (defaults to '.').
        """
        await self._workspace.run_git('add', str(path))

    async def commit(
        self,
        commit_msg: str,
        amend: bool = False,
    ) -> None:
        """Commits pending changes to the local workspace branch.

        Args:
          commit_msg: The commit message.
          amend: If True and branch is not pristine, amends previous commit.
        """
        commit_cmd = ['commit']
        if not self._pristine and amend:
            commit_cmd.append('--amend')
        commit_cmd.extend(['-m', commit_msg])

        await self._workspace.run_git(*commit_cmd)
        self._pristine = False

    async def push(self) -> int | None:
        """Pushes committed changes to Gerrit, returning the CL number.

        Returns:
          The Gerrit CL number if successfully parsed; None otherwise.
        """
        push_result = await self._workspace.run_git(
            'push',
            'origin',
            'HEAD:refs/for/main',
            '--recurse-submodules=check',
            text=True,
        )

        stderr_output = push_result.stderr
        logging.debug('Git push stderr: %s', stderr_output)

        # Parse CL number from Gerrit response
        cl_match = re.search(r'googlesource\.com/c/.+/\+/(\d+)', stderr_output)
        if cl_match:
            return int(cl_match.group(1))

        return None


class WritableGitWorkspace(ReadOnlyGitWorkspace):
    """Local Git workspace with operations for modifying and pushing changes."""

    def branch(self, name: str = 'tmp', keep: bool = False) -> GitBranch:
        """Returns a context-managed GitBranch object for creating a branch.

        Args:
          name: Branch name to checkout (defaults to 'tmp').
          keep: If True, skips cleanup on context exit.

        Returns:
          A GitBranch instance.
        """
        return GitBranch(self, name, keep=keep)
