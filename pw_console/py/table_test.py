# Copyright 2021 The Pigweed Authors
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
"""Tests for pw_console.text_formatting"""

from datetime import datetime
import logging
import unittest
from unittest.mock import MagicMock

from parameterized import parameterized  # type: ignore

from pw_console.console_prefs import ConsolePrefs
from pw_console.log_line import LogLine
from pw_console.log_store import LogStore
from pw_console.widgets.table import TableView

_TIMESTAMP_FORMAT = '%Y%m%d %H:%M:%S'
_TIMESTAMP_SAMPLE = datetime(2021, 6, 30, 16, 10, 37, 818901)
_TIMESTAMP_SAMPLE_STRING = _TIMESTAMP_SAMPLE.strftime(_TIMESTAMP_FORMAT)

_TABLE_PADDING = '  '
_TABLE_PADDING_FRAGMENT = ('', _TABLE_PADDING)
_TABLE_HEADER_SEPARATOR = ('', '| ')

formatter = logging.Formatter(
    '\x1b[30m\x1b[47m'
    '%(asctime)s'
    '\x1b[0m'
    ' '
    '\x1b[33m\x1b[1m'
    '%(levelname)s'
    '\x1b[0m'
    ' '
    '%(message)s',
    _TIMESTAMP_FORMAT,
)


def make_log(**kwargs):
    """Create a LogLine instance."""
    # Construct a LogRecord
    attributes = dict(
        name='testlogger',
        levelno=logging.INFO,
        levelname='INF',
        msg='[%s] %.3f %s',
        args=('MOD1', 3.14159, 'Real message here'),
        created=_TIMESTAMP_SAMPLE.timestamp(),
        filename='test.py',
        lineno=42,
        pathname='/home/user/test.py',
    )
    # Override any above attrs that are passed in.
    attributes.update(kwargs)
    # Create the record
    record = logging.makeLogRecord(dict(attributes))
    # Format
    formatted_message = formatter.format(record)
    log_line = LogLine(
        record=record, formatted_log=formatted_message, ansi_stripped_log=''
    )
    log_line.update_metadata()
    return log_line


def _create_log_pane(startup_window_options: dict[str, dict] | None = None):
    log_pane = MagicMock()
    if startup_window_options is None:
        startup_window_options = {}
    log_pane.startup_window_options = MagicMock(
        return_value=startup_window_options
    )
    return log_pane


class TestTableView(unittest.TestCase):
    """Tests for rendering log lines into tables."""

    maxDiff = None

    def setUp(self):
        # Show large diffs
        self.prefs = ConsolePrefs(
            project_file=False, project_user_file=False, user_file=False
        )
        self.prefs.reset_config()

    @parameterized.expand(
        [
            (
                'Correct column widths with all fields set',
                [
                    make_log(
                        args=('M1', 1.2345, 'Something happened'),
                        extra_metadata_fields=dict(module='M1', anumber=12),
                    ),
                    make_log(
                        args=('MD2', 567.5, 'Another cool event'),
                        extra_metadata_fields=dict(module='MD2', anumber=123),
                    ),
                ],
                dict(module=len('MD2'), anumber=len('123')),
            ),
            (
                'Missing metadata fields on some rows',
                [
                    make_log(
                        args=('M1', 54321.2, 'Something happened'),
                        extra_metadata_fields=dict(
                            module='M1', anumber=54321.2
                        ),
                    ),
                    make_log(
                        args=('MOD2', 567.5, 'Another cool event'),
                        extra_metadata_fields=dict(module='MOD2'),
                    ),
                ],
                dict(module=len('MOD2'), anumber=len('54321.200')),
            ),
        ]
    )
    def test_column_widths(self, _name, logs, expected_widths) -> None:
        """Test colum widths calculation."""
        table = TableView(prefs=self.prefs, log_pane=_create_log_pane())
        log_store = LogStore()
        for log in logs:
            log_store.update_metadata_column_widths(log)
            table.update_column_widths_from_logs(log_store.column_widths)
            metadata_fields = {
                k: v
                for k, v in log.metadata.fields.items()
                if k not in ['py_file', 'py_logger']
            }
            # LogStore.update_metadata_column_widths should populate
            # log the log metadata fields.
            self.assertEqual(metadata_fields, log.record.extra_metadata_fields)
        # Check expected column widths
        results = {
            k: v
            for k, v in table.column_width_from_logs.items()
            if k not in ['time', 'level', 'py_file', 'py_logger']
        }
        self.assertCountEqual(expected_widths, results)

    @parameterized.expand(
        [
            (
                'Build header adding fields incrementally',
                [
                    make_log(
                        args=('MODULE2', 567.5, 'Another cool event'),
                        extra_metadata_fields=dict(
                            # timestamp missing
                            module='MODULE2'
                        ),
                    ),
                    make_log(
                        args=('MODULE1', 54321.2, 'Something happened'),
                        extra_metadata_fields=dict(
                            # timestamp added in
                            module='MODULE1',
                            timestamp=54321.2,
                        ),
                    ),
                ],
                [
                    [
                        ('bold', 'Time             '),
                        _TABLE_HEADER_SEPARATOR,
                        ('bold', 'Lev'),
                        _TABLE_HEADER_SEPARATOR,
                        ('bold', 'Module '),
                        _TABLE_HEADER_SEPARATOR,
                        ('bold', 'Message'.ljust(100)),
                    ],
                    [
                        ('bold', 'Time             '),
                        _TABLE_HEADER_SEPARATOR,
                        ('bold', 'Lev'),
                        _TABLE_HEADER_SEPARATOR,
                        ('bold', 'Module '),
                        _TABLE_HEADER_SEPARATOR,
                        # timestamp added in
                        ('bold', 'Timestamp'),
                        _TABLE_HEADER_SEPARATOR,
                        ('bold', 'Message'.ljust(100)),
                    ],
                ],
            ),
        ]
    )
    def test_formatted_header(self, _name, logs, expected_headers) -> None:
        """Test colum widths calculation."""
        table = TableView(prefs=self.prefs, log_pane=_create_log_pane())
        log_store = LogStore()

        for log, header in zip(logs, expected_headers):
            log_store.update_metadata_column_widths(log)
            table.update_column_widths_from_logs(log_store.column_widths)
            self.assertEqual(table.formatted_header(), header)

    @parameterized.expand(
        [
            (
                'Build rows adding fields incrementally',
                [
                    make_log(
                        args=('MODULE2', 567.5, 'Another cool event'),
                        extra_metadata_fields=dict(
                            # timestamp missing
                            module='MODULE2',
                            msg='Another cool event',
                        ),
                    ),
                    make_log(
                        args=('MODULE2', 567.5, 'Another cool event'),
                        extra_metadata_fields=dict(
                            # timestamp and msg missing
                            module='MODULE2'
                        ),
                    ),
                    make_log(
                        args=('MODULE1', 54321.2, 'Something happened'),
                        extra_metadata_fields=dict(
                            # timestamp added in
                            module='MODULE1',
                            timestamp=54321.2,
                            msg='Something happened',
                        ),
                    ),
                ],
                [
                    [
                        ('class:log-time', _TIMESTAMP_SAMPLE_STRING),
                        _TABLE_PADDING_FRAGMENT,
                        ('class:log-level-20', 'INF'),
                        _TABLE_PADDING_FRAGMENT,
                        ('class:log-table-column-0', 'MODULE2'),
                        _TABLE_PADDING_FRAGMENT,
                        ('', 'Another cool event'),
                        ('', '\n'),
                    ],
                    [
                        ('class:log-time', _TIMESTAMP_SAMPLE_STRING),
                        _TABLE_PADDING_FRAGMENT,
                        ('class:log-level-20', 'INF'),
                        _TABLE_PADDING_FRAGMENT,
                        ('class:log-table-column-0', 'MODULE2'),
                        _TABLE_PADDING_FRAGMENT,
                        ('', '[MODULE2] 567.500 Another cool event'),
                        ('', '\n'),
                    ],
                    [
                        ('class:log-time', _TIMESTAMP_SAMPLE_STRING),
                        _TABLE_PADDING_FRAGMENT,
                        ('class:log-level-20', 'INF'),
                        _TABLE_PADDING_FRAGMENT,
                        ('class:log-table-column-0', 'MODULE1'),
                        _TABLE_PADDING_FRAGMENT,
                        ('class:log-table-column-1', '54321.200'),
                        _TABLE_PADDING_FRAGMENT,
                        ('', 'Something happened'),
                        ('', '\n'),
                    ],
                ],
            ),
        ]
    )
    def test_formatted_rows(self, _name, logs, expected_log_format) -> None:
        """Test colum widths calculation."""
        table = TableView(prefs=self.prefs, log_pane=_create_log_pane())
        log_store = LogStore()
        # Check each row meets expected formats incrementally.
        for log, formatted_log in zip(logs, expected_log_format):
            log_store.update_metadata_column_widths(log)
            table.update_column_widths_from_logs(log_store.column_widths)
            self.assertEqual(formatted_log, table.formatted_row(log))


if __name__ == '__main__':
    unittest.main()
