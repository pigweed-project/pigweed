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
"""Tests for the GitWorkspace class in pw_fortifier."""

# pylint: disable=protected-access

from datetime import date
import os
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch

from pyfakefs.fake_filesystem_unittest import TestCaseMixin
from pw_fortifier.git_utils import (
    GitBranch,
    ReadOnlyGitWorkspace,
    WritableGitWorkspace,
    _git_env,
    get_git_repo_root,
    get_git_repo_url,
    make_git_commit_msg,
)


class TestGitWorkspace(unittest.IsolatedAsyncioTestCase, TestCaseMixin):
    """Unit tests for GitWorkspace verifying git and Gerrit integration."""

    def setUp(self) -> None:
        self.setUpPyfakefs()
        self.project_dir = Path('/tmp/fake_project').resolve()
        self.fs.create_dir(self.project_dir)
        self.repo_url = 'https://example.com/repo.git'
        self.workspace = WritableGitWorkspace(self.project_dir, self.repo_url)

    @patch('subprocess.run')
    async def test_clone_async(self, mock_run) -> None:
        """Tests async clone initiates shallow git clone in correct path."""

        def fake_clone(cmd, **_kwargs):
            if len(cmd) > 1 and cmd[1] == 'clone':
                Path(cmd[-1]).mkdir(parents=True, exist_ok=True)
            return subprocess.CompletedProcess(
                args=cmd, returncode=0, stdout='', stderr=''
            )

        mock_run.side_effect = fake_clone

        project_dir = Path('/tmp/test_clone/project').resolve()
        workspace = await ReadOnlyGitWorkspace.clone(self.repo_url, project_dir)

        mock_run.assert_called_once()
        call_args = mock_run.call_args[0][0]
        self.assertEqual(call_args[:4], ['git', 'clone', '--depth', '1'])
        self.assertEqual(call_args[4], self.repo_url)
        self.assertEqual(call_args[5], str(project_dir))
        self.assertEqual(workspace.project_dir, project_dir.resolve())

    @patch('subprocess.run')
    def test_clone_sync(self, mock_run) -> None:
        """Tests sync clone initiates shallow git clone in correct path."""

        def fake_clone(cmd, **_kwargs):
            if len(cmd) > 1 and cmd[1] == 'clone':
                Path(cmd[-1]).mkdir(parents=True, exist_ok=True)
            return subprocess.CompletedProcess(
                args=cmd, returncode=0, stdout='', stderr=''
            )

        mock_run.side_effect = fake_clone

        project_dir = Path('/tmp/test_clone/project').resolve()
        workspace = ReadOnlyGitWorkspace.clone_sync(self.repo_url, project_dir)

        mock_run.assert_called_once()
        call_args = mock_run.call_args[0][0]
        self.assertEqual(call_args[:4], ['git', 'clone', '--depth', '1'])
        self.assertEqual(call_args[4], self.repo_url)
        self.assertEqual(call_args[5], str(project_dir))
        self.assertEqual(workspace.project_dir, project_dir.resolve())

    @patch('subprocess.run')
    async def test_clone_temp_dir_async(self, mock_run) -> None:
        """Tests that async clone to a temp dir works and cleans up."""

        def fake_clone(cmd, **_kwargs):
            if len(cmd) > 1 and cmd[1] == 'clone':
                Path(cmd[-1]).mkdir(parents=True, exist_ok=True)
            return subprocess.CompletedProcess(
                args=cmd, returncode=0, stdout='', stderr=''
            )

        mock_run.side_effect = fake_clone

        workspace = await ReadOnlyGitWorkspace.clone(self.repo_url)
        self.assertIsNotNone(workspace._tmp_dir)
        self.assertEqual(workspace.project_dir.name, 'src')

        assert workspace._tmp_dir is not None
        tmp_dir_path = Path(workspace._tmp_dir.name)
        self.assertTrue(tmp_dir_path.exists())

        # Trigger cleanup
        workspace.__del__()  # pylint: disable=unnecessary-dunder-call
        self.assertFalse(tmp_dir_path.exists())

    @patch('subprocess.run')
    def test_clone_temp_dir_sync(self, mock_run) -> None:
        """Tests that sync clone to a temp dir works and cleans up."""

        def fake_clone(cmd, **_kwargs):
            if len(cmd) > 1 and cmd[1] == 'clone':
                Path(cmd[-1]).mkdir(parents=True, exist_ok=True)
            return subprocess.CompletedProcess(
                args=cmd, returncode=0, stdout='', stderr=''
            )

        mock_run.side_effect = fake_clone

        workspace = ReadOnlyGitWorkspace.clone_sync(self.repo_url)
        self.assertIsNotNone(workspace._tmp_dir)
        self.assertEqual(workspace.project_dir.name, 'src')

        assert workspace._tmp_dir is not None
        tmp_dir_path = Path(workspace._tmp_dir.name)
        self.assertTrue(tmp_dir_path.exists())

        # Trigger cleanup
        workspace.__del__()  # pylint: disable=unnecessary-dunder-call
        self.assertFalse(tmp_dir_path.exists())

    @patch('subprocess.run')
    def test_get_versions(self, mock_run) -> None:
        """Tests retrieving versions from git log."""
        log_output = (
            'commit1,2026-06-01 12:00:00 -0700\n'
            'commit2,2026-06-02 12:00:00 -0700\n'
        )
        mock_run.return_value = subprocess.CompletedProcess(
            args=[], returncode=0, stdout=log_output, stderr=''
        )

        versions = list(self.workspace.get_versions(num=2))
        self.assertEqual(len(versions), 2)
        self.assertEqual(versions[0][0], 'commit1')
        self.assertEqual(versions[0][1], date(2026, 6, 1))
        self.assertEqual(versions[1][0], 'commit2')
        self.assertEqual(versions[1][1], date(2026, 6, 2))

        mock_run.assert_called_once_with(
            [
                'git',
                '-C',
                str(self.project_dir),
                'log',
                '--format=%H,%cd',
                '--date=iso',
                '-n',
                '2',
            ],
            check=True,
            capture_output=True,
            env=_git_env(),
            text=True,
        )

    @patch('subprocess.run')
    async def test_branch_context_manager(self, mock_run) -> None:
        """Tests branch() creates and cleans up branch using context manager."""
        mock_run.return_value = subprocess.CompletedProcess(
            args=[], returncode=0, stdout='main\n', stderr=''
        )
        async with self.workspace.branch('my_branch') as branch:
            self.assertIsInstance(branch, GitBranch)
            self.assertEqual(branch.name, 'my_branch')
            self.assertEqual(
                mock_run.call_args_list,
                [
                    unittest.mock.call(
                        [
                            'git',
                            '-C',
                            str(self.project_dir),
                            'rev-parse',
                            '--abbrev-ref',
                            'HEAD',
                        ],
                        check=True,
                        capture_output=True,
                        env=_git_env(),
                        text=True,
                    ),
                    unittest.mock.call(
                        [
                            'git',
                            '-C',
                            str(self.project_dir),
                            'checkout',
                            '-b',
                            'my_branch',
                        ],
                        check=True,
                        capture_output=True,
                        env=_git_env(),
                    ),
                ],
            )

        self.assertEqual(mock_run.call_count, 6)
        exit_calls = mock_run.call_args_list[2:]
        self.assertEqual(
            exit_calls[0][0][0],
            ['git', '-C', str(self.project_dir), 'reset', '--hard'],
        )
        self.assertEqual(
            exit_calls[1][0][0],
            ['git', '-C', str(self.project_dir), 'clean', '-fd'],
        )
        self.assertEqual(
            exit_calls[2][0][0],
            ['git', '-C', str(self.project_dir), 'checkout', 'main'],
        )
        self.assertEqual(
            exit_calls[3][0][0],
            ['git', '-C', str(self.project_dir), 'branch', '-D', 'my_branch'],
        )

    @patch('subprocess.run')
    async def test_branch_context_manager_custom_branch(self, mock_run) -> None:
        """Tests branch() switches back to custom initial branch on exit."""
        mock_run.return_value = subprocess.CompletedProcess(
            args=[], returncode=0, stdout='feature/test\n', stderr=''
        )
        async with self.workspace.branch('my_branch') as branch:
            self.assertEqual(branch.name, 'my_branch')

        self.assertEqual(mock_run.call_count, 6)
        exit_calls = mock_run.call_args_list[2:]
        self.assertEqual(
            exit_calls[2][0][0],
            ['git', '-C', str(self.project_dir), 'checkout', 'feature/test'],
        )
        self.assertEqual(
            exit_calls[3][0][0],
            ['git', '-C', str(self.project_dir), 'branch', '-D', 'my_branch'],
        )

    @patch('subprocess.run')
    async def test_add(self, mock_run) -> None:
        """Tests staging changes with git add."""
        mock_run.return_value = subprocess.CompletedProcess(
            args=[], returncode=0, stdout='', stderr=''
        )
        async with self.workspace.branch() as branch:
            mock_run.reset_mock()
            await branch.add()
            mock_run.assert_called_once_with(
                ['git', '-C', str(self.project_dir), 'add', '.'],
                check=True,
                capture_output=True,
                env=_git_env(),
            )

            mock_run.reset_mock()
            await branch.add('foo/bar.txt')
            mock_run.assert_called_once_with(
                ['git', '-C', str(self.project_dir), 'add', 'foo/bar.txt'],
                check=True,
                capture_output=True,
                env=_git_env(),
            )

    def test_make_git_commit_msg(self) -> None:
        """Tests commit message formatting."""
        msg_lines = list(make_git_commit_msg('Title', 'Description text', 999))
        msg_str = '\n'.join(msg_lines)
        self.assertEqual(
            msg_str,
            'Title\n\nDescription text\n\nBug: 999',
        )

        msg_title_only = '\n'.join(list(make_git_commit_msg('Title only', '')))
        self.assertEqual(msg_title_only, 'Title only')

    @patch('subprocess.run')
    async def test_commit(self, mock_run) -> None:
        """Tests committing changes with commit message string."""
        mock_run.return_value = subprocess.CompletedProcess(
            args=[], returncode=0, stdout='', stderr=''
        )

        msg = '\n'.join(list(make_git_commit_msg('Title', 'Description', 999)))

        async with self.workspace.branch() as branch:
            mock_run.reset_mock()
            self.assertTrue(branch.pristine)
            await branch.commit(msg)
            self.assertFalse(branch.pristine)

            self.assertEqual(mock_run.call_count, 1)  # commit

            commit_args = mock_run.call_args_list[0][0][0]
            self.assertEqual(commit_args[:2], ['git', '-C'])
            self.assertEqual(commit_args[3], 'commit')
            self.assertNotIn('--amend', commit_args)
            m_index = commit_args.index('-m')
            commit_msg = commit_args[m_index + 1]
            self.assertEqual(commit_msg, msg)

            # Subsequent commit with default amend=False should NOT include
            # --amend
            mock_run.reset_mock()
            await branch.commit('Title 2')
            commit_args_2 = mock_run.call_args_list[0][0][0]
            self.assertNotIn('--amend', commit_args_2)

            # Subsequent commit with explicit amend=True should include --amend
            mock_run.reset_mock()
            await branch.commit('Title 3', amend=True)
            commit_args_3 = mock_run.call_args_list[0][0][0]
            self.assertIn('--amend', commit_args_3)

    @patch('subprocess.run')
    async def test_push_success(self, mock_run) -> None:
        """Tests successful push to Gerrit and parsing of the CL number."""
        gerrit_output = """
        remote: Resolving deltas: 100% (1/1)
        remote: Processing changes: new: 1, refs: 1
        remote: 
        remote:   https://pigweed-review.googlesource.com/c/pigweed/+/123456 New Change
        remote: 
        To sso://pigweed/pigweed
        """
        mock_run.return_value = subprocess.CompletedProcess(
            args=[], returncode=0, stdout='', stderr=gerrit_output
        )

        async with self.workspace.branch() as branch:
            mock_run.reset_mock()
            cl_num = await branch.push()

            self.assertEqual(cl_num, 123456)
            self.assertEqual(mock_run.call_count, 1)  # push

    @patch('subprocess.run')
    async def test_push_no_cl(self, mock_run) -> None:
        """Tests push where Gerrit does not return a CL (e.g. no changes)."""
        mock_run.return_value = subprocess.CompletedProcess(
            args=[], returncode=0, stdout='', stderr='No changes left to push'
        )
        async with self.workspace.branch() as branch:
            cl_num = await branch.push()
            self.assertIsNone(cl_num)

    @patch('subprocess.run')
    async def test_diff_unstaged(self, mock_run) -> None:
        """Tests retrieving unstaged working tree diff lines from GitBranch."""
        mock_run.return_value = subprocess.CompletedProcess(
            args=[], returncode=0, stdout='line1\nline2\n', stderr=''
        )
        async with self.workspace.branch() as branch:
            mock_run.reset_mock()
            diff_lines = await branch.diff(staged=False)
            self.assertEqual(diff_lines, ['line1', 'line2'])
            mock_run.assert_called_once_with(
                ['git', '-C', str(self.project_dir), 'diff'],
                check=True,
                capture_output=True,
                env=_git_env(),
                text=True,
            )

    @patch('subprocess.run')
    async def test_diff_staged(self, mock_run) -> None:
        """Tests retrieving staged diff lines from GitBranch."""
        mock_run.return_value = subprocess.CompletedProcess(
            args=[],
            returncode=0,
            stdout='staged_line1\nstaged_line2\n',
            stderr='',
        )
        async with self.workspace.branch() as branch:
            mock_run.reset_mock()
            diff_lines = await branch.diff(staged=True)
            self.assertEqual(diff_lines, ['staged_line1', 'staged_line2'])
            mock_run.assert_called_once_with(
                ['git', '-C', str(self.project_dir), 'diff', '--staged'],
                check=True,
                capture_output=True,
                env=_git_env(),
                text=True,
            )

    @patch('subprocess.run')
    async def test_commit_msg(self, mock_run) -> None:
        """Tests retrieving commit message lines from GitBranch.commit_msg."""
        mock_run.return_value = subprocess.CompletedProcess(
            args=[],
            returncode=0,
            stdout='Commit Title\n\nCommit Body\n',
            stderr='',
        )
        async with self.workspace.branch() as branch:
            mock_run.reset_mock()
            lines = await branch.commit_msg()
            self.assertEqual(lines, ['Commit Title', '', 'Commit Body'])
            mock_run.assert_called_once_with(
                [
                    'git',
                    '-C',
                    str(self.project_dir),
                    'log',
                    '-1',
                    '--format=%B',
                    'HEAD',
                ],
                check=True,
                capture_output=True,
                env=_git_env(),
                text=True,
            )

    @patch('subprocess.run')
    async def test_branch_reset_unstaged(self, mock_run) -> None:
        """Tests reset(staged=False) reverts working tree and cleans files."""
        mock_run.return_value = subprocess.CompletedProcess(
            args=[],
            returncode=0,
            stdout='',
            stderr='',
        )
        async with self.workspace.branch(keep=True) as branch:
            mock_run.reset_mock()
            await branch.reset(staged=False)
            self.assertEqual(
                mock_run.call_args_list,
                [
                    unittest.mock.call(
                        [
                            'git',
                            '-C',
                            str(self.project_dir),
                            'checkout',
                            '--',
                            '.',
                        ],
                        check=True,
                        capture_output=True,
                        env=_git_env(),
                    ),
                    unittest.mock.call(
                        ['git', '-C', str(self.project_dir), 'clean', '-fd'],
                        check=True,
                        capture_output=True,
                        env=_git_env(),
                    ),
                ],
            )

    @patch('subprocess.run')
    async def test_branch_reset_staged(self, mock_run) -> None:
        """Tests reset(staged=True) resets index and cleans files."""
        mock_run.return_value = subprocess.CompletedProcess(
            args=[],
            returncode=0,
            stdout='',
            stderr='',
        )
        async with self.workspace.branch(keep=True) as branch:
            mock_run.reset_mock()
            await branch.reset(staged=True)
            self.assertEqual(
                mock_run.call_args_list,
                [
                    unittest.mock.call(
                        [
                            'git',
                            '-C',
                            str(self.project_dir),
                            'reset',
                            '--hard',
                        ],
                        check=True,
                        capture_output=True,
                        env=_git_env(),
                    ),
                    unittest.mock.call(
                        ['git', '-C', str(self.project_dir), 'clean', '-fd'],
                        check=True,
                        capture_output=True,
                        env=_git_env(),
                    ),
                ],
            )

    @patch('subprocess.run')
    async def test_branch_keep_skips_cleanup(self, mock_run) -> None:
        """Tests that keep=True skips cleanup operations on context exit."""
        mock_run.return_value = subprocess.CompletedProcess(
            args=[],
            returncode=0,
            stdout='',
            stderr='',
        )
        async with self.workspace.branch(keep=True) as branch:
            self.assertTrue(branch.keep)
            mock_run.reset_mock()

        # On exit with keep=True, it switches back to previous branch
        # without deleting it.
        mock_run.assert_called_once_with(
            ['git', '-C', str(self.project_dir), 'checkout', 'main'],
            check=True,
            capture_output=True,
            env=_git_env(),
        )

    @patch('subprocess.run')
    def test_clone_existing_repo_on_main(self, mock_run) -> None:
        """Tests that cloning an existing clean repo on main pulls updates."""
        project_dir = Path('/tmp/existing_project_main').resolve()
        self.fs.create_dir(project_dir / '.git')

        def fake_run(cmd, **_kwargs):
            if 'remote' in cmd and 'get-url' in cmd:
                return subprocess.CompletedProcess(
                    args=cmd, returncode=0, stdout=self.repo_url + '\n'
                )
            if 'status' in cmd and '--porcelain' in cmd:
                return subprocess.CompletedProcess(
                    args=cmd, returncode=0, stdout=''
                )
            if 'branch' in cmd and '--show-current' in cmd:
                return subprocess.CompletedProcess(
                    args=cmd, returncode=0, stdout='main\n'
                )
            if 'pull' in cmd:
                return subprocess.CompletedProcess(
                    args=cmd, returncode=0, stdout=''
                )
            return subprocess.CompletedProcess(
                args=cmd, returncode=0, stdout=''
            )

        mock_run.side_effect = fake_run

        workspace = ReadOnlyGitWorkspace.clone_sync(self.repo_url, project_dir)
        self.assertEqual(workspace.project_dir, project_dir.resolve())

        called_cmds = [c[0][0] for c in mock_run.call_args_list]
        resolved_path = str(project_dir.resolve())
        self.assertIn(
            ['git', '-C', resolved_path, 'remote', 'get-url', 'origin'],
            called_cmds,
        )
        self.assertIn(
            ['git', '-C', resolved_path, 'status', '--porcelain'],
            called_cmds,
        )
        self.assertIn(
            ['git', '-C', resolved_path, 'branch', '--show-current'],
            called_cmds,
        )
        self.assertIn(['git', '-C', resolved_path, 'pull'], called_cmds)
        self.assertFalse(any('clone' in cmd for cmd in called_cmds))

    @patch('subprocess.run')
    def test_clone_existing_repo_switch_to_main(self, mock_run) -> None:
        """Tests that cloning repo on other branch switches to main."""
        project_dir = Path('/tmp/existing_project_branch').resolve()
        self.fs.create_dir(project_dir / '.git')

        def fake_run(cmd, **_kwargs):
            if 'remote' in cmd and 'get-url' in cmd:
                return subprocess.CompletedProcess(
                    args=cmd, returncode=0, stdout=self.repo_url + '\n'
                )
            if 'status' in cmd and '--porcelain' in cmd:
                return subprocess.CompletedProcess(
                    args=cmd, returncode=0, stdout=''
                )
            if 'branch' in cmd and '--show-current' in cmd:
                return subprocess.CompletedProcess(
                    args=cmd, returncode=0, stdout='feature_branch\n'
                )
            return subprocess.CompletedProcess(
                args=cmd, returncode=0, stdout=''
            )

        mock_run.side_effect = fake_run

        workspace = ReadOnlyGitWorkspace.clone_sync(self.repo_url, project_dir)
        self.assertEqual(workspace.project_dir, project_dir.resolve())

        called_cmds = [c[0][0] for c in mock_run.call_args_list]
        resolved_path = str(project_dir.resolve())
        self.assertIn(
            ['git', '-C', resolved_path, 'checkout', 'main'],
            called_cmds,
        )
        self.assertIn(['git', '-C', resolved_path, 'pull'], called_cmds)

    @patch('subprocess.run')
    def test_clone_existing_repo_mismatched_remote_url(self, mock_run) -> None:
        """Tests error raised if existing repo has a different remote URL."""
        project_dir = Path('/tmp/existing_project_mismatch').resolve()
        self.fs.create_dir(project_dir / '.git')

        mock_run.return_value = subprocess.CompletedProcess(
            args=[],
            returncode=0,
            stdout='https://other.com/different.git\n',
        )

        with self.assertRaises(RuntimeError) as ctx:
            ReadOnlyGitWorkspace.clone_sync(self.repo_url, project_dir)

        self.assertIn('remote URL', str(ctx.exception))

    @patch('subprocess.run')
    def test_clone_existing_repo_uncommitted_changes(self, mock_run) -> None:
        """Tests error raised if existing repo has uncommitted changes."""
        project_dir = Path('/tmp/existing_project_uncommitted').resolve()
        self.fs.create_dir(project_dir / '.git')

        def fake_run(cmd, **_kwargs):
            if 'remote' in cmd and 'get-url' in cmd:
                return subprocess.CompletedProcess(
                    args=cmd, returncode=0, stdout=self.repo_url + '\n'
                )
            if 'status' in cmd and '--porcelain' in cmd:
                return subprocess.CompletedProcess(
                    args=cmd, returncode=0, stdout=' M modified_file.py\n'
                )
            return subprocess.CompletedProcess(
                args=cmd, returncode=0, stdout=''
            )

        mock_run.side_effect = fake_run

        with self.assertRaises(RuntimeError) as ctx:
            ReadOnlyGitWorkspace.clone_sync(self.repo_url, project_dir)

        self.assertIn('uncommitted changes', str(ctx.exception))

    @patch('subprocess.run')
    async def test_clone_existing_repo_async(self, mock_run) -> None:
        """Tests async clone on an existing repo."""
        project_dir = Path('/tmp/existing_project_async').resolve()
        self.fs.create_dir(project_dir / '.git')

        def fake_run(cmd, **_kwargs):
            if 'remote' in cmd and 'get-url' in cmd:
                return subprocess.CompletedProcess(
                    args=cmd, returncode=0, stdout=self.repo_url + '\n'
                )
            if 'status' in cmd and '--porcelain' in cmd:
                return subprocess.CompletedProcess(
                    args=cmd, returncode=0, stdout=''
                )
            if 'branch' in cmd and '--show-current' in cmd:
                return subprocess.CompletedProcess(
                    args=cmd, returncode=0, stdout='main\n'
                )
            return subprocess.CompletedProcess(
                args=cmd, returncode=0, stdout=''
            )

        mock_run.side_effect = fake_run

        workspace = await ReadOnlyGitWorkspace.clone(self.repo_url, project_dir)
        self.assertEqual(workspace.project_dir, project_dir.resolve())


class TestGetGitRepoUrl(unittest.TestCase):
    """Unit tests for get_git_repo_url helper function."""

    @patch('subprocess.run')
    def test_get_git_repo_url_explicit_path(self, mock_run) -> None:
        """Tests get_git_repo_url with explicit path."""
        mock_run.return_value = subprocess.CompletedProcess(
            args=[],
            returncode=0,
            stdout='sso://pigweed/pigweed\n',
            stderr='',
        )
        url = get_git_repo_url('/some/repo')
        self.assertEqual(url, 'sso://pigweed/pigweed')
        mock_run.assert_called_once_with(
            ['git', '-C', '/some/repo', 'remote', 'get-url', 'origin'],
            check=True,
            capture_output=True,
            env=_git_env(),
            text=True,
        )

    @patch('subprocess.run')
    def test_get_git_repo_url_failure(self, mock_run) -> None:
        """Tests get_git_repo_url raises subprocess error on failure."""
        mock_run.side_effect = subprocess.CalledProcessError(1, ['git'])
        with self.assertRaises(subprocess.CalledProcessError):
            get_git_repo_url('/not/a/git/repo')


class TestGetGitRepoRoot(unittest.TestCase):
    """Unit tests for get_git_repo_root helper function."""

    @patch('subprocess.run')
    def test_get_git_repo_root_explicit_path(self, mock_run) -> None:
        """Tests get_git_repo_root with explicit path."""
        mock_run.return_value = subprocess.CompletedProcess(
            args=[],
            returncode=0,
            stdout='/some/repo\n',
            stderr='',
        )
        root = get_git_repo_root('/some/repo/subdir')
        self.assertEqual(root, Path('/some/repo').resolve())
        mock_run.assert_called_once_with(
            ['git', '-C', '/some/repo/subdir', 'rev-parse', '--show-toplevel'],
            check=False,
            capture_output=True,
            env=_git_env(),
            text=True,
        )

    @patch.dict(os.environ, {'BUILD_WORKSPACE_DIRECTORY': '/workspace/root'})
    @patch('subprocess.run')
    def test_get_git_repo_root_build_workspace_directory(
        self, mock_run
    ) -> None:
        """Tests get_git_repo_root using BUILD_WORKSPACE_DIRECTORY."""
        mock_run.return_value = subprocess.CompletedProcess(
            args=[],
            returncode=0,
            stdout='/workspace/root\n',
            stderr='',
        )
        root = get_git_repo_root()
        self.assertEqual(root, Path('/workspace/root').resolve())
        mock_run.assert_called_once_with(
            ['git', '-C', '/workspace/root', 'rev-parse', '--show-toplevel'],
            check=False,
            capture_output=True,
            env=_git_env(),
            text=True,
        )

    @patch.dict(os.environ, {}, clear=True)
    @patch('subprocess.run')
    def test_get_git_repo_root_not_in_git_returns_none(self, mock_run) -> None:
        """Tests get_git_repo_root returns None when not in git repository."""
        mock_run.return_value = subprocess.CompletedProcess(
            args=[],
            returncode=128,
            stdout='',
            stderr='fatal: not a git repository\n',
        )
        root = get_git_repo_root('/some/dir')
        self.assertIsNone(root)


if __name__ == '__main__':
    unittest.main()
