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
"""Defines the Emitter pipeline stage for producing paths to analyze."""

import argparse
from pathlib import Path

from pw_fortifier.async_path import AsyncPath
from pw_fortifier.pipeline_stage import PipelineProducerStage
from pw_fortifier.path_enumerator import PathEnumerator


class Emitter(PipelineProducerStage):
    """First stage in the pipeline that enumerates files to be scanned."""

    def __init__(self, enumerator: PathEnumerator | None = None) -> None:
        """Initializes the emitter with an optional path enumerator.

        Args:
            enumerator: Optional PathEnumerator instance to wrap.
        """
        super().__init__()
        self._enumerator: PathEnumerator | None = enumerator
        self._files: list[str] | None = None
        self._resume = False
        self._last_emitted_path: AsyncPath | None = None
        self._preserve_inputs = True

    @property
    def enumerator(self) -> PathEnumerator:
        """Returns the path enumerator.

        Returns:
            The PathEnumerator instance.
        """
        assert self._enumerator is not None
        return self._enumerator

    async def configure(self, args: argparse.Namespace) -> None:
        """Configures the stage.

        Args:
            args: Command-line arguments namespace object.
        """
        await super().configure(args)
        self._files = args.files
        self._resume = args.resume
        self._last_emitted_path = (
            AsyncPath(args.working_dir) / 'last_emitted.txt'
        )
        if self._enumerator is None:
            assert self._src_repo is not None
            self._enumerator = PathEnumerator(self._src_repo)

    async def _produce_all(self) -> None:
        """Runs the emitter stage to discover and send file paths."""
        assert self._enumerator is not None

        # THe caller did not provide files and wants to scan the whole repo.
        if self._files is None:
            await self._enumerate()
            return

        # The caller provided specific file patterns.
        project_dir = self._enumerator.src_repo.project_dir
        for file_pattern in self._files:
            rel_pattern = file_pattern
            p = Path(file_pattern)
            if p.is_absolute() or p.root:
                resolved_p = p.resolve()
                resolved_project_dir = project_dir.resolve()
                if not resolved_p.is_relative_to(resolved_project_dir):
                    raise ValueError(
                        f'File pattern {file_pattern!r} is outside '
                        f'repository {project_dir!r}'
                    )
                rel_pattern = str(resolved_p.relative_to(resolved_project_dir))
            for path in sorted(project_dir.glob(rel_pattern)):
                if path.is_file():
                    self.send(AsyncPath(path))

    async def _enumerate(self) -> None:
        """Enumerates matching paths from the underlying enumerator."""
        assert self._enumerator is not None
        assert self._last_emitted_path is not None

        path_iter = iter(self._enumerator)

        previous = None
        if self._resume and await self._last_emitted_path.exists():
            previous = await self._last_emitted_path.read_text()

        while True:
            try:
                next_path = next(path_iter)
            except StopIteration:
                await self._last_emitted_path.unlink(missing_ok=True)
                return

            abs_path = AsyncPath(
                self._enumerator.src_repo.project_dir / next_path
            )
            if previous is None:
                self.send(abs_path)
                await self._last_emitted_path.write_text(str(abs_path))

            elif previous == str(abs_path):
                previous = None

    async def _process_one(self, path: AsyncPath) -> None:
        """Overrides PipelineStage._process_one (unused by Emitter)."""
