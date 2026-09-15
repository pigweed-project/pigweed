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
"""Defines the PipelineStage base class for the pw_fortifier pipeline."""

from abc import ABC, abstractmethod
import argparse
import asyncio
import logging
import re
import shutil
import subprocess
import time

import requests

from pw_fortifier.async_path import AsyncPath
from pw_fortifier.git_utils import (
    ReadOnlyGitWorkspace,
    WritableGitWorkspace,
)

DEFAULT_MAX_RETRIES = 3


class BasicPipelineStage(ABC):
    """Basic interface representing a stage in a data processing pipeline."""

    def __init__(self, name: str | None = None) -> None:
        if name is not None:
            self._name = name
        else:
            name = self.__class__.__name__
            s1 = re.sub('(.)([A-Z][a-z]+)', r'\1_\2', name)
            self._name = re.sub('([a-z0-9])([A-Z])', r'\1_\2', s1).lower()
        self._src_repo: ReadOnlyGitWorkspace | None = None
        self._dst_repo: WritableGitWorkspace | None = None
        self._max_retries: int = DEFAULT_MAX_RETRIES
        self._verbose: bool = False
        self._skip_setup: bool = False

    @property
    def skip_setup(self) -> bool:
        """Whether to skip the setup phase in run()."""
        return self._skip_setup

    @skip_setup.setter
    def skip_setup(self, value: bool) -> None:
        self._skip_setup = value

    @property
    def name(self) -> str:
        """Returns the stage's name, derived from the class name.

        Returns:
            The stage name string.
        """
        return self._name

    @property
    def src_repo(self) -> ReadOnlyGitWorkspace | None:
        """Returns the source Git repository workspace.

        Returns:
            ReadOnlyGitWorkspace instance or None.
        """
        return self._src_repo

    @src_repo.setter
    def src_repo(self, value: ReadOnlyGitWorkspace | None) -> None:
        """Sets the source Git repository workspace."""
        self._src_repo = value

    @property
    def dst_repo(self) -> WritableGitWorkspace | None:
        """Returns the destination Git repository workspace.

        Returns:
            WritableGitWorkspace instance or None.
        """
        return self._dst_repo

    @dst_repo.setter
    def dst_repo(self, value: WritableGitWorkspace | None) -> None:
        """Sets the destination Git repository workspace."""
        self._dst_repo = value

    async def configure(self, args: argparse.Namespace) -> None:
        """Configures the stage.

        Args:
            args: Command-line arguments namespace.
        """
        self._max_retries = args.max_retries
        self._verbose = args.verbose

    @abstractmethod
    async def run(self) -> None:
        """Runs the main processing loop."""
        raise NotImplementedError


class PipelineConsumerMixin(ABC):
    """A consumer mixin that receives paths from an input queue."""

    _max_retries: int = DEFAULT_MAX_RETRIES

    def __init__(self) -> None:
        """Initializes the consumer with an input queue."""
        self._input_queue: asyncio.Queue[AsyncPath | None] = asyncio.Queue()
        self._preserve_inputs: bool = False
        self._in_dir: AsyncPath | None = None
        self._err_dir: AsyncPath | None = None

    @property
    def input_queue(self) -> asyncio.Queue[AsyncPath | None]:
        """Returns the input queue of this consumer.

        Returns:
            The input Queue instance.
        """
        return self._input_queue

    async def _configure(self, name: str, args: argparse.Namespace) -> None:
        """Configures the stage's name and directories.

        Args:
            args: The command-line arguments containing configuration.
        """
        self._in_dir = AsyncPath(args.working_dir, f'{name}_in')
        self._err_dir = AsyncPath(args.working_dir, f'{name}_err')
        if not args.resume:
            loop = asyncio.get_running_loop()
            if await self._in_dir.exists():
                await loop.run_in_executor(
                    None, shutil.rmtree, self._in_dir.path
                )
            if await self._err_dir.exists():
                await loop.run_in_executor(
                    None, shutil.rmtree, self._err_dir.path
                )
        await self._in_dir.mkdir(parents=True, exist_ok=True)
        await self._err_dir.mkdir(parents=True, exist_ok=True)

    async def _run(self) -> None:
        """Runs the stage's main processing loop."""
        assert self._in_dir is not None
        assert self._err_dir is not None

        retries = 0
        while True:
            filenames = []
            async for f in self._in_dir.iterdir():
                if await f.is_file():
                    filenames.append(f.name)
            filenames.sort()

            path: AsyncPath | None = None
            if filenames:
                path = AsyncPath(self._in_dir, filenames[0])
            else:
                queue_path = await self._input_queue.get()
                if queue_path is None:
                    break

                if self._preserve_inputs:
                    path = queue_path
                else:
                    path = AsyncPath(self._in_dir, queue_path.name)
                    await queue_path.rename(path)

            try:
                await self._process_one(path)
                if not self._preserve_inputs:
                    await path.unlink(missing_ok=True)
                retries = 0
            except (requests.RequestException, subprocess.SubprocessError) as e:
                logging.warning(
                    'Transient error processing %s (retry %d): %s',
                    path.name,
                    retries,
                    e,
                )
                retries += 1
                if retries <= self._max_retries:
                    continue
                logging.error(
                    'Failed to process %s after %d retries: %s',
                    path.name,
                    self._max_retries,
                    e,
                )
                if not self._preserve_inputs:
                    err_path = AsyncPath(self._err_dir, path.name)
                    try:
                        await path.rename(err_path)
                    except FileNotFoundError as err:
                        logging.error(
                            'Failed to save %s to %s: %s',
                            path.name,
                            err_path,
                            err,
                        )
                retries = 0

    @abstractmethod
    async def _process_one(self, path: AsyncPath) -> None:
        """Processes a single input file path."""
        raise NotImplementedError


class PipelineConsumerStage(BasicPipelineStage, PipelineConsumerMixin):
    """Pipeline stage that consumes items."""

    def __init__(self, name: str | None = None) -> None:
        """Initializes the pipeline stage with default queues and folders."""
        BasicPipelineStage.__init__(self, name)
        PipelineConsumerMixin.__init__(self)

    async def configure(self, args: argparse.Namespace) -> None:
        """Configures the stage's name and directories.

        Args:
            args: The command-line arguments containing configuration.
        """
        await BasicPipelineStage.configure(self, args)
        await PipelineConsumerMixin._configure(self, self.name, args)

    async def run(self) -> None:
        """Runs the stage's main processing loop."""
        if not self.skip_setup:
            await self._set_up()
        await PipelineConsumerMixin._run(self)
        await self._tear_down()

    async def _set_up(self) -> None:
        """Hook for subclasses to perform asynchronous initialization."""

    async def _tear_down(self) -> None:
        """Hook for subclasses to perform asynchronous cleanup."""


class PipelineSink(PipelineConsumerMixin):
    """Pipeline consumer that acts as a sink, discarding received items."""

    async def _process_one(self, path: AsyncPath) -> None:
        return None


class PipelineProducerMixin:
    """A producer mixin that sends paths to a connected consumer."""

    def __init__(self) -> None:
        """Initializes the producer with no connected output queue."""
        self._output_queue: asyncio.Queue[AsyncPath | None] | None = None
        self._out_dir: AsyncPath | None = None

    def connect(self, next_stage: PipelineConsumerMixin) -> None:
        """Connects this producer to a consumer.

        Args:
            next_stage: The consumer stage to connect to.
        """
        assert self._output_queue is None, 'Already connected'
        self._output_queue = next_stage.input_queue

    async def _configure(self, name: str, args: argparse.Namespace) -> None:
        """Configures the stage's name and directories.

        Args:
            args: The command-line arguments containing configuration.
        """
        self._out_dir = AsyncPath(args.working_dir, f'{name}_out')
        if not args.resume and await self._out_dir.exists():
            loop = asyncio.get_running_loop()
            await loop.run_in_executor(None, shutil.rmtree, self._out_dir.path)
        await self._out_dir.mkdir(parents=True, exist_ok=True)

    async def _forward_one(self, path: AsyncPath) -> None:
        """Forwards a processed file or report to the next stage."""
        assert self._out_dir is not None
        out_path = AsyncPath(self._out_dir, path.name)
        await path.rename(out_path)
        self.send(out_path)

    async def _generate_out_path(self) -> AsyncPath:
        """Generates a unique report path in the output directory."""
        assert self._out_dir is not None
        while True:
            out_path = AsyncPath(self._out_dir, f'{time.time_ns()}.json')
            if not await out_path.exists():
                return out_path

    def send(self, path: AsyncPath) -> None:
        """Sends a path to the connected consumer.

        Args:
            path: The AsyncPath to send downstream.
        """
        assert self._output_queue is not None, 'Output queue is not connected'
        self._output_queue.put_nowait(path)

    async def close(self) -> None:
        """Closes the producer by sending the None sentinel."""
        if self._output_queue is not None:
            await self._output_queue.put(None)


class PipelineProducerStage(BasicPipelineStage, PipelineProducerMixin):
    """Pipeline stage that produces items."""

    def __init__(self, name: str | None = None) -> None:
        """Initializes the pipeline stage with default queues and folders."""
        BasicPipelineStage.__init__(self, name)
        PipelineProducerMixin.__init__(self)

    async def configure(self, args: argparse.Namespace) -> None:
        """Configures the stage's name and directories.

        Args:
            args: The command-line arguments containing configuration.
        """
        await BasicPipelineStage.configure(self, args)
        await PipelineProducerMixin._configure(self, self.name, args)

    async def run(self) -> None:
        """Runs the stage's main processing loop."""
        if not self.skip_setup:
            await self._set_up()
        try:
            await self._produce_all()
        finally:
            await self._tear_down()

    async def _set_up(self) -> None:
        """Hook for subclasses to perform asynchronous initialization."""

    @abstractmethod
    async def _produce_all(self) -> None:
        """Produces and sends all inputs one by one."""
        raise NotImplementedError

    async def _tear_down(self) -> None:
        """Hook for subclasses to perform asynchronous cleanup."""
        await self.close()


class PipelineStage(
    BasicPipelineStage, PipelineConsumerMixin, PipelineProducerMixin
):
    """Base class for all stages in the defect finding and fixing pipeline."""

    def __init__(self, name: str | None = None) -> None:
        """Initializes the pipeline stage with default queues and folders."""
        BasicPipelineStage.__init__(self, name)
        PipelineConsumerMixin.__init__(self)
        PipelineProducerMixin.__init__(self)

    async def configure(self, args: argparse.Namespace) -> None:
        """Configures the stage's name and directories.

        Args:
            args: The command-line arguments containing configuration.
        """
        await BasicPipelineStage.configure(self, args)
        await PipelineConsumerMixin._configure(self, self.name, args)
        await PipelineProducerMixin._configure(self, self.name, args)

    async def run(self) -> None:
        """Runs the stage's main processing loop."""
        if not self.skip_setup:
            await self._set_up()
        await PipelineConsumerMixin._run(self)
        await self._tear_down()

    async def _set_up(self) -> None:
        """Hook for subclasses to perform asynchronous initialization."""

    async def _tear_down(self) -> None:
        """Hook for subclasses to perform asynchronous cleanup."""
        await self.close()


class PipelineMux(PipelineStage):
    """Muxes input from a single consumer to multiple consumers."""

    def __init__(self, name: str | None = None) -> None:
        """Initializes the mux with an empty fanout mapping."""
        super().__init__(name)
        self._preserve_inputs = True
        self._fanout: dict[str, PipelineConsumerMixin] = {}
        self._strict_matching: bool = False

    @property
    def strict_matching(self) -> bool:
        """Whether to raise an error if a target stage is not found."""
        return self._strict_matching

    @strict_matching.setter
    def strict_matching(self, value: bool) -> None:
        self._strict_matching = value

    @property
    def targets(self) -> list[str]:
        """Returns the registered target names.

        Returns:
            List of target names string.
        """
        return list(self._fanout.keys())

    def add_stage(self, target: str, next_stage: PipelineConsumerMixin) -> None:
        """Maps a target basename to a next stage.

        Args:
            target: Basename of the file to match.
            next_stage: Consumer stage to forward the file to.
        """
        self._fanout[target] = next_stage

    async def _process_one(self, path: AsyncPath) -> None:
        target_stage = self._fanout.get(path.name)
        if target_stage:
            target_stage.input_queue.put_nowait(path)
        elif self._strict_matching:
            raise ValueError(f"No target stage registered for '{path.name}'")

    async def _tear_down(self) -> None:
        for next_stage in self._fanout.values():
            await next_stage.input_queue.put(None)


class PipelineDemux(PipelineProducerStage):
    """Demuxes input from multiple producers to a single output."""

    def __init__(self, name: str | None = None) -> None:
        """Initializes the demux with empty fanin."""
        super().__init__(name)
        self._fanin: list[PipelineProducerMixin] = []
        self._consumers: dict[PipelineProducerMixin, PipelineSink] = {}
        self._tasks: dict[
            asyncio.Task[AsyncPath | None], PipelineProducerMixin
        ] = {}

    @property
    def fanin(self) -> list[PipelineProducerMixin]:
        """Returns the list of previous stages fanning in.

        Returns:
            List of producer stages.
        """
        return self._fanin

    def add_stage(self, prev_stage: PipelineProducerMixin) -> None:
        """Adds a previous stage to the demux.

        Args:
            prev_stage: The producer stage to add.
        """
        self._fanin.append(prev_stage)
        consumer = PipelineSink()
        prev_stage.connect(consumer)
        self._consumers[prev_stage] = consumer

    async def _set_up(self) -> None:
        for stage in self._fanin:
            self._tasks[self._create_task(stage)] = stage

    async def _produce_all(self) -> None:
        while self._tasks:
            done, _ = await asyncio.wait(
                self._tasks.keys(), return_when=asyncio.FIRST_COMPLETED
            )
            for task in done:
                stage = self._tasks.pop(task)
                res = task.result()
                if res is None:
                    self._fanin.remove(stage)
                    del self._consumers[stage]
                    continue

                self.send(res)
                if stage in self._fanin:
                    self._tasks[self._create_task(stage)] = stage

    async def _tear_down(self) -> None:
        for task in self._tasks.keys():
            task.cancel()
        self._tasks = {}
        await super()._tear_down()

    def _create_task(
        self, stage: PipelineProducerMixin
    ) -> asyncio.Task[AsyncPath | None]:
        consumer = self._consumers[stage]
        return asyncio.create_task(consumer.input_queue.get())


################################################################################
# Test support


class PipelineStageStub(PipelineStage):
    """Stub PipelineStage that allows choosing whether to forward or drop."""

    def __init__(
        self, should_forward: bool = True, should_fail: bool = False
    ) -> None:
        """Initializes the stub with configuration options.

        Args:
            should_forward: If True, forward outputs to next stage.
            should_fail: If True, raise error during processing.
        """
        super().__init__()
        self.should_forward = should_forward
        self.should_fail = should_fail
        self.processed_paths: list[AsyncPath] = []
        self.setup_called = False
        self.teardown_called = False

    async def _set_up(self) -> None:
        """Sets a flag to verify setup was called."""
        self.setup_called = True

    async def _process_one(self, path: AsyncPath) -> None:
        """Processes a single path, either failing, forwarding, or dropping."""
        self.processed_paths.append(path)
        if self.should_fail:
            raise subprocess.SubprocessError('Simulated processing failure')
        if self.should_forward:
            await self._forward_one(path)

    async def _tear_down(self) -> None:
        """Sets a flag to verify teardown was called."""
        self.teardown_called = True
        await super()._tear_down()
