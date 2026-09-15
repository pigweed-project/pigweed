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
"""Tests for RollGenerator stage."""

# pylint: disable=protected-access

import asyncio
from datetime import date
import json
from pathlib import Path
import subprocess
import unittest
from unittest.mock import AsyncMock, MagicMock, patch

from pyfakefs.fake_filesystem_unittest import TestCaseMixin
from pw_fortifier.async_path import AsyncPath
from pw_fortifier.code_snippet import CodeSnippet
from pw_fortifier.freshness_result import FreshnessResult, PackageVersion
from pw_fortifier.git_utils import WritableGitWorkspace
from pw_fortifier.pipeline_stage import PipelineSink
from pw_fortifier.package_updater import (
    PackageUpdater,
    PackageUpdaterStub,
)
from pw_fortifier.roll_generator import (
    RollGenerator,
    RollGeneratorStub,
)
from pw_fortifier.scanner import configure_stage_for_test


class TestRollGenerator(unittest.IsolatedAsyncioTestCase, TestCaseMixin):
    """Unit tests for RollGenerator functionality."""

    def setUp(self) -> None:
        self.setUpPyfakefs()
        self.working_dir = AsyncPath('/working')
        self.dst_dir = Path('/dst')
        self.fs.create_dir(self.working_dir.path)
        self.fs.create_dir(self.dst_dir)

        # Mock WritableGitWorkspace and GitBranch
        self.mock_branch = AsyncMock()
        self.mock_branch.setup = AsyncMock(return_value=self.mock_branch)
        self.mock_branch.teardown = AsyncMock()
        self.mock_branch.reset = AsyncMock()
        self.mock_branch.add = AsyncMock()
        self.mock_branch.commit = AsyncMock()
        self.mock_branch.commit_msg.return_value = [
            'roll: Update 3p deps',
            '',
            'pkg1 -> 1.0.0',
            '',
            'Bug: 111',
        ]
        self.mock_branch.push.return_value = 123456

        self.mock_dst_repo = MagicMock(spec=WritableGitWorkspace)
        self.mock_dst_repo.project_dir = self.dst_dir
        self.mock_dst_repo.run_git = AsyncMock()
        self.mock_dst_repo.branch.return_value = self.mock_branch

    async def test_process_roll_success(self) -> None:
        """Tests roll processing on branch and updates commit message."""
        generator = RollGeneratorStub(generate_return_value=True)
        next_stage = PipelineSink()
        generator.connect(next_stage)

        await configure_stage_for_test(
            generator,
            working_dir=str(self.working_dir),
            dst_repo=self.mock_dst_repo,
            allow_edits=True,
            allow_uploads=True,
        )

        res1 = FreshnessResult(
            package='pkg1',
            location=CodeSnippet(file='MODULE.bazel'),
            pkg_type='bazel_dep',
            current=PackageVersion('0.9.0', date(2026, 1, 1)),
            earliest=PackageVersion('1.0.0', date(2026, 2, 1)),
            tier=1,
            issue_id=111,
        )

        res2 = FreshnessResult(
            package='pkg2',
            location=CodeSnippet(file='Cargo.toml'),
            pkg_type='cargo',
            current=PackageVersion('2.0.0', date(2026, 1, 1)),
            earliest=PackageVersion('2.1.0', date(2026, 2, 1)),
            tier=2,
            issue_id=222,
        )

        path1 = AsyncPath(self.working_dir, 'res1.json')
        path2 = AsyncPath(self.working_dir, 'res2.json')
        await res1.save(path1)
        await res2.save(path2)

        run_task = asyncio.create_task(generator.run())
        await generator.input_queue.put(path1)
        await generator.input_queue.put(path2)
        await generator.input_queue.put(None)
        await run_task

        # Verify stub received both results
        self.assertEqual(len(generator.passed_results), 2)
        self.assertEqual(generator.passed_results[0].package, 'pkg1')
        self.assertEqual(generator.passed_results[1].package, 'pkg2')

        # Verify forwarded results updated with CL number
        fwd1 = await next_stage.input_queue.get()
        fwd2 = await next_stage.input_queue.get()
        assert fwd1 is not None and fwd2 is not None

        out_res1 = await FreshnessResult.load(fwd1)
        out_res2 = await FreshnessResult.load(fwd2)
        self.assertEqual(out_res1.cl_num, 123456)
        self.assertEqual(out_res2.cl_num, 123456)

    async def test_process_roll_no_generate(self) -> None:
        """Tests processing when roll generation returns False."""
        generator = RollGeneratorStub(generate_return_value=False)
        next_stage = PipelineSink()
        generator.connect(next_stage)

        await configure_stage_for_test(
            generator,
            working_dir=str(self.working_dir),
            dst_repo=self.mock_dst_repo,
            allow_edits=True,
            allow_uploads=True,
        )

        res = FreshnessResult(
            package='pkg1',
            location=CodeSnippet(file='MODULE.bazel'),
            pkg_type='bazel_dep',
            current=PackageVersion('0.9.0', date(2026, 1, 1)),
            earliest=PackageVersion('1.0.0', date(2026, 2, 1)),
            tier=1,
            issue_id=111,
        )

        path = AsyncPath(self.working_dir, 'res.json')
        await res.save(path)

        run_task = asyncio.create_task(generator.run())
        await generator.input_queue.put(path)
        await generator.input_queue.put(None)
        await run_task

        self.mock_branch.commit.assert_not_called()
        self.mock_branch.push.assert_not_called()

        fwd = await next_stage.input_queue.get()
        assert fwd is not None
        out_res = await FreshnessResult.load(fwd)
        self.assertIsNone(out_res.cl_num)

    async def test_process_roll_dry_run(self) -> None:
        """Tests allow_edits without uploads does not push commit."""
        generator = RollGeneratorStub(generate_return_value=True)
        next_stage = PipelineSink()
        generator.connect(next_stage)

        await configure_stage_for_test(
            generator,
            working_dir=str(self.working_dir),
            dst_repo=self.mock_dst_repo,
            allow_edits=True,
            allow_uploads=False,
        )

        res = FreshnessResult(
            package='pkg1',
            location=CodeSnippet(file='MODULE.bazel'),
            pkg_type='bazel_dep',
            current=PackageVersion('0.9.0', date(2026, 1, 1)),
            earliest=PackageVersion('1.0.0', date(2026, 2, 1)),
            tier=1,
            issue_id=111,
        )

        path = AsyncPath(self.working_dir, 'res.json')
        await res.save(path)

        run_task = asyncio.create_task(generator.run())
        await generator.input_queue.put(path)
        await generator.input_queue.put(None)
        await run_task

        # Verify branch was created with keep=True and not pushed
        self.mock_dst_repo.branch.assert_called_once_with(
            name='b111', keep=True
        )
        self.mock_branch.add.assert_called_once()
        self.mock_branch.commit.assert_called_once()
        self.mock_branch.push.assert_not_called()

        fwd = await next_stage.input_queue.get()
        assert fwd is not None
        out_res = await FreshnessResult.load(fwd)
        self.assertIsNone(out_res.cl_num)

    async def test_process_roll_failure_resets_branch(self) -> None:
        """Tests that a failed roll after a branch exists calls branch.reset."""
        generator = RollGeneratorStub()
        next_stage = PipelineSink()
        generator.connect(next_stage)

        await configure_stage_for_test(
            generator,
            working_dir=str(self.working_dir),
            dst_repo=self.mock_dst_repo,
            allow_edits=True,
            allow_uploads=True,
        )
        generator.dst_repo = self.mock_dst_repo

        res1 = FreshnessResult(
            package='pkg1',
            location=CodeSnippet(file='MODULE.bazel'),
            pkg_type='bazel_dep',
            current=PackageVersion('0.9.0', date(2026, 1, 1)),
            earliest=PackageVersion('1.0.0', date(2026, 2, 1)),
            tier=1,
            issue_id=111,
        )
        res2 = FreshnessResult(
            package='pkg2',
            location=CodeSnippet(file='Cargo.toml'),
            pkg_type='cargo',
            current=PackageVersion('2.0.0', date(2026, 1, 1)),
            earliest=PackageVersion('2.1.0', date(2026, 2, 1)),
            tier=2,
            issue_id=222,
        )

        path1 = AsyncPath(self.working_dir, 'res1.json')
        path2 = AsyncPath(self.working_dir, 'res2.json')
        await res1.save(path1)
        await res2.save(path2)

        # First roll succeeds, second roll fails
        generator.generate_return_value = True

        async def _mock_generate(result: FreshnessResult) -> bool:
            if result.package == 'pkg2':
                return False
            return True

        generator._generate_roll = _mock_generate  # type: ignore[method-assign]

        run_task = asyncio.create_task(generator.run())
        await generator.input_queue.put(path1)
        await generator.input_queue.put(path2)
        await generator.input_queue.put(None)
        await run_task

        # Verify branch.reset(staged=False) was called for the failed roll
        self.mock_branch.reset.assert_called_once_with(staged=False)

    @patch('asyncio.create_subprocess_exec')
    async def test_generate_roll_success(
        self,
        mock_exec: AsyncMock,
    ) -> None:
        """Tests _generate_roll replaces version and returns True."""
        mock_proc = AsyncMock()
        mock_proc.communicate.return_value = (b'', b'')
        mock_proc.returncode = 0
        mock_exec.return_value = mock_proc

        source_file = self.dst_dir / 'MODULE.bazel'
        source_file.write_text('bazel_dep(name = "pkg1", version = "0.9.0")\n')

        generator = RollGenerator()
        generator.dst_repo = self.mock_dst_repo

        res = FreshnessResult(
            package='pkg1',
            location=CodeSnippet(file='MODULE.bazel'),
            pkg_type='bazel_dep',
            current=PackageVersion('0.9.0', date(2026, 1, 1)),
            earliest=PackageVersion('1.0.0', date(2026, 2, 1)),
            tier=1,
        )

        result = await generator._generate_roll(res)
        self.assertTrue(result)
        mock_exec.assert_called_once_with(
            'pw',
            'presubmit',
            cwd=str(self.dst_dir),
            env=generator._env or unittest.mock.ANY,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        self.assertEqual(
            source_file.read_text(),
            'bazel_dep(name = "pkg1", version = "1.0.0")\n',
        )

    @patch('asyncio.create_subprocess_exec')
    async def test_generate_roll_with_pw_script(
        self,
        mock_exec: AsyncMock,
    ) -> None:
        """Tests _generate_roll uses ./pw script if present."""
        pw_script = self.dst_dir / 'pw'
        pw_script.touch(mode=0o755)

        mock_proc = AsyncMock()
        mock_proc.communicate.return_value = (b'', b'')
        mock_proc.returncode = 0
        mock_exec.return_value = mock_proc

        source_file = self.dst_dir / 'MODULE.bazel'
        source_file.write_text('bazel_dep(name = "pkg1", version = "0.9.0")\n')

        generator = RollGenerator()
        generator.dst_repo = self.mock_dst_repo

        res = FreshnessResult(
            package='pkg1',
            location=CodeSnippet(file='MODULE.bazel'),
            pkg_type='bazel_dep',
            current=PackageVersion('0.9.0', date(2026, 1, 1)),
            earliest=PackageVersion('1.0.0', date(2026, 2, 1)),
            tier=1,
        )

        result = await generator._generate_roll(res)
        self.assertTrue(result)
        mock_exec.assert_called_once_with(
            str(pw_script),
            'presubmit',
            cwd=str(self.dst_dir),
            env=generator._env or unittest.mock.ANY,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )

    @patch('asyncio.create_subprocess_exec')
    async def test_generate_roll_presubmit_failure(
        self,
        mock_exec: AsyncMock,
    ) -> None:
        """Tests _generate_roll returns False when presubmit fails."""
        mock_proc = AsyncMock()
        mock_proc.communicate.return_value = (b'', b'')
        mock_proc.returncode = 1
        mock_exec.return_value = mock_proc

        source_file = self.dst_dir / 'Cargo.toml'
        source_file.write_text('pkg2 = "2.0.0"\n')

        generator = RollGenerator()
        generator.dst_repo = self.mock_dst_repo

        res = FreshnessResult(
            package='pkg2',
            location=CodeSnippet(file='Cargo.toml'),
            pkg_type='cargo',
            current=PackageVersion('2.0.0', date(2026, 1, 1)),
            earliest=PackageVersion('2.1.0', date(2026, 2, 1)),
            tier=2,
        )

        result = await generator._generate_roll(res)
        self.assertFalse(result)
        mock_exec.assert_called_once()
        self.mock_dst_repo.run_git.assert_not_called()

    @patch('asyncio.create_subprocess_exec')
    async def test_generate_roll_presubmit_not_found_logs_and_raises(
        self,
        mock_exec: AsyncMock,
    ) -> None:
        """Tests _generate_roll logs and raises when presubmit not found."""
        mock_exec.side_effect = FileNotFoundError('pw not found')

        source_file = self.dst_dir / 'Cargo.toml'
        source_file.write_text('pkg2 = "2.0.0"\n')

        generator = RollGenerator()
        generator.dst_repo = self.mock_dst_repo

        res = FreshnessResult(
            package='pkg2',
            location=CodeSnippet(file='Cargo.toml'),
            pkg_type='cargo',
            current=PackageVersion('2.0.0', date(2026, 1, 1)),
            earliest=PackageVersion('2.1.0', date(2026, 2, 1)),
            tier=2,
        )

        with self.assertLogs(level='ERROR') as cm:
            with self.assertRaises(FileNotFoundError):
                await generator._generate_roll(res)

        self.assertTrue(
            any('No presubmit checks could be run' in msg for msg in cm.output)
        )

    async def test_generate_roll_missing_file_or_version(self) -> None:
        """Tests _generate_roll when file or version is missing."""
        generator = RollGenerator()
        generator.dst_repo = self.mock_dst_repo

        # Missing file
        res = FreshnessResult(
            package='pkg1',
            location=CodeSnippet(file='nonexistent.json'),
            pkg_type='npm',
            current=PackageVersion('1.0.0', date(2026, 1, 1)),
            earliest=PackageVersion('2.0.0', date(2026, 2, 1)),
            tier=1,
        )
        self.assertFalse(await generator._generate_roll(res))

        # File exists but current version not found
        source_file = self.dst_dir / 'package.json'
        source_file.write_text('{"version": "3.0.0"}')
        res_updated = res._replace(location=CodeSnippet(file='package.json'))
        self.assertFalse(await generator._generate_roll(res_updated))

    @patch('asyncio.create_subprocess_shell')
    async def test_set_up(
        self,
        mock_shell: AsyncMock,
    ) -> None:
        """Tests _set_up runs bootstrap and captures environment."""
        mock_proc = AsyncMock()
        mock_proc.communicate.return_value = (
            json.dumps({'PW_ROOT': '/foo/bar'}).encode(),
            b'',
        )
        mock_proc.returncode = 0
        mock_shell.return_value = mock_proc

        generator = RollGenerator()
        generator.dst_repo = self.mock_dst_repo

        await generator._set_up()
        self.assertEqual(generator._env, {'PW_ROOT': '/foo/bar'})

    @patch('asyncio.create_subprocess_shell')
    async def test_set_up_failure_raises(
        self,
        mock_shell: AsyncMock,
    ) -> None:
        """Tests _set_up raises CalledProcessError when bootstrap fails."""
        mock_proc = AsyncMock()
        mock_proc.communicate.return_value = (b'', b'error')
        mock_proc.returncode = 1
        mock_shell.return_value = mock_proc

        generator = RollGenerator()
        generator.dst_repo = self.mock_dst_repo

        with self.assertRaises(subprocess.CalledProcessError):
            await generator._set_up()

    async def test_register_and_dispatch_updater_success(self) -> None:
        """Tests registered PackageUpdater handles matching pkg_type."""

        class CustomUpdater(PackageUpdaterStub):
            PKG_TYPE = 'custom_type'

        updater = CustomUpdater(update_return_value=True)
        generator = RollGenerator()
        generator.dst_repo = self.mock_dst_repo
        generator.register(updater)

        res = FreshnessResult(
            package='custom_pkg',
            location=CodeSnippet(file='custom.lock'),
            pkg_type='custom_type',
            current=PackageVersion('1.0.0', date(2026, 1, 1)),
            earliest=PackageVersion('1.1.0', date(2026, 2, 1)),
            tier=1,
        )

        result = await generator._generate_roll(res)
        self.assertTrue(result)
        self.assertEqual(len(updater.updated_results), 1)
        self.assertEqual(
            updater.updated_results[0],
            (self.mock_dst_repo, res),
        )

    async def test_register_and_dispatch_updater_failure(self) -> None:
        """Tests registered PackageUpdater failure returns False."""

        class CustomUpdater(PackageUpdaterStub):
            PKG_TYPE = 'custom_type'

        updater = CustomUpdater(update_return_value=False)
        generator = RollGenerator()
        generator.dst_repo = self.mock_dst_repo
        generator.register(updater)

        res = FreshnessResult(
            package='custom_pkg',
            location=CodeSnippet(file='custom.lock'),
            pkg_type='custom_type',
            current=PackageVersion('1.0.0', date(2026, 1, 1)),
            earliest=PackageVersion('1.1.0', date(2026, 2, 1)),
            tier=1,
        )

        result = await generator._generate_roll(res)
        self.assertFalse(result)
        self.assertEqual(len(updater.updated_results), 1)
        self.assertEqual(
            updater.updated_results[0],
            (self.mock_dst_repo, res),
        )

    async def test_register_and_dispatch_target_updater_success(self) -> None:
        """Tests registered PackageUpdater handles matching target filename."""

        class TargetUpdater(PackageUpdaterStub):
            PKG_TYPE = None
            TARGET = 'custom_target.json'

        updater = TargetUpdater(update_return_value=True)
        generator = RollGenerator()
        generator.dst_repo = self.mock_dst_repo
        generator.register(updater)

        res = FreshnessResult(
            package='custom_pkg',
            location=CodeSnippet(file='custom_target.json'),
            pkg_type='unregistered_type',
            current=PackageVersion('1.0.0', date(2026, 1, 1)),
            earliest=PackageVersion('1.1.0', date(2026, 2, 1)),
            tier=1,
        )

        result = await generator._generate_roll(res)
        self.assertTrue(result)
        self.assertEqual(len(updater.updated_results), 1)
        self.assertEqual(
            updater.updated_results[0],
            (self.mock_dst_repo, res),
        )

    def test_register_asserts_on_both_none(self) -> None:
        """Tests register raises AssertionError when both fields are None."""

        class EmptyUpdater(PackageUpdaterStub):
            PKG_TYPE = None
            TARGET = None

        updater = EmptyUpdater()
        generator = RollGenerator()
        with self.assertRaises(AssertionError):
            generator.register(updater)

    async def test_register_both_pkg_type_and_target(self) -> None:
        """Tests register adds updater to both registries when both are set."""

        class BothUpdater(PackageUpdaterStub):
            PKG_TYPE = 'both_type'
            TARGET = 'both_target.json'

        updater = BothUpdater(update_return_value=True)
        generator = RollGenerator()
        generator.dst_repo = self.mock_dst_repo
        generator.register(updater)

        self.assertIn('both_type', generator._pkg_registry)
        self.assertIn('both_target.json', generator._target_registry)

        # Dispatch via pkg_type
        res_by_type = FreshnessResult(
            package='pkg_a',
            location=CodeSnippet(file='other.json'),
            pkg_type='both_type',
            current=PackageVersion('1.0.0', date(2026, 1, 1)),
            earliest=PackageVersion('1.1.0', date(2026, 2, 1)),
            tier=1,
        )
        self.assertTrue(await generator._generate_roll(res_by_type))

        # Dispatch via target source
        res_by_target = FreshnessResult(
            package='pkg_b',
            location=CodeSnippet(file='both_target.json'),
            pkg_type='other_type',
            current=PackageVersion('1.0.0', date(2026, 1, 1)),
            earliest=PackageVersion('1.1.0', date(2026, 2, 1)),
            tier=1,
        )
        self.assertTrue(await generator._generate_roll(res_by_target))
        self.assertEqual(len(updater.updated_results), 2)

    def test_package_updater_abstract(self) -> None:
        """Tests PackageUpdater cannot be instantiated without update."""

        class IncompleteUpdater(PackageUpdater):
            pass

        with self.assertRaises(TypeError):
            # pylint: disable=abstract-class-instantiated
            IncompleteUpdater()  # type: ignore[abstract]


if __name__ == '__main__':
    unittest.main()
