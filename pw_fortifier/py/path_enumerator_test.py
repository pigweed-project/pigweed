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
"""Tests for the PathEnumerator class in pw_fortifier."""

from pathlib import Path
import unittest

from pyfakefs.fake_filesystem_unittest import TestCase, TestCaseMixin
from pw_fortifier.async_path import AsyncPath
from pw_fortifier.emitter import Emitter
from pw_fortifier.git_utils import ReadOnlyGitWorkspace
from pw_fortifier.path_enumerator import FakePathEnumerator, PathEnumerator
from pw_fortifier.pipeline_stage import PipelineSink
from pw_fortifier.scanner import configure_stage_for_test


class TestPathEnumerator(TestCase):
    """Unit tests for PathEnumerator verifying file/dir enumeration."""

    def setUp(self) -> None:
        self.setUpPyfakefs()
        self.root = Path('/repo').resolve()
        self.src_repo = ReadOnlyGitWorkspace(self.root)

        # Create a mock directory structure:
        # root/
        #   foo.cc
        #   bar.h
        #   pw_sync/
        #     sync.cc
        #     sync.h
        #     private/
        #       secret.cc
        #   pw_thread/
        #     thread.cc
        #   build/
        #     output.o
        self.fs.create_file(self.root / 'foo.cc')
        self.fs.create_file(self.root / 'bar.h')
        self.fs.create_file(self.root / 'pw_sync/sync.cc')
        self.fs.create_file(self.root / 'pw_sync/sync.h')
        self.fs.create_file(self.root / 'pw_sync/private/secret.cc')
        self.fs.create_file(self.root / 'pw_thread/thread.cc')
        self.fs.create_file(self.root / 'build/output.o')
        self.fs.create_file(self.root / '.git/HEAD')
        self.fs.create_file(self.root / '.git/config')

    def test_default_enumerates_files_only(self) -> None:
        """Tests that by default only files are enumerated, if matched."""
        enumerator = PathEnumerator(self.src_repo)
        enumerator.match('*')
        paths = list(enumerator)

        # Should include all files, but no directories
        expected = {
            Path('foo.cc'),
            Path('bar.h'),
            Path('pw_sync/sync.cc'),
            Path('pw_sync/sync.h'),
            Path('pw_sync/private/secret.cc'),
            Path('pw_thread/thread.cc'),
            Path('build/output.o'),
        }
        self.assertEqual(set(paths), expected)

    def test_include_dirs(self) -> None:
        """Tests enumerating directories when include_dirs is True."""
        enumerator = PathEnumerator(
            self.src_repo, include_files=False, include_dirs=True
        )
        enumerator.match('*')
        paths = list(enumerator)

        expected = {
            Path('pw_sync'),
            Path('pw_sync/private'),
            Path('pw_thread'),
            Path('build'),
        }
        self.assertEqual(set(paths), expected)

    def test_match_patterns(self) -> None:
        """Tests that only paths matching the patterns are yielded."""
        enumerator = PathEnumerator(self.src_repo)
        enumerator.match('*.cc')
        paths = list(enumerator)

        expected = {
            Path('foo.cc'),
            Path('pw_sync/sync.cc'),
            Path('pw_sync/private/secret.cc'),
            Path('pw_thread/thread.cc'),
        }
        self.assertEqual(set(paths), expected)

    def test_multiple_match_patterns(self) -> None:
        """Tests that paths matching any of the match patterns are yielded."""
        enumerator = PathEnumerator(self.src_repo)
        enumerator.match('*.cc')
        enumerator.match('*.h')
        paths = list(enumerator)

        expected = {
            Path('foo.cc'),
            Path('bar.h'),
            Path('pw_sync/sync.cc'),
            Path('pw_sync/sync.h'),
            Path('pw_sync/private/secret.cc'),
            Path('pw_thread/thread.cc'),
        }
        self.assertEqual(set(paths), expected)

    def test_git_directory_excluded(self) -> None:
        """Tests that the .git directory and its contents are excluded."""
        enumerator = PathEnumerator(self.src_repo)
        paths = list(enumerator)
        for path in paths:
            self.assertNotIn('.git', path.parts)

    def test_no_match_patterns_yields_everything(self) -> None:
        """Tests all non-ignored paths are yielded if no patterns specified."""
        enumerator = PathEnumerator(self.src_repo)
        paths = list(enumerator)

        expected = {
            Path('foo.cc'),
            Path('bar.h'),
            Path('pw_sync/sync.cc'),
            Path('pw_sync/sync.h'),
            Path('pw_sync/private/secret.cc'),
            Path('pw_thread/thread.cc'),
            Path('build/output.o'),
        }
        self.assertEqual(set(paths), expected)

    def test_gitignore_exclusion(self) -> None:
        """Tests that files and directories in .gitignore are excluded."""
        gitignore_path = self.root / '.gitignore'
        gitignore_path.write_text('build/\n*.o\npw_sync/private/\n')

        enumerator = PathEnumerator(self.src_repo)
        paths = list(enumerator)

        expected = {
            Path('.gitignore'),
            Path('foo.cc'),
            Path('bar.h'),
            Path('pw_sync/sync.cc'),
            Path('pw_sync/sync.h'),
            Path('pw_thread/thread.cc'),
        }
        self.assertEqual(set(paths), expected)

    def test_gitignore_negation(self) -> None:
        """Tests that gitignore negation rules are respected."""
        gitignore_path = self.root / '.gitignore'
        gitignore_path.write_text('pw_sync/*\n!pw_sync/sync.cc\n')

        enumerator = PathEnumerator(self.src_repo)
        paths = list(enumerator)

        expected = {
            Path('.gitignore'),
            Path('foo.cc'),
            Path('bar.h'),
            Path('pw_sync/sync.cc'),
            Path('pw_thread/thread.cc'),
            Path('build/output.o'),
        }
        self.assertEqual(set(paths), expected)


class TestEmitter(unittest.IsolatedAsyncioTestCase, TestCaseMixin):
    """Unit tests for the Emitter stage."""

    def setUp(self) -> None:
        self.setUpPyfakefs()
        self.working_dir = AsyncPath('/working').resolve()
        self.fs.create_dir(self.working_dir.path)

    async def test_emitter_fresh_run(self) -> None:
        """Tests that Emitter emits all files on a fresh run."""
        root_path = Path(self.working_dir.path) / 'repo'
        self.fs.create_file(root_path / 'file1.cc')
        self.fs.create_file(root_path / 'file2.cc')

        # Use FakePathEnumerator with real root to keep order deterministic
        enumerator = FakePathEnumerator(
            [Path('file1.cc'), Path('file2.cc')],
            src_repo=ReadOnlyGitWorkspace(root_path),
        )
        emitter = Emitter(enumerator)

        await configure_stage_for_test(
            emitter,  # type: ignore[arg-type]
            working_dir=str(self.working_dir),
        )

        next_stage = PipelineSink()
        emitter.connect(next_stage)

        await emitter.run()

        self.assertEqual(next_stage.input_queue.qsize(), 3)
        path1 = await next_stage.input_queue.get()
        path2 = await next_stage.input_queue.get()
        sentinel = await next_stage.input_queue.get()
        self.assertIsNone(sentinel)

        assert path1 is not None
        assert path2 is not None
        self.assertEqual(path1, AsyncPath(root_path / 'file1.cc'))
        self.assertEqual(path2, AsyncPath(root_path / 'file2.cc'))

        # Verify last_emitted.txt was cleaned up
        self.assertFalse(await (self.working_dir / 'last_emitted.txt').exists())

    async def test_emitter_resume_run(self) -> None:
        """Tests that Emitter resumes emitting from the last index."""
        root_path = Path(self.working_dir.path) / 'repo'
        self.fs.create_file(root_path / 'file1.cc')
        self.fs.create_file(root_path / 'file2.cc')
        self.fs.create_file(root_path / 'file3.cc')

        enumerator = FakePathEnumerator(
            [Path('file1.cc'), Path('file2.cc'), Path('file3.cc')],
            src_repo=ReadOnlyGitWorkspace(root_path),
        )
        emitter = Emitter(enumerator)

        # Simulate last emitted file
        last_emitted_file = self.working_dir / 'last_emitted.txt'
        await last_emitted_file.write_text(str(root_path / 'file1.cc'))

        await configure_stage_for_test(
            emitter,  # type: ignore[arg-type]
            working_dir=str(self.working_dir),
            resume=True,
        )

        next_stage = PipelineSink()
        emitter.connect(next_stage)

        await emitter.run()

        self.assertEqual(next_stage.input_queue.qsize(), 3)
        path1 = await next_stage.input_queue.get()
        path2 = await next_stage.input_queue.get()
        sentinel = await next_stage.input_queue.get()
        self.assertIsNone(sentinel)

        assert path1 is not None
        assert path2 is not None
        self.assertEqual(path1, AsyncPath(root_path / 'file2.cc'))
        self.assertEqual(path2, AsyncPath(root_path / 'file3.cc'))

    async def test_emitter_files_within_repo(self) -> None:
        """Tests that Emitter emits files using relative and absolute paths."""
        root_path = Path(self.working_dir.path) / 'repo'
        self.fs.create_file(root_path / 'foo' / 'file1.cc')
        self.fs.create_file(root_path / 'bar' / 'file2.cc')

        enumerator = FakePathEnumerator(
            [], src_repo=ReadOnlyGitWorkspace(root_path)
        )
        emitter = Emitter(enumerator)

        await configure_stage_for_test(
            emitter,  # type: ignore[arg-type]
            working_dir=str(self.working_dir),
            files=['foo/file1.cc', str(root_path / 'bar' / 'file2.cc')],
        )

        next_stage = PipelineSink()
        emitter.connect(next_stage)

        await emitter.run()

        self.assertEqual(next_stage.input_queue.qsize(), 3)
        path1 = await next_stage.input_queue.get()
        path2 = await next_stage.input_queue.get()
        sentinel = await next_stage.input_queue.get()
        self.assertIsNone(sentinel)

        self.assertEqual(path1, AsyncPath(root_path / 'foo' / 'file1.cc'))
        self.assertEqual(path2, AsyncPath(root_path / 'bar' / 'file2.cc'))

    async def test_emitter_files_outside_repo_raises(self) -> None:
        """Tests specifying a file outside repository raises ValueError."""
        root_path = Path(self.working_dir.path) / 'repo'
        outside_path = Path(self.working_dir.path) / 'outside' / 'external.cc'
        self.fs.create_file(root_path / 'file1.cc')
        self.fs.create_file(outside_path)

        enumerator = FakePathEnumerator(
            [], src_repo=ReadOnlyGitWorkspace(root_path)
        )
        emitter = Emitter(enumerator)

        await configure_stage_for_test(
            emitter,  # type: ignore[arg-type]
            working_dir=str(self.working_dir),
            files=[str(outside_path)],
        )

        next_stage = PipelineSink()
        emitter.connect(next_stage)

        with self.assertRaises(ValueError):
            await emitter.run()


if __name__ == '__main__':
    unittest.main()
