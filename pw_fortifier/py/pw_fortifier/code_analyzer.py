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
"""Defines the CodeAnalyzer base class and helper types for scanning files."""

import time
from typing import AsyncIterator

from pw_fortifier.async_path import AsyncPath
from pw_fortifier.code_snippet import CodeSnippet
from pw_fortifier.defect import Defect, SOURCE_FILE_PATTERN
from pw_fortifier.pipeline_stage import PipelineStage


class CodeAnalyzer(PipelineStage):
    """Base class for stages that analyze codebase files for defects."""

    def __init__(self) -> None:
        """Initializes the analyzer."""
        super().__init__()
        self._preserve_inputs = True

    async def _process_one(self, path: AsyncPath) -> None:
        """Processes the trigger by scanning the file specified in it."""
        assert self._in_dir is not None
        assert self._out_dir is not None
        async for scan_result_path in self._analyze(path, self._out_dir):
            description = await scan_result_path.read_text()
            affected_files = list(
                dict.fromkeys(SOURCE_FILE_PATTERN.findall(description))
            )

            defect_path = await self._generate_out_path()
            primary_file = (
                affected_files[0] if affected_files else defect_path.name
            )

            defect = Defect(
                location=CodeSnippet(file=primary_file),
                issue_id=None,
                title=None,
                description=description,
                cl_num=None,
                affected_files=affected_files,
            )

            await defect.save(defect_path)
            await self._forward_one(defect_path)

    def _analyze(
        self, input_file: AsyncPath, out_path: AsyncPath
    ) -> AsyncIterator[AsyncPath]:
        """Scans a single file and writes findings to the output path."""
        raise NotImplementedError


################################################################################
# Test support


class CodeAnalyzerStub(CodeAnalyzer):
    """A stub implementation of CodeAnalyzer for testing."""

    def __init__(self) -> None:
        """Initializes the stub."""
        super().__init__()
        self.scanned_files: list[AsyncPath] = []

    def _analyze(
        self, input_file: AsyncPath, out_path: AsyncPath
    ) -> AsyncIterator[AsyncPath]:
        """Simulates a scan by generating a mock Markdown defect."""

        async def _impl():
            self.scanned_files.append(input_file)
            timestamp = time.time_ns()
            filename = f'report-{timestamp}.md'
            defect_path = out_path / filename

            content = (
                f'# Stub Defect\n\n'
                f'A defect detected by the CodeAnalyzer stub in '
                f'{input_file.name}.\n'
            )
            await defect_path.write_text(content)
            yield defect_path

        return _impl()
