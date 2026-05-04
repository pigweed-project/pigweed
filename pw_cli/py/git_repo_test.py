#!/usr/bin/env python3
# Copyright 2024 The Pigweed Authors
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
"""git repo module tests"""

import os
import sys
import tempfile
from pathlib import Path
from subprocess import CompletedProcess
from typing import Any
import re
import shlex
import unittest
from unittest import mock

from pw_cli.tool_runner import ToolRunner
from pw_cli.git_repo import GitRepo, GitRepoFinder, RebaseInfo
from pw_cli.file_filter import FileFilter
from pw_cli.git_repo import collect_files
from pyfakefs import fake_filesystem_unittest


class FakeGitToolRunner(ToolRunner):
    def __init__(self, command_results: dict[str, CompletedProcess]) -> None:
        self._results = command_results
        self.calls: list[tuple[str, tuple[str, ...], dict[str, Any]]] = []

    def _run_tool(self, tool: str, args, **kwargs) -> CompletedProcess:
        self.calls.append((tool, args, kwargs))
        full_command = shlex.join((tool, *tuple(args)))
        for cmd, result in self._results.items():
            if cmd in full_command:
                return result

        return CompletedProcess(
            args=full_command,
            returncode=0xFF,
            stderr=f'I do not know how to `{full_command}`'.encode(),
            stdout=b'Failed to execute command',
        )


def git_ok(cmd: str, stdout: str) -> CompletedProcess:
    return CompletedProcess(
        args=cmd,
        returncode=0,
        stderr='',
        stdout=stdout.encode(),
    )


def git_err(cmd: str, stderr: str, returncode: int = 1) -> CompletedProcess:
    return CompletedProcess(
        args=cmd,
        returncode=returncode,
        stderr=stderr.encode(),
        stdout='',
    )


class TestGitRepo(unittest.TestCase):
    """Tests for git_repo.py"""

    GIT_ROOT = Path("/dev/null/test").resolve()
    SUBMODULES = [
        Path("/dev/null/test/third_party/pigweed").resolve(),
        Path("/dev/null/test/vendor/anycom/p1").resolve(),
        Path("/dev/null/test/vendor/anycom/p2").resolve(),
    ]
    GIT_SUBMODULES_OUT = "\n".join([str(x) for x in SUBMODULES])

    EXPECTED_SUBMODULE_LIST_CMD = shlex.join(
        (
            'submodule',
            'foreach',
            '--quiet',
            '--recursive',
            'echo $toplevel/$sm_path',
        )
    )

    def make_fake_git_repo(self, cmds):
        return GitRepo(self.GIT_ROOT, FakeGitToolRunner(cmds))

    def test_mock_root(self):
        """Ensure our mock works since so many of our tests depend upon it."""
        cmds = {}
        repo = self.make_fake_git_repo(cmds)
        self.assertEqual(repo.root(), self.GIT_ROOT)

    def test_list_submodules_1(self):
        """Ensures the root git repo appears in the submodule list."""
        cmds = {
            self.EXPECTED_SUBMODULE_LIST_CMD: git_ok(
                self.EXPECTED_SUBMODULE_LIST_CMD, self.GIT_SUBMODULES_OUT
            )
        }
        repo = self.make_fake_git_repo(cmds)
        paths = repo.list_submodules()
        self.assertNotIn(self.GIT_ROOT, paths)

    def test_list_submodules_2(self):
        cmds = {
            self.EXPECTED_SUBMODULE_LIST_CMD: git_ok(
                self.EXPECTED_SUBMODULE_LIST_CMD, self.GIT_SUBMODULES_OUT
            )
        }
        repo = self.make_fake_git_repo(cmds)
        paths = repo.list_submodules()
        self.assertIn(self.SUBMODULES[2], paths)

    def test_list_submodules_with_exclude_str(self):
        cmds = {
            self.EXPECTED_SUBMODULE_LIST_CMD: git_ok(
                self.EXPECTED_SUBMODULE_LIST_CMD, self.GIT_SUBMODULES_OUT
            )
        }
        repo = self.make_fake_git_repo(cmds)
        paths = repo.list_submodules(
            excluded_paths=(self.GIT_ROOT.as_posix(),),
        )
        self.assertNotIn(self.GIT_ROOT, paths)

    def test_list_submodules_with_exclude_regex(self):
        cmds = {
            self.EXPECTED_SUBMODULE_LIST_CMD: git_ok(
                self.EXPECTED_SUBMODULE_LIST_CMD, self.GIT_SUBMODULES_OUT
            )
        }
        repo = self.make_fake_git_repo(cmds)
        paths = repo.list_submodules(
            excluded_paths=(re.compile("third_party/.*"),),
        )
        self.assertNotIn(self.SUBMODULES[0], paths)

    def test_list_submodules_with_exclude_str_miss(self):
        cmds = {
            self.EXPECTED_SUBMODULE_LIST_CMD: git_ok(
                self.EXPECTED_SUBMODULE_LIST_CMD, self.GIT_SUBMODULES_OUT
            )
        }
        repo = self.make_fake_git_repo(cmds)
        paths = repo.list_submodules(
            excluded_paths=(re.compile("pigweed"),),
        )
        self.assertIn(self.SUBMODULES[-1], paths)

    def test_list_submodules_with_exclude_regex_miss_1(self):
        cmds = {
            self.EXPECTED_SUBMODULE_LIST_CMD: git_ok(
                self.EXPECTED_SUBMODULE_LIST_CMD, self.GIT_SUBMODULES_OUT
            )
        }
        repo = self.make_fake_git_repo(cmds)
        paths = repo.list_submodules(
            excluded_paths=(re.compile("foo/.*"),),
        )
        self.assertNotIn(self.GIT_ROOT, paths)
        for module in self.SUBMODULES:
            self.assertIn(module, paths)

    def test_list_submodules_with_exclude_regex_miss_2(self):
        cmds = {
            self.EXPECTED_SUBMODULE_LIST_CMD: git_ok(
                self.EXPECTED_SUBMODULE_LIST_CMD, self.GIT_SUBMODULES_OUT
            )
        }
        repo = self.make_fake_git_repo(cmds)
        paths = repo.list_submodules(
            excluded_paths=(re.compile("pigweed"),),
        )
        self.assertNotIn(self.GIT_ROOT, paths)
        for module in self.SUBMODULES:
            self.assertIn(module, paths)

    def test_list_files_unknown_hash(self):
        bad_cmd = "diff --name-only --diff-filter=d 'something' --"
        good_cmd = 'ls-files --'
        fake_path = 'path/to/foo.h'
        cmds = {
            bad_cmd: git_err(bad_cmd, "fatal: bad revision 'something'"),
            good_cmd: git_ok(good_cmd, fake_path + '\n'),
        }

        expected_file_path = self.GIT_ROOT / Path(fake_path)
        repo = self.make_fake_git_repo(cmds)

        # This function needs to be mocked because it does a `is_file()` check
        # on returned paths. Since we're not using real files, nothing will
        # be yielded.
        repo._ls_files = mock.MagicMock(  # pylint: disable=protected-access
            return_value=[expected_file_path]
        )
        paths = repo.list_files(commit='something')
        self.assertIn(expected_file_path, paths)

    def test_fake_uncommitted_changes(self):
        index_update = 'update-index -q --refresh'
        diff_index = 'diff-index --quiet HEAD --'
        cmds = {
            index_update: git_ok(index_update, ''),
            diff_index: git_err(diff_index, '', returncode=1),
        }
        repo = self.make_fake_git_repo(cmds)
        self.assertTrue(repo.has_uncommitted_changes())

    def test_name_rev(self):
        git_cmd = 'name-rev --no-undefined --name-only HEAD'
        cmds = {
            git_cmd: git_ok(git_cmd, 'main'),
        }
        repo = self.make_fake_git_repo(cmds)
        self.assertEqual(repo.name_rev(), 'main')

    def test_name_rev_undefined_fallback(self):
        name_rev_cmd = 'name-rev --no-undefined --name-only HEAD'
        rev_parse_cmd = 'rev-parse --short HEAD'
        cmds = {
            name_rev_cmd: git_err(
                name_rev_cmd, 'fatal: cannot describe', returncode=128
            ),
            rev_parse_cmd: git_ok(rev_parse_cmd, '1234567'),
        }
        repo = self.make_fake_git_repo(cmds)
        self.assertEqual(repo.name_rev(), '1234567')

    def test_commit_count(self):
        git_cmd_1 = 'rev-list --count HEAD'
        git_cmd_2 = "rev-list --count abc..def"

        repo = self.make_fake_git_repo(
            {
                git_cmd_1: git_ok(git_cmd_1, '1'),
                git_cmd_2: git_ok(git_cmd_2, '2'),
            }
        )
        self.assertEqual(repo.commit_count('HEAD'), 1)
        self.assertEqual(repo.commit_count('abc', 'def'), 2)

    def test_is_in_rebase(self):
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)
            git_cmd = 'rev-parse --absolute-git-dir'
            cmds = {
                git_cmd: git_ok(git_cmd, str(tmp_path / '.git')),
            }
            repo = GitRepo(tmp_path, FakeGitToolRunner(cmds))

            git_dir = tmp_path / '.git'
            git_dir.mkdir()

            rebase_merge = git_dir / 'rebase-merge'
            rebase_apply = git_dir / 'rebase-apply'

            # Test neither exists
            self.assertFalse(repo.is_in_rebase())

            # Test rebase-merge exists
            rebase_merge.mkdir()
            self.assertTrue(repo.is_in_rebase())
            rebase_merge.rmdir()

            # Test rebase-apply exists
            rebase_apply.mkdir()
            self.assertTrue(repo.is_in_rebase())
            rebase_apply.rmdir()

    def test_rebase_info(self):
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)
            git_cmd = 'rev-parse --absolute-git-dir'
            cmds = {
                git_cmd: git_ok(git_cmd, str(tmp_path / '.git')),
            }
            repo = GitRepo(tmp_path, FakeGitToolRunner(cmds))

            git_dir = tmp_path / '.git'
            git_dir.mkdir()

            rebase_merge = git_dir / 'rebase-merge'

            # Test not in rebase
            self.assertIsNone(repo.rebase_info())

            # Test in rebase
            rebase_merge.mkdir()
            (rebase_merge / 'onto').write_text('1234567890abcdef')
            (rebase_merge / 'orig-head').write_text('abcdef1234567890')

            info = repo.rebase_info()
            self.assertIsNotNone(info)
            self.assertIsInstance(info, RebaseInfo)
            self.assertEqual(info.onto, '1234567890abcdef')
            self.assertEqual(info.orig_head, 'abcdef1234567890')

    def test_rebase_interactive(self):
        """Verify interactive rebase editor script."""
        expected_cmd = 'rebase -i origin/main'
        runner = FakeGitToolRunner({expected_cmd: git_ok(expected_cmd, '')})
        repo = GitRepo(self.GIT_ROOT, runner)
        repo.modify().rebase_interactive('origin/main')

        self.assertEqual(len(runner.calls), 1)
        tool, args, kwargs = runner.calls[0]
        self.assertEqual(tool, 'git')
        self.assertIn('rebase', args)
        self.assertIn('-i', args)
        self.assertIn('origin/main', args)

        # Extract the script and run it to verify it works as expected.
        editor_cmd = kwargs['env']['GIT_SEQUENCE_EDITOR']
        script = editor_cmd.split("'", 1)[1].rstrip("'")

        with tempfile.NamedTemporaryFile(mode='w+', delete=False) as f:
            f.write("pick 1234 normal commit\n")
            f.write("pick 5678 cherry-pick this\n")
            f.write("pick 9012 pick in subject\n")
            f.close()

            with mock.patch.object(sys, 'argv', [sys.executable, f.name]):
                exec(script, {})  # pylint: disable=exec-used

            result = Path(f.name).read_text()
            Path(f.name).unlink()

        self.assertEqual(
            result,
            "edit 1234 normal commit\n"
            "edit 5678 cherry-pick this\n"
            "edit 9012 pick in subject\n",
        )

    def test_rebase_continue(self):
        git_cmd = 'rebase --continue'
        cmds = {
            git_cmd: git_ok(git_cmd, ''),
        }
        repo = self.make_fake_git_repo(cmds)
        repo.modify().rebase_continue()

    def test_amend_commit_with_updated_files(self):
        cmds = {
            'add -u': git_ok('add -u', ''),
            'commit --amend --no-edit': git_ok('commit --amend --no-edit', ''),
        }
        runner = FakeGitToolRunner(cmds)
        repo = GitRepo(self.GIT_ROOT, runner)
        repo.modify().amend_commit_with_updated_files()

        self.assertEqual(len(runner.calls), 2)
        tool1, args1, _ = runner.calls[0]
        tool2, args2, _ = runner.calls[1]

        self.assertEqual(tool1, 'git')
        self.assertIn('add', args1)
        self.assertIn('-u', args1)

        self.assertEqual(tool2, 'git')
        self.assertIn('commit', args2)
        self.assertIn('--amend', args2)
        self.assertIn('--no-edit', args2)


def _resolve(path: str) -> str:
    """Needed to make Windows happy.

    Since resolved paths start with drive letters, any literal string
    paths in these tests need to be resolved so they are prefixed with `C:`.
    """
    # Avoid manipulation on other OSes since they don't strictly require it.
    if os.name != 'nt':
        return path
    return str(Path(path).resolve())


class TestGitRepoFinder(fake_filesystem_unittest.TestCase):
    """Tests for GitRepoFinder."""

    FAKE_ROOT = _resolve('/dev/null/fake/root')
    FAKE_NESTED_REPO = _resolve('/dev/null/fake/root/third_party/bogus')

    def setUp(self):
        self.setUpPyfakefs()
        self.fs.create_dir(self.FAKE_ROOT)
        os.chdir(self.FAKE_ROOT)

    def test_cwd_is_root(self):
        """Tests when cwd is the root of a repo."""
        expected_repo_query = shlex.join(
            (
                '-C',
                '.',
                'rev-parse',
                '--show-toplevel',
            )
        )
        runner = FakeGitToolRunner(
            {expected_repo_query: git_ok(expected_repo_query, self.FAKE_ROOT)}
        )
        finder = GitRepoFinder(runner)
        path_to_search = '.'
        maybe_repo = finder.find_git_repo(path_to_search)
        self.assertNotEqual(
            maybe_repo, None, f'Could not resolve {path_to_search}'
        )
        self.assertEqual(maybe_repo.root(), Path(self.FAKE_ROOT))

    def test_cwd_is_not_repo(self):
        """Tests when cwd is not tracked by a repo."""
        expected_repo_query = shlex.join(
            (
                '-C',
                '.',
                'rev-parse',
                '--show-toplevel',
            )
        )
        runner = FakeGitToolRunner(
            {expected_repo_query: git_err(expected_repo_query, self.FAKE_ROOT)}
        )
        finder = GitRepoFinder(runner)
        self.assertEqual(finder.find_git_repo('.'), None)

    def test_file(self):
        """Tests a file at the root of a repo."""
        expected_repo_query = shlex.join(
            (
                '-C',
                '.',
                'rev-parse',
                '--show-toplevel',
            )
        )
        runner = FakeGitToolRunner(
            {expected_repo_query: git_ok(expected_repo_query, self.FAKE_ROOT)}
        )
        finder = GitRepoFinder(runner)
        path_to_search = 'foo.txt'
        self.fs.create_file(path_to_search)
        maybe_repo = finder.find_git_repo(path_to_search)
        self.assertNotEqual(
            maybe_repo, None, f'Could not resolve {path_to_search}'
        )
        self.assertEqual(maybe_repo.root(), Path(self.FAKE_ROOT))

    def test_parents_memoized(self):
        """Tests multiple queries that are optimized via memoization."""
        expected_repo_query = shlex.join(
            (
                '-C',
                str(Path('subdir/nested')),
                'rev-parse',
                '--show-toplevel',
            )
        )
        runner = FakeGitToolRunner(
            {expected_repo_query: git_ok(expected_repo_query, self.FAKE_ROOT)}
        )
        finder = GitRepoFinder(runner)

        # Because of the ordering, only ONE call to git should be necessary.
        paths = [
            'subdir/nested/foo.txt',
            'subdir/bar.txt',
            'subdir/nested/baz.txt',
            'bleh.txt',
        ]
        for file_to_find in paths:
            self.fs.create_file(file_to_find)
            maybe_repo = finder.find_git_repo(file_to_find)
            self.assertNotEqual(
                maybe_repo, None, f'Could not resolve {file_to_find}'
            )
            self.assertEqual(maybe_repo.root(), Path(self.FAKE_ROOT))

    def test_absolute_path(self):
        """Test that absolute paths hit memoized paths."""
        expected_repo_query = shlex.join(
            (
                '-C',
                str(Path('subdir/nested')),
                'rev-parse',
                '--show-toplevel',
            )
        )
        runner = FakeGitToolRunner(
            {expected_repo_query: git_ok(expected_repo_query, self.FAKE_ROOT)}
        )
        finder = GitRepoFinder(runner)

        # Because of the ordering, only ONE call to git should be necessary.
        paths = [
            'subdir/nested/foo.txt',
            _resolve(f'{self.FAKE_ROOT}/subdir/bar.txt'),
        ]
        for file_to_find in paths:
            self.fs.create_file(file_to_find)
            maybe_repo = finder.find_git_repo(file_to_find)
            self.assertNotEqual(
                maybe_repo, None, f'Could not resolve {file_to_find}'
            )
            self.assertEqual(maybe_repo.root(), Path(self.FAKE_ROOT))

    def test_subdir(self):
        """Test that querying a dir properly memoizes things."""
        expected_repo_query = shlex.join(
            (
                '-C',
                'subdir',
                'rev-parse',
                '--show-toplevel',
            )
        )
        runner = FakeGitToolRunner(
            {expected_repo_query: git_ok(expected_repo_query, self.FAKE_ROOT)}
        )
        finder = GitRepoFinder(runner)

        dir_to_check = 'subdir'
        self.fs.create_dir(dir_to_check)
        maybe_repo = finder.find_git_repo(dir_to_check)
        self.assertNotEqual(
            maybe_repo, None, f'Could not resolve {dir_to_check}'
        )
        self.assertEqual(maybe_repo.root(), Path(self.FAKE_ROOT))

    def test_nested_repo(self):
        """Test a nested repo works as expected."""
        expected_inner_repo_query = shlex.join(
            (
                '-C',
                str(Path('third_party/bogus/test')),
                'rev-parse',
                '--show-toplevel',
            )
        )
        expected_outer_repo_query = shlex.join(
            (
                '-C',
                'test',
                'rev-parse',
                '--show-toplevel',
            )
        )
        runner = FakeGitToolRunner(
            {
                expected_inner_repo_query: git_ok(
                    expected_inner_repo_query, self.FAKE_NESTED_REPO
                ),
                expected_outer_repo_query: git_ok(
                    expected_outer_repo_query, self.FAKE_ROOT
                ),
            }
        )
        finder = GitRepoFinder(runner)

        inner_repo_file = "third_party/bogus/test/baz.txt"
        self.fs.create_file(inner_repo_file)
        maybe_repo = finder.find_git_repo(inner_repo_file)
        self.assertNotEqual(
            maybe_repo, None, f'Could not resolve {inner_repo_file}'
        )
        self.assertEqual(maybe_repo.root(), Path(self.FAKE_NESTED_REPO))

        outer_repo_file = "test/baz.txt"
        self.fs.create_file(outer_repo_file)
        maybe_repo = finder.find_git_repo(outer_repo_file)
        self.assertNotEqual(
            maybe_repo, None, f'Could not resolve {outer_repo_file}'
        )
        self.assertEqual(maybe_repo.root(), Path(self.FAKE_ROOT))

    def test_absolute_repo_not_under_cwd(self):
        """Test an absolute path that isn't a subdir of cwd works."""
        fake_parallel_repo = _resolve('/dev/null/fake/parallel')
        expected_repo_query = shlex.join(
            (
                '-C',
                _resolve('/dev/null/fake/parallel/yep'),
                'rev-parse',
                '--show-toplevel',
            )
        )
        runner = FakeGitToolRunner(
            {
                expected_repo_query: git_ok(
                    expected_repo_query, fake_parallel_repo
                )
            }
        )
        finder = GitRepoFinder(runner)
        path_to_search = _resolve('/dev/null/fake/parallel/yep/foo.txt')
        self.fs.create_file(path_to_search)
        maybe_repo = finder.find_git_repo(path_to_search)
        self.assertNotEqual(
            maybe_repo, None, f'Could not resolve {path_to_search}'
        )
        self.assertEqual(maybe_repo.root(), Path(fake_parallel_repo))

    def test_absolute_not_under_cwd(self):
        """Test files not tracked by a repo."""
        expected_repo_query = shlex.join(
            (
                '-C',
                _resolve('/dev/null/fake/parallel/yep'),
                'rev-parse',
                '--show-toplevel',
            )
        )
        runner = FakeGitToolRunner(
            {expected_repo_query: git_err(expected_repo_query, '')}
        )
        finder = GitRepoFinder(runner)
        # Because of the ordering, only ONE call to git should be necessary.
        paths = [
            _resolve('/dev/null/fake/parallel/yep/foo.txt'),
            _resolve('/dev/null/fake/bar.txt'),
            _resolve('/dev/null/fake/parallel/yep'),
        ]
        for file_to_find in paths:
            if file_to_find.endswith('.txt'):
                self.fs.create_file(file_to_find)
            self.assertEqual(finder.find_git_repo(file_to_find), None)

    def test_make_pathspec_relative(self):
        """Tests that pathspec relativization works."""
        expected_queries = (
            (
                shlex.join(
                    (
                        '-C',
                        str(Path('george/one')),
                        'rev-parse',
                        '--show-toplevel',
                    )
                ),
                self.FAKE_ROOT,
            ),
            (
                shlex.join(
                    (
                        '-C',
                        str(Path('third_party/bogus')),
                        'rev-parse',
                        '--show-toplevel',
                    )
                ),
                self.FAKE_NESTED_REPO,
            ),
            (
                shlex.join(
                    (
                        '-C',
                        str(Path('third_party/bogus/frob')),
                        'rev-parse',
                        '--show-toplevel',
                    )
                ),
                self.FAKE_NESTED_REPO,
            ),
        )
        runner = FakeGitToolRunner(
            {
                expected_args: git_ok(expected_args, repo)
                for expected_args, repo in expected_queries
            }
        )
        finder = GitRepoFinder(runner)

        files = [
            'george/one/two.txt',
            'third_party/bogus/sad.png',
        ]
        for file_to_find in files:
            self.fs.create_file(file_to_find)
        self.fs.create_dir('third_party/bogus/frob')

        pathspecs = {
            'george/one/two.txt': str(Path('george/one/two.txt')),
            'a/': 'a',
            'third_party/bogus/sad.png': 'sad.png',
            'third_party/bogus/': '.',
            'third_party/bogus/frob/j*': str(Path('frob/j*')),
        }
        for pathspec, expected in pathspecs.items():
            maybe_repo, relativized = finder.make_pathspec_relative(pathspec)
            self.assertNotEqual(
                maybe_repo, None, f'Could not resolve {pathspec}'
            )
            self.assertEqual(relativized, expected)

    def test_make_pathspec_relative_untracked(self):
        """Tests that untracked files work with relativization."""
        expected_repo_query = shlex.join(
            (
                '-C',
                str(Path('subdir/nested')),
                'rev-parse',
                '--show-toplevel',
            )
        )
        runner = FakeGitToolRunner(
            {expected_repo_query: git_err(expected_repo_query, '')}
        )
        finder = GitRepoFinder(runner)

        self.fs.create_file('george/one/two.txt')

        pathspecs = {
            'george/one/two.txt': 'george/one/two.txt',
        }
        for pathspec, expected in pathspecs.items():
            maybe_repo, relativized = finder.make_pathspec_relative(pathspec)
            self.assertEqual(
                maybe_repo, None, f'Unexpectedly resolved {pathspec}'
            )
            self.assertEqual(relativized, expected)

    def test_make_pathspec_relative_absolute(self):
        """Tests that absolute paths work with relativization."""
        expected_repo_query = shlex.join(
            (
                '-C',
                _resolve('/dev/null/fake/root/third_party/bogus/one'),
                'rev-parse',
                '--show-toplevel',
            )
        )
        runner = FakeGitToolRunner(
            {
                expected_repo_query: git_ok(
                    expected_repo_query, self.FAKE_NESTED_REPO
                )
            }
        )
        finder = GitRepoFinder(runner)

        self.fs.create_file('third_party/bogus/one/two.txt')

        pathspecs = {
            _resolve('/dev/null/fake/root/third_party/bogus/one/two.txt'): str(
                Path('one/two.txt')
            ),
        }
        for pathspec, expected in pathspecs.items():
            maybe_repo, relativized = finder.make_pathspec_relative(pathspec)
            self.assertNotEqual(
                maybe_repo, None, f'Could not resolve {pathspec}'
            )
            self.assertEqual(relativized, expected)


class TestCollectFiles(fake_filesystem_unittest.TestCase):
    """Tests for collect_files."""

    def setUp(self) -> None:
        self.setUpPyfakefs()
        self.root = Path(_resolve('/repo'))
        self.fs.create_dir(self.root)
        os.chdir(self.root)

    @mock.patch('pw_cli.git_repo.describe_git_pattern')
    def test_collect_all_files(self, mock_describe: mock.MagicMock) -> None:
        mock_describe.return_value = 'mocked description'

        runner = FakeGitToolRunner(
            {
                'rev-parse': git_ok('rev-parse', '/repo'),
                'ls-files': git_ok('ls-files', 'file1.txt\nfile2.py'),
            }
        )

        self.fs.create_file(self.root / 'file1.txt')
        self.fs.create_file(self.root / 'file2.py')

        result = collect_files(
            repos=[self.root],
            pathspecs=[],
            base=None,
            tool_runner=runner,
        )

        self.assertEqual(len(result.paths), 2)
        self.assertIn(self.root / 'file1.txt', result.paths)
        self.assertIn(self.root / 'file2.py', result.paths)
        self.assertEqual(result.paths, result.modified_paths)

    @mock.patch('pw_cli.git_repo.describe_git_pattern')
    def test_collect_modified_files(
        self, mock_describe: mock.MagicMock
    ) -> None:
        mock_describe.return_value = 'mocked description'

        runner = FakeGitToolRunner(
            {
                'rev-parse': git_ok('rev-parse', '/repo'),
                'ls-files': git_ok('ls-files', 'file1.txt\nfile2.py'),
                'diff': git_ok('diff', 'file2.py'),
            }
        )

        self.fs.create_file(self.root / 'file1.txt')
        self.fs.create_file(self.root / 'file2.py')

        result = collect_files(
            repos=[self.root],
            pathspecs=[],
            base='HEAD~1',
            tool_runner=runner,
        )

        self.assertEqual(len(result.paths), 2)
        self.assertEqual(len(result.modified_paths), 1)
        self.assertIn(self.root / 'file2.py', result.modified_paths)

    @mock.patch('pw_cli.git_repo.describe_git_pattern')
    def test_collect_with_filter(self, mock_describe: mock.MagicMock) -> None:
        mock_describe.return_value = 'mocked description'

        runner = FakeGitToolRunner(
            {
                'rev-parse': git_ok('rev-parse', '/repo'),
                'ls-files': git_ok('ls-files', 'file1.txt\nfile2.py'),
            }
        )

        self.fs.create_file(self.root / 'file1.txt')
        self.fs.create_file(self.root / 'file2.py')

        filt = FileFilter(exclude=('.py',))

        result = collect_files(
            repos=[self.root],
            pathspecs=[],
            base=None,
            file_filter=filt,
            tool_runner=runner,
        )

        self.assertEqual(len(result.paths), 1)
        self.assertIn(self.root / 'file1.txt', result.paths)
        self.assertNotIn(self.root / 'file2.py', result.paths)


if __name__ == '__main__':
    unittest.main()
