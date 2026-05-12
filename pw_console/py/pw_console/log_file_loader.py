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
"""Pigweed Console log file loading facilities."""

import fnmatch
import logging
import os
from pathlib import Path
from typing import Any, IO, Iterable, NamedTuple
import zipfile

from prompt_toolkit.shortcuts import ProgressBar, ProgressBarCounter
from prompt_toolkit.shortcuts.progress_bar import formatters as pb_formatters

from pw_console.background_command_log_parsers import (
    get_command_log_parser,
)
from pw_console.log_store import LogStore


_PROGRESS_BAR_FORMAT = [
    pb_formatters.Label(),
    pb_formatters.Text(" "),
    pb_formatters.Bar(sym_a="#", sym_b="#", sym_c="."),
    pb_formatters.Text(" "),
    pb_formatters.Text("[", style="class:percentage"),
    pb_formatters.Percentage(),
    pb_formatters.Text("]", style="class:percentage"),
    pb_formatters.Text(" "),
    pb_formatters.Progress(),
    pb_formatters.Text(" KB"),
]


class FileLogStore(NamedTuple):
    """Class to associate a file with the correct logger and LogStore."""

    path: Path
    logger: logging.Logger
    log_store: LogStore


class LogFileLoader:
    """Pigweed Console log file loader class."""

    def __init__(
        self,
        files: list[Path],
        log_parser: str,
        merge_files: bool = False,
        zip_globs: list[str] | None = None,
        reverse_file_order: bool = False,
    ) -> None:
        self.files = files
        self.zip_globs = ['*']
        if zip_globs:
            self.zip_globs = zip_globs
        self.reverse_file_order = reverse_file_order

        self.log_parser_name = log_parser
        self.log_parser = get_command_log_parser(log_parser)
        if self.log_parser is None:
            raise ValueError(f'Unknown log parser: {log_parser}')

        self.default_formatter = logging.Formatter('%(message)s')
        self.merge_files = merge_files

        self.merged_logger = logging.getLogger('pw_console.open_files')
        self.merged_logger.propagate = False

        if self.merge_files:
            # Use one logger for all files.
            self.merged_log_store = LogStore()
            self.merged_log_store.setFormatter(self.default_formatter)
            self.merged_logger.addHandler(self.merged_log_store)

        self.file_log_stores: dict[Path, FileLogStore] = {}

        self.pb: ProgressBar | None = None

    def load_files(self) -> None:
        """Load all files into respective log panes."""
        with ProgressBar(
            title='Loading...',
            formatters=_PROGRESS_BAR_FORMAT,
        ) as pb:
            self.pb = pb
            for f in self.files:
                path = Path(os.path.expandvars(f.expanduser()))
                if path.suffix == '.zip':
                    self.load_zip_file(path)
                else:
                    self.load_file(path)

    def load_file(self, input_file: Path) -> None:
        """Load a single plain text file."""
        fls = self.get_log_store(input_file)
        file_size = int(input_file.stat().st_size / 1000)

        # Add a progress bar line for this file.
        assert self.pb
        progress: ProgressBarCounter = self.pb(
            label=f'{input_file.name}', total=file_size
        )
        with input_file.open('rb') as f:
            self.load_lines(fls, f)
        # For plaintext files we only update the progress at the end. So the bar
        # goes from 0 to 100% but there is a new bar for each file.
        progress.items_completed += file_size
        progress.done = True

    def load_zip_file(self, input_file: Path) -> None:
        """Load zip file contents."""
        with zipfile.ZipFile(input_file, 'r') as zf:
            zip_file_names = zf.namelist()
            matched_files = set()

            # Match files we want to load.
            for pattern in self.zip_globs:
                for filename in fnmatch.filter(zip_file_names, pattern):
                    matched_files.add(filename)

            # Sort ascending or descending.
            sorted_files = sorted(
                list(matched_files), reverse=self.reverse_file_order
            )
            # Calculate the total size.
            total_size = 0
            for fname in sorted_files:
                total_size += zf.getinfo(fname).file_size
            total_size = int(total_size / 1000)

            def _load_sub_file(fname: str) -> int:
                fls = self.get_log_store(Path(fname))
                with zf.open(fname, 'r') as f:
                    self.load_lines(fls, f)
                file_size = zf.getinfo(fname).file_size
                return int(file_size / 1000)

            def _load_all_files() -> Iterable[int]:
                for fname in sorted_files:
                    yield _load_sub_file(fname)

            # Add a progress bar line for the zip file.
            assert self.pb
            progress: ProgressBarCounter = self.pb(
                label=f'{input_file.name}', total=total_size
            )
            # Update the bar with the processed kb after each sub file.
            for data_processed in _load_all_files():
                progress.items_completed += data_processed
            progress.done = True

    def load_lines(
        self,
        file_log_store: FileLogStore,
        file_handle: IO[bytes],
    ) -> None:
        """Parse file lines into log messages."""
        assert self.log_parser

        for line in file_handle.readlines():
            self.log_parser(
                file_log_store.logger,
                line.decode('utf-8', errors='replace'),
            )

    def get_log_store(self, file_path: Path) -> FileLogStore:
        """Return the correct log store instance for a given file."""
        if file_path in self.file_log_stores:
            return self.file_log_stores[file_path]

        if self.merge_files:
            log_store = self.merged_log_store
            logger = self.merged_logger
        else:
            log_store = LogStore()
            log_store.setFormatter(self.default_formatter)
            logger = logging.getLogger(str(file_path).replace('.', '_'))
            logger.propagate = False
            logger.addHandler(log_store)

        self.file_log_stores[file_path] = FileLogStore(
            file_path, logger, log_store
        )
        return self.file_log_stores[file_path]

    def default_loggers(self) -> dict[str, LogStore]:
        """Return expected log panes for the loaded files."""
        if self.merge_files:
            return {self.merged_logger.name: self.merged_log_store}

        return {
            str(fls.path): fls.log_store
            for fls in self.file_log_stores.values()
        }

    def window_config(self) -> dict[str, dict]:
        """Return the window config for a log pane displaying file contents."""
        log_window_options: dict[str, Any] = {}

        if self.log_parser_name == 'basic':
            log_window_options['table_mode'] = False
        elif self.log_parser_name in [
            'android-logcat-text',
            'android-persistent-logcat-text',
        ]:
            log_window_options.update(
                {
                    'column_order': [
                        'time',
                        'timestamp',
                        'level',
                        'user',
                        'pid',
                        'tid',
                        'tag',
                    ],
                    'column_width': {
                        'tag': 20,
                    },
                    'column_visibility': {
                        'time': False,
                        'level': False,
                    },
                }
            )

        if self.merge_files:
            return {
                "Group 1 stacked": {
                    'pw_console.open_files': log_window_options,
                    'Python Repl': {'hidden': True},
                }
            }

        overridden_window_config = {
            "Group 1 tabbed": {
                str(fls.path): log_window_options
                for fls in self.file_log_stores.values()
            }
        }

        # Move Python Repl to the end of the group.
        overridden_window_config['Group 1 tabbed'].update(
            {'Python Repl': None}  # type: ignore
        )
        return overridden_window_config
