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
"""Defines the generic Collector stage."""

import argparse
import csv
from typing import Final, IO, Any, NamedTuple

from pw_fortifier.async_path import AsyncPath
from pw_fortifier.pipeline_stage import PipelineConsumerStage


class _FieldSpec(NamedTuple):
    name: str
    width: int
    hidden: bool
    truncate_from_left: bool


class Collector(PipelineConsumerStage):
    """Final stage in the pipeline that collects and summarizes outputs."""

    MAX_WIDTH: Final[int] = 120

    def __init__(self) -> None:
        """Initializes the collector."""
        super().__init__()
        self._count = 0
        self._fields: list[_FieldSpec] = []
        self._separator = '+'
        self._format = '|'
        self._csv_file: IO[str] | None = None
        self._csv_writer: Any = None

    async def configure(self, args: argparse.Namespace) -> None:
        """Configures the stage.

        Args:
            args: Command-line arguments namespace object.
        """
        await super().configure(args)
        if args.output:
            out_path = AsyncPath(args.output)
            await out_path.parent.mkdir(parents=True, exist_ok=True)
            self._csv_file = open(
                args.output, 'w', newline='', encoding='utf-8'
            )
            self._csv_writer = csv.writer(self._csv_file)

    def _add_field(
        self,
        field: str,
        min_width: int = 0,
        hidden: bool = False,
        truncate_from_left: bool = False,
    ) -> None:
        """Adds a column field specification for output formatting.

        Args:
            field: Name of the field / column header.
            min_width: Minimum width for the column (-1 for expanding).
            hidden: Whether this field should be omitted from terminal output.
            truncate_from_left: Whether long values should be truncated from
                the left ('...xyz') rather than the right ('abc...').
        """
        if hidden:
            width = max(min_width, len(field), 4)
        else:
            max_width = self.MAX_WIDTH - len(self._separator) - 3
            assert max_width >= 3
            assert len(field) <= max_width
            width = max_width if min_width < 0 else max(min_width, len(field))
            width = max(width, 4)

            fmt_str = f' {{:<{width}}} |'
            self._format += fmt_str
            self._separator += '-' * (width + 2) + '+'
            assert len(self._separator) <= self.MAX_WIDTH

        self._fields.append(
            _FieldSpec(
                name=field,
                width=width,
                hidden=hidden,
                truncate_from_left=truncate_from_left,
            )
        )

    def _report_fields(self, *args: str) -> None:
        """Formats and reports a row of field values."""
        assert len(args) == len(
            self._fields
        ), f'Expected {len(self._fields)} arguments, got {len(args)}'
        argslist = list(args)
        if self._csv_writer is not None:
            self._csv_writer.writerow(argslist)

        for i, arg in enumerate(argslist):
            spec = self._fields[i]
            if len(arg) > spec.width:
                if spec.truncate_from_left:
                    argslist[i] = '...' + arg[-(spec.width - 3) :]
                else:
                    argslist[i] = arg[: (spec.width - 3)] + '...'

        visible_args = [
            arg for arg, spec in zip(argslist, self._fields) if not spec.hidden
        ]
        print(self._format.format(*visible_args))

    async def _process_one(self, path: AsyncPath) -> None:
        """Processes a single item by delegating to _print_item."""
        if self._count == 0:
            visible_headers = [
                spec.name for spec in self._fields if not spec.hidden
            ]
            print(self._format.format(*visible_headers))
            print(self._separator)
            if self._csv_writer is not None:
                self._csv_writer.writerow([spec.name for spec in self._fields])

        self._count += 1
        await self._print_item(path)

    async def _tear_down(self) -> None:
        """Prints a summary of all collected items to stdout."""
        print(f'\nDONE! Processed {self._count} reports.')
        if self._csv_file is not None:
            self._csv_file.close()

    async def _print_item(self, path: AsyncPath) -> None:
        """Prints a single item. Subclasses must implement this."""
        raise NotImplementedError
