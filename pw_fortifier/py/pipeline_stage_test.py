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
"""Tests for PipelineStage in pw_fortifier."""

# pylint: disable=protected-access

import asyncio
import os
from pathlib import Path
import subprocess
import unittest

from pyfakefs.fake_filesystem_unittest import TestCaseMixin
from pw_fortifier.async_path import AsyncPath
from pw_fortifier.pipeline_stage import (
    PipelineConsumerStage,
    PipelineDemux,
    PipelineMux,
    PipelineProducerStage,
    PipelineSink,
    PipelineStageStub,
)
from pw_fortifier.scanner import configure_stage_for_test


class TestPipelineStage(unittest.IsolatedAsyncioTestCase, TestCaseMixin):
    """IsolatedAsyncioTestCase to test async methods of PipelineStage."""

    def setUp(self) -> None:
        self.setUpPyfakefs()
        self.working_dir = AsyncPath('/working')
        self.fs.create_dir(self.working_dir.path)

    async def test_configure_creates_dirs(self) -> None:
        """Tests that configure creates the in, out, and err directories."""
        stage = PipelineStageStub()
        await configure_stage_for_test(stage, working_dir=str(self.working_dir))

        self.assertTrue(
            await AsyncPath(self.working_dir, 'pipeline_stage_stub_in').is_dir()
        )
        self.assertTrue(
            await AsyncPath(
                self.working_dir, 'pipeline_stage_stub_out'
            ).is_dir()
        )
        self.assertTrue(
            await AsyncPath(
                self.working_dir, 'pipeline_stage_stub_err'
            ).is_dir()
        )

    async def test_configure_creates_in_dir_files(self) -> None:
        """Tests configure creates _in_dir and existing files are present."""
        stage = PipelineStageStub()
        await configure_stage_for_test(stage, working_dir=str(self.working_dir))

        in_dir = AsyncPath(self.working_dir, 'pipeline_stage_stub_in')
        file1 = AsyncPath(in_dir, 'file1.txt')
        await file1.write_text('content1')

        self.assertTrue(await file1.is_file())
        self.assertEqual(await file1.read_text(), 'content1')

    async def test_connect_queues(self) -> None:
        """Tests connect links output queue of one stage to input of next."""
        stage1 = PipelineStageStub()
        stage2 = PipelineStageStub()
        stage1.connect(stage2)

        self.assertIs(stage1._output_queue, stage2.input_queue)

    async def test_run_processes_initial_files_and_sentinel(self) -> None:
        """Tests processing in _in_dir, stopping when sentinel received."""
        stage = PipelineStageStub(should_forward=True)
        consumer = PipelineSink()
        stage.connect(consumer)
        await configure_stage_for_test(stage, working_dir=str(self.working_dir))

        src_file = AsyncPath(
            self.working_dir, 'pipeline_stage_stub_in/initial.txt'
        )
        await src_file.write_text('initial content')

        # Start stage execution in background
        run_task = asyncio.create_task(stage.run())

        # Send sentinel to stop after initial files are processed
        await stage.input_queue.put(None)
        await run_task

        self.assertEqual(len(stage.processed_paths), 1)
        self.assertEqual(stage.processed_paths[0].name, 'initial.txt')
        # File should be unlinked from in_dir and moved to out_dir (since it
        # forwarded)
        self.assertFalse(
            await AsyncPath(
                self.working_dir, 'pipeline_stage_stub_in/initial.txt'
            ).exists()
        )
        self.assertTrue(
            await AsyncPath(
                self.working_dir, 'pipeline_stage_stub_out/initial.txt'
            ).exists()
        )

        self.assertTrue(stage.setup_called)
        self.assertTrue(stage.teardown_called)

    async def test_run_processes_queue_inputs(self) -> None:
        """Tests processing files received dynamically via the input queue."""
        stage = PipelineStageStub(should_forward=False)  # Drop files
        await configure_stage_for_test(stage, working_dir=str(self.working_dir))

        run_task = asyncio.create_task(stage.run())

        # Put a file path into the queue
        dyn_file = AsyncPath(self.working_dir, 'dynamic.txt')
        await dyn_file.write_text('dynamic content')

        await stage.input_queue.put(dyn_file)
        await stage.input_queue.put(None)
        await run_task

        self.assertEqual(len(stage.processed_paths), 1)
        self.assertEqual(stage.processed_paths[0].name, 'dynamic.txt')
        # File was dropped, so it should be deleted (unlinked) from stage_in
        self.assertFalse(
            await AsyncPath(
                self.working_dir, 'pipeline_stage_stub_in/dynamic.txt'
            ).exists()
        )
        # And since should_forward=False, it should NOT be in stage_out
        self.assertFalse(
            await AsyncPath(
                self.working_dir, 'pipeline_stage_stub_out/dynamic.txt'
            ).exists()
        )

    async def test_run_retry_and_move_to_error_dir(self) -> None:
        """Tests failing files are retried up to max_retries then moved."""
        stage = PipelineStageStub(should_fail=True)
        await configure_stage_for_test(
            stage,
            working_dir=str(self.working_dir),
            max_retries=2,
        )

        run_task = asyncio.create_task(stage.run())

        fail_file = AsyncPath(self.working_dir, 'fail.txt')
        await fail_file.write_text('fail content')

        await stage.input_queue.put(fail_file)
        await stage.input_queue.put(None)
        await run_task

        # With max_retries=2, it should attempt 3 times (1 initial + 2 retries)
        self.assertEqual(len(stage.processed_paths), 3)
        self.assertTrue(
            all(p.name == 'fail.txt' for p in stage.processed_paths)
        )

        # After exhausting retries, it should be moved to err_dir
        self.assertFalse(
            await AsyncPath(
                self.working_dir, 'pipeline_stage_stub_in/fail.txt'
            ).exists()
        )
        self.assertTrue(
            await AsyncPath(
                self.working_dir, 'pipeline_stage_stub_err/fail.txt'
            ).exists()
        )

    async def test_forward_sentinel_to_next_stage(self) -> None:
        """Tests sentinel (None) is propagated to next connected stage."""
        stage1 = PipelineStageStub(should_forward=True)
        stage2 = PipelineStageStub(should_forward=True)
        stage1.connect(stage2)

        await configure_stage_for_test(
            stage1,
            working_dir=str(self.working_dir),
        )
        await configure_stage_for_test(
            stage2,
            working_dir=str(self.working_dir),
        )

        run_task1 = asyncio.create_task(stage1.run())
        run_task2 = asyncio.create_task(stage2.run())

        # Stop stage1
        await stage1.input_queue.put(None)

        # Wait for both to complete
        await asyncio.gather(run_task1, run_task2)

        # stage2 should have received None from stage1 and terminated
        self.assertTrue(stage1.teardown_called)
        self.assertTrue(stage2.teardown_called)

    async def test_resumption_after_interruption(self) -> None:
        """Tests queued file is moved to _in_dir and can be resumed."""
        processing_started = asyncio.Event()
        resume_processing = asyncio.Event()

        class HangingStage(PipelineStageStub):
            async def _process_one(self, path: AsyncPath) -> None:
                processing_started.set()
                await resume_processing.wait()
                await super()._process_one(path)

        stage = HangingStage(should_forward=True)
        await configure_stage_for_test(stage, working_dir=str(self.working_dir))

        run_task = asyncio.create_task(stage.run())

        # Put a file in the queue
        dyn_file = AsyncPath(self.working_dir, 'dynamic.txt')
        await dyn_file.write_text('dynamic content')
        await stage.input_queue.put(dyn_file)

        # Wait until processing starts
        await processing_started.wait()

        # Verify the file was moved to stage_in
        local_path = AsyncPath(self.working_dir, 'hanging_stage_in/dynamic.txt')
        self.assertTrue(await local_path.is_file())
        self.assertFalse(await dyn_file.exists())

        # Simulate interruption by cancelling the task
        run_task.cancel()
        try:
            await run_task
        except asyncio.CancelledError:
            pass

        # The file should still be in stage_in
        self.assertTrue(await local_path.is_file())

        # Now start a new stage with resume=True
        resumed_stage = HangingStage(should_forward=True)
        consumer = PipelineSink()
        resumed_stage.connect(consumer)
        await configure_stage_for_test(
            resumed_stage,
            working_dir=str(self.working_dir),
            resume=True,
        )

        # It should find the file in hanging_stage_in and process it
        run_task2 = asyncio.create_task(resumed_stage.run())

        # Unblock processing and send sentinel to stop after processing
        # existing files
        resume_processing.set()
        await resumed_stage.input_queue.put(None)
        await run_task2

        self.assertEqual(len(resumed_stage.processed_paths), 1)
        self.assertEqual(resumed_stage.processed_paths[0].name, 'dynamic.txt')
        self.assertFalse(await local_path.exists())
        self.assertTrue(
            await AsyncPath(
                self.working_dir, 'hanging_stage_out/dynamic.txt'
            ).exists()
        )

    async def test_failure_quarantine_missing_file_logs_error(self) -> None:
        """Tests that failure to quarantine a missing file logs an error."""

        class FailingStage(PipelineStageStub):
            async def _process_one(self, path: AsyncPath) -> None:
                await path.unlink()
                raise subprocess.SubprocessError('Command failed')

        stage = FailingStage()
        await configure_stage_for_test(
            stage,
            working_dir=str(self.working_dir),
            max_retries=0,
        )

        fail_file = AsyncPath(self.working_dir, 'failing.txt')
        await fail_file.write_text('content')

        with self.assertLogs(level='ERROR') as cm:
            run_task = asyncio.create_task(stage.run())
            await stage.input_queue.put(fail_file)
            await stage.input_queue.put(None)
            await run_task

        self.assertTrue(
            any('Failed to save failing.txt' in msg for msg in cm.output)
        )

    async def test_skip_setup_defaults_to_false(self) -> None:
        """Tests that skip_setup flag defaults to False."""
        stage = PipelineStageStub()
        self.assertFalse(stage.skip_setup)

    async def test_pipeline_stage_skip_setup(self) -> None:
        """Tests that PipelineStage skips _set_up when skip_setup is True."""
        stage = PipelineStageStub(should_forward=False)
        stage.skip_setup = True
        await configure_stage_for_test(stage, working_dir=str(self.working_dir))
        await stage.input_queue.put(None)
        await stage.run()
        self.assertFalse(stage.setup_called)
        self.assertTrue(stage.teardown_called)

    async def test_pipeline_consumer_stage_skip_setup(self) -> None:
        """Tests that ConsumerStage skips _set_up when skip_setup is True."""
        stage = StubConsumerStage()
        stage.skip_setup = True
        await configure_stage_for_test(stage, working_dir=str(self.working_dir))
        await stage.input_queue.put(None)
        await stage.run()
        self.assertFalse(stage.setup_called)

    async def test_pipeline_producer_stage_skip_setup(self) -> None:
        """Tests that ProducerStage skips _set_up when skip_setup is True."""
        stage = StubProducerStage()
        stage.skip_setup = True
        consumer = PipelineSink()
        stage.connect(consumer)
        await configure_stage_for_test(stage, working_dir=str(self.working_dir))
        await stage.run()
        self.assertFalse(stage.setup_called)
        self.assertTrue(stage.produced)


class TestPipelineMux(unittest.IsolatedAsyncioTestCase, TestCaseMixin):
    """Tests for PipelineMux."""

    def setUp(self) -> None:
        self.setUpPyfakefs()
        self.working_dir = AsyncPath('/working')
        self.fs.create_dir(self.working_dir.path)

    async def test_mux_routing(self) -> None:
        """Tests that PipelineMux routes files to correct stages."""
        mux = PipelineMux()
        await configure_stage_for_test(mux, working_dir=str(self.working_dir))

        dest1 = PipelineSink()
        dest2 = PipelineSink()

        mux.add_stage('file1.txt', dest1)
        mux.add_stage('file2.txt', dest2)

        file1 = AsyncPath(self.working_dir, 'file1.txt')
        file2 = AsyncPath(self.working_dir, 'file2.txt')
        unmapped = AsyncPath(self.working_dir, 'unmapped.txt')
        await file1.write_text('content1')
        await file2.write_text('content2')
        await unmapped.write_text('content3')

        mux_task = asyncio.create_task(mux.run())

        # Send files to mux
        await mux.input_queue.put(file1)
        await mux.input_queue.put(file2)
        await mux.input_queue.put(unmapped)  # Should be ignored
        await mux.input_queue.put(None)  # Sentinel

        await mux_task

        # dest1 should get file1.txt and then None
        res1 = await dest1.input_queue.get()
        self.assertIsNotNone(res1)
        assert res1 is not None
        self.assertEqual(res1.name, 'file1.txt')
        self.assertIsNone(await dest1.input_queue.get())

        # dest2 should get file2.txt and then None
        res2 = await dest2.input_queue.get()
        self.assertIsNotNone(res2)
        assert res2 is not None
        self.assertEqual(res2.name, 'file2.txt')
        self.assertIsNone(await dest2.input_queue.get())

    async def test_mux_strict_matching_raises_on_unmapped(self) -> None:
        """Tests that strict_matching raises ValueError on unmapped files."""
        mux = PipelineMux()
        self.assertFalse(mux.strict_matching)
        mux.strict_matching = True
        self.assertTrue(mux.strict_matching)

        await configure_stage_for_test(mux, working_dir=str(self.working_dir))
        unmapped = AsyncPath(self.working_dir, 'unmapped.txt')
        await unmapped.write_text('content')

        with self.assertRaises(ValueError) as ctx:
            await mux._process_one(unmapped)
        self.assertIn(
            "No target stage registered for 'unmapped.txt'",
            str(ctx.exception),
        )


class TestPipelineDemux(unittest.IsolatedAsyncioTestCase):
    """Tests for PipelineDemux."""

    async def test_demux_merging(self) -> None:
        """Tests merging output from multiple sources in PipelineDemux."""
        demux = PipelineDemux()
        src1 = (
            PipelineStageStub()
        )  # PipelineStageStub inherits from PipelineProducerMixin
        src2 = PipelineStageStub()
        dest = PipelineSink()

        demux.add_stage(src1)
        demux.add_stage(src2)
        demux.connect(dest)

        demux_task = asyncio.create_task(demux.run())

        # Send files from sources
        src1.send(AsyncPath('file1.txt'))
        src2.send(AsyncPath('file2.txt'))

        # Close sources
        await src1.close()
        await src2.close()

        await demux_task

        # dest should get both files (order might vary)
        results = []
        for _ in range(2):
            results.append(await dest.input_queue.get())

        filenames = {r.name for r in results if r is not None}
        self.assertEqual(filenames, {'file1.txt', 'file2.txt'})

        # dest should then get None (sentinel)
        self.assertIsNone(await dest.input_queue.get())


class StubConsumerStage(PipelineConsumerStage):
    """Stub consumer stage for testing skip_setup."""

    def __init__(self) -> None:
        super().__init__()
        self.setup_called = False

    async def _set_up(self) -> None:
        self.setup_called = True

    async def _process_one(self, path: AsyncPath) -> None:
        pass


class StubProducerStage(PipelineProducerStage):
    """Stub producer stage for testing skip_setup."""

    def __init__(self) -> None:
        super().__init__()
        self.setup_called = False
        self.produced = False

    async def _set_up(self) -> None:
        self.setup_called = True

    async def _produce_all(self) -> None:
        self.produced = True


class TestAsyncPath(unittest.IsolatedAsyncioTestCase, TestCaseMixin):
    """Unit tests for AsyncPath path operations."""

    def setUp(self) -> None:
        self.setUpPyfakefs()

    def test_resolve(self) -> None:
        """Tests that resolve returns a resolved AsyncPath."""
        path = AsyncPath('.')
        resolved = path.resolve()
        self.assertIsInstance(resolved, AsyncPath)
        self.assertEqual(resolved, AsyncPath(Path('.').resolve()))

    def test_pure_properties(self) -> None:
        """Tests pure path properties accessed synchronously."""
        p = AsyncPath('/foo/bar/archive.tar.gz')
        self.assertEqual(p.name, 'archive.tar.gz')
        self.assertEqual(p.parent, AsyncPath('/foo/bar'))
        self.assertEqual(p.stem, 'archive.tar')
        self.assertEqual(p.suffix, '.gz')
        self.assertEqual(p.suffixes, ['.tar', '.gz'])
        self.assertEqual(p.parts, (os.sep, 'foo', 'bar', 'archive.tar.gz'))
        self.assertEqual(
            p.parents,
            (
                AsyncPath('/foo/bar'),
                AsyncPath('/foo'),
                AsyncPath('/'),
            ),
        )

    def test_pure_methods(self) -> None:
        """Tests pure path methods returning results synchronously."""
        p = AsyncPath('/foo/bar/baz.txt')
        self.assertEqual(p.with_name('qux.txt'), AsyncPath('/foo/bar/qux.txt'))
        self.assertEqual(p.with_suffix('.json'), AsyncPath('/foo/bar/baz.json'))
        self.assertEqual(
            p.with_stem('new_stem'), AsyncPath('/foo/bar/new_stem.txt')
        )
        self.assertEqual(
            p.relative_to(AsyncPath('/foo')), AsyncPath('bar/baz.txt')
        )
        self.assertEqual(
            p.is_absolute(), Path('/foo/bar/baz.txt').is_absolute()
        )
        self.assertTrue(AsyncPath(Path.cwd()).is_absolute())
        self.assertTrue(p.match('*.txt'))
        self.assertEqual(p.as_posix(), '/foo/bar/baz.txt')

    def test_fspath_and_ordering(self) -> None:
        """Tests os.PathLike protocol and ordering comparisons."""
        p1 = AsyncPath('/a/b')
        p2 = AsyncPath('/a/c')
        self.assertEqual(os.fspath(p1), os.fspath(Path('/a/b')))
        self.assertTrue(p1 < p2)
        self.assertTrue(p2 > p1)
        self.assertEqual(sorted([p2, p1]), [p1, p2])

    async def test_async_io_forwarding(self) -> None:
        """Tests forwarding I/O methods to asyncio executor."""
        p = AsyncPath('/test_dir/test_file.bin')
        self.assertFalse(await p.exists())
        await p.parent.mkdir(parents=True, exist_ok=True)
        bytes_written = await p.write_bytes(b'hello binary')
        self.assertEqual(bytes_written, 12)
        self.assertTrue(await p.exists())
        self.assertTrue(await p.is_file())
        self.assertFalse(await p.is_dir())
        self.assertEqual(await p.read_bytes(), b'hello binary')

        text_file = AsyncPath('/test_dir/test_text.txt')
        await text_file.write_text('hello text')
        self.assertEqual(await text_file.read_text(), 'hello text')

        renamed = await text_file.rename(AsyncPath('/test_dir/renamed.txt'))
        self.assertEqual(renamed, AsyncPath('/test_dir/renamed.txt'))
        self.assertTrue(await renamed.exists())
        self.assertFalse(await text_file.exists())
        await renamed.unlink()
        self.assertFalse(await renamed.exists())

        stat_res = await p.stat()
        self.assertEqual(stat_res.st_size, 12)

        dir_path = AsyncPath('/test_dir')
        self.assertTrue(await dir_path.is_dir())

        glob_results = await dir_path.glob('*.bin')
        self.assertEqual(glob_results, [p])
        glob_async_for = [f async for f in dir_path.glob('*.bin')]
        self.assertEqual(glob_async_for, [p])

        iterdir_await = await dir_path.iterdir()
        self.assertEqual(iterdir_await, [p])
        iterdir_async_for = [f async for f in dir_path.iterdir()]
        self.assertEqual(iterdir_async_for, [p])

        rglob_await = await dir_path.rglob('*.bin')
        self.assertEqual(rglob_await, [p])
        rglob_async_for = [f async for f in dir_path.rglob('*.bin')]
        self.assertEqual(rglob_async_for, [p])


if __name__ == '__main__':
    unittest.main()
