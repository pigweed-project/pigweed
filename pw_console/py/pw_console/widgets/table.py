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
"""Table view renderer for LogLines."""

import collections
import copy
import logging
from typing import Iterable, TYPE_CHECKING

from prompt_toolkit.formatted_text import (
    ANSI,
    OneStyleAndTextTuple,
    StyleAndTextTuples,
)

from pw_console.console_prefs import ConsolePrefs
from pw_console.log_line import LogLine
from pw_console.text_formatting import strip_ansi

if TYPE_CHECKING:
    from pw_console.log_pane import LogPane

_LOG = logging.getLogger(__package__)


class TableView:  # pylint: disable=too-many-instance-attributes
    """Store column information and render logs into formatted tables."""

    FLOAT_FORMAT = '%.3f'
    INT_FORMAT = '%s'
    LAST_TABLE_COLUMN_NAMES = ['msg', 'message']

    def __init__(self, prefs: ConsolePrefs, log_pane: 'LogPane'):
        self._header_fragment_cache: list[StyleAndTextTuples] = []

        # Max column sizes determined from logs.
        self.column_width_from_logs: collections.OrderedDict = (
            collections.OrderedDict()
        )
        # Set width defaults.
        self._default_time_width: int = 17
        self.column_width_from_logs['time'] = self._default_time_width
        self.column_width_from_logs['level'] = 3
        self._year_month_day_width: int = 9

        # List of all column names. This is updated as new logs come in. Used
        # for rendering column visibility checkboxes in the log pane menu.
        self.column_names: list[str] = []
        # Hidden column bool values.
        self.hidden_columns: dict[str, bool] = {}

        # Column resizing variables
        # Starting x coordinate for the separators between columns.
        self.drag_handle_x_coordinates: list[int] = []
        # name and +/- values for x coordinate drag distance.
        self.drag_diff_amounts: dict[str, int] = {}
        # Index and name of the selected column to resize.
        self.resize_column_index: int | None = None
        self.resize_column_name: str | None = None
        # Start x coordinate of a drag event
        self.resize_column_start_cursor_position: int | None = None
        # Ending x coordinate of a drag event
        self.resize_column_end_cursor_position: int | None = None
        # Final desired column width based on the default width +/- the
        # drag_diff_amounts.
        self.user_resized_width: dict[str, int] = {}

        # Set prefs last to override defaults.
        self.log_pane = log_pane
        self.prefs = prefs
        self.reset_prefs()

    def reset_prefs(self) -> None:
        column_spacing = max(1, self.prefs.spaces_between_columns)
        self.column_padding = ' ' * column_spacing
        self.column_padding_handle = '|' + (' ' * (column_spacing - 1))

        # Set columns hidden based on legacy hide column prefs.
        if not self.prefs.show_python_file:
            self.hidden_columns['py_file'] = True
        if not self.prefs.show_python_logger:
            self.hidden_columns['py_logger'] = True
        if not self.prefs.show_source_file:
            self.hidden_columns['file'] = True

        # Apply default visibility for any settings in the prefs.
        for column_name, is_visible in self.prefs.column_visibility(
            self.log_pane.startup_window_options
        ).items():
            is_hidden = not is_visible
            self.hidden_columns[column_name] = is_hidden

        self.reset_user_column_widths()

    def is_column_hidden(self, name: str) -> bool:
        return self.hidden_columns.get(name, False)

    def set_column_hidden(self, name: str, hidden: bool = True) -> None:
        self.hidden_columns[name] = hidden
        self.update_table_header()

    def all_column_names(self) -> Iterable[str]:
        yield from self.column_names

    def _ordered_column_widths(self) -> dict[str, int]:
        """Return each column and default width value in the preferred order."""
        if self.prefs.column_order(self.log_pane.startup_window_options):
            # Get ordered_columns
            columns = copy.copy(self.column_width_from_logs)
            ordered_columns = {}

            for column_name in self.prefs.column_order(
                self.log_pane.startup_window_options
            ):
                # If valid column name
                if column_name in columns:
                    ordered_columns[column_name] = columns.pop(column_name)

            # Add remaining columns unless user preference to hide them.
            if not self.prefs.omit_unspecified_columns:
                for column_name in columns:
                    ordered_columns[column_name] = columns[column_name]
        else:
            ordered_columns = copy.copy(self.column_width_from_logs)

        for column_name, is_hidden in self.hidden_columns.items():
            if is_hidden and column_name in ordered_columns:
                del ordered_columns[column_name]

        for column_name, width in self.user_resized_width.items():
            if column_name in ordered_columns:
                ordered_columns[column_name] = width

        return ordered_columns

    def set_column_to_resize(self, x_position: int) -> None:
        """Flag the column that a mouse drag will resize."""
        self.resize_column_index = None

        _LOG.warning(
            'drag_handle_x_coordinates %s', self.drag_handle_x_coordinates
        )

        # Determine which column is being resized.
        for i, position in enumerate(self.drag_handle_x_coordinates):
            if x_position >= position:
                self.resize_column_index = i

        if self.resize_column_index is None:
            return

        # Save the start x coordinate.
        self.resize_column_start_cursor_position = x_position

        # Get the visible column names
        visible_columns = self._ordered_column_widths()
        visible_column_names = list(name for name in visible_columns.keys())
        try:
            self.resize_column_name = visible_column_names[
                self.resize_column_index
            ]
        except IndexError as _error:
            _LOG.debug('INVALID index %s', self.resize_column_index)
            self.stop_column_resize()
            return

        _LOG.warning(
            'SET resize index = %s, name = %s',
            self.resize_column_index,
            self.resize_column_name,
        )

    def stop_column_resize(self) -> None:
        """Stop column resizing."""
        self.resize_column_index = None
        self.resize_column_name = None
        self.resize_column_start_cursor_position = None
        self.resize_column_end_cursor_position = None
        for key in self.drag_diff_amounts.keys():
            self.drag_diff_amounts[key] = 0
        _LOG.warning('CLEAR resize index')

    def set_column_resize_amount(self, x_position: int) -> None:
        """Update the user_resized_width based on a mouse drag event."""

        # Mouse down events are sometimes missed, so if no column has been
        # flagged yet, set it here. This should only run once per mouse drag.
        if self.resize_column_index is None:
            self.set_column_to_resize(x_position)
        if self.resize_column_index is None:
            _LOG.error('resize_column_index is None')
            return
        if self.resize_column_name is None:
            _LOG.error('resize_column_name is None')
            return

        _LOG.debug(
            'CHANGE resize amount %s -> %s',
            self.resize_column_end_cursor_position,
            x_position,
        )
        self.resize_column_end_cursor_position = x_position

        if not self.resize_column_start_cursor_position or (
            self.resize_column_end_cursor_position
            == self.resize_column_start_cursor_position
        ):
            # The mouse hasn't moved yet, return.
            return

        drag_amount = x_position - self.resize_column_start_cursor_position

        if drag_amount != 0:
            self.drag_diff_amounts[self.resize_column_name] = drag_amount
            # Move the end coordinate to the start for the next event.
            self.resize_column_start_cursor_position = (
                self.resize_column_end_cursor_position
            )
            _LOG.debug('  drag_diff %s', self.drag_diff_amounts)

            self._update_user_desired_width(
                drag_amount, self.resize_column_name
            )

        self.update_table_header()

    def _update_user_desired_width(self, drag_amount: int, name: str) -> None:
        default_width = self.column_width_from_logs.get(name, 5)
        width = self.user_resized_width.get(name, default_width)
        new_width = max(1, width + drag_amount)
        self.user_resized_width[name] = new_width
        _LOG.debug('  user_resized_width %s', self.user_resized_width)

    def reset_user_column_widths(self) -> None:
        # Set widths based on max values from logs.
        for name, width in self.column_width_from_logs.items():
            self.user_resized_width[name] = width

        # Override widths based on settings in prefs.
        for name, width in self.prefs.column_width(
            self.log_pane.startup_window_options
        ).items():
            self.user_resized_width[name] = width

        self.update_table_header()

    def update_column_widths_from_logs(
        self, new_column_widths: collections.OrderedDict
    ) -> None:
        """Calculate the max widths for each metadata field."""
        self.column_width_from_logs.update(new_column_widths)

        self.update_table_header()

    def update_table_header(self) -> None:
        """Redraw the table header."""
        self.column_names = [
            name
            for name, _width in self.column_width_from_logs.items()
            if name != 'msg'
        ] + ['message']

        default_style = 'bold'
        fragments: collections.deque = collections.deque()

        # Update time column width to current prefs setting
        self.column_width_from_logs['time'] = self._default_time_width
        if self.prefs.hide_date_from_log_time:
            self.column_width_from_logs['time'] = (
                self._default_time_width - self._year_month_day_width
            )

        self.drag_handle_x_coordinates = list()
        x_coordinate = 0
        ordered_column_widths = self._ordered_column_widths()
        for name, width in ordered_column_widths.items():
            # These fields will be shown at the end
            if name in TableView.LAST_TABLE_COLUMN_NAMES:
                continue

            fragments.append((default_style, name.title()[:width].ljust(width)))

            x_coordinate += width

            fragments.append(('', self.column_padding_handle))

            self.drag_handle_x_coordinates.append(x_coordinate)

            x_coordinate += self.prefs.spaces_between_columns

        fragments.append(
            (
                default_style,
                # Add extra space to allow mouse dragging. Without this
                # prompt_toolkit does not process mosue events.
                'Message'.ljust(100),
            )
        )

        self._header_fragment_cache = list(fragments)

    def formatted_header(self) -> list[StyleAndTextTuples]:
        """Get pre-formatted table header."""
        return self._header_fragment_cache

    def formatted_row(self, log: LogLine) -> StyleAndTextTuples:
        """Render a single table row."""
        # pylint: disable=too-many-locals
        padding_formatted_text = ('', self.column_padding)
        # Don't apply any background styling that would override the parent
        # window or selected-log-line style.
        default_style = ''

        table_fragments: StyleAndTextTuples = []

        # NOTE: To preseve ANSI formatting on log level use:
        # table_fragments.extend(
        #   ANSI(log.record.levelname.ljust(
        #     self.column_width_from_logs['level'])).__pt_formatted_text__())

        # Collect remaining columns to display after host time and level.
        columns: dict[str, str | tuple[str, str]] = {}
        ordered_column_widths = self._ordered_column_widths()
        for name, width in ordered_column_widths.items():
            # Skip these modifying these fields
            if name in TableView.LAST_TABLE_COLUMN_NAMES:
                continue

            # hasattr checks are performed here since a log record may not have
            # asctime or levelname if they are not included in the formatter
            # fmt string.
            if name == 'time' and hasattr(log.record, 'asctime'):
                time_text = log.record.asctime
                if self.prefs.hide_date_from_log_time:
                    time_text = time_text[self._year_month_day_width :]
                time_style = self.prefs.column_style(
                    'time',
                    time_text,
                    default='class:log-time',
                    window_config=self.log_pane.startup_window_options,
                )
                columns['time'] = (
                    time_style,
                    time_text[:width].ljust(width),
                )
                continue

            if name == 'level' and hasattr(log.record, 'levelname'):
                # Remove any existing ANSI formatting and apply our colors.
                level_text = strip_ansi(log.record.levelname)
                level_style = self.prefs.column_style(
                    'level',
                    level_text,
                    default='class:log-level-{}'.format(log.record.levelno),
                    window_config=self.log_pane.startup_window_options,
                )
                columns['level'] = (
                    level_style,
                    level_text[:width].ljust(width),
                )
                continue

            value = ' '
            # If fields are populated, grab the metadata column.
            if hasattr(log.metadata, 'fields'):
                value = log.metadata.fields.get(name, ' ')
            if value is None:
                value = ' '
            left_justify = True

            # Right justify and format numbers
            if isinstance(value, float):
                value = TableView.FLOAT_FORMAT % value
                left_justify = False
            elif isinstance(value, int):
                value = TableView.INT_FORMAT % value
                left_justify = False

            if left_justify:
                columns[name] = value[:width].ljust(width)
            else:
                columns[name] = value[:width].rjust(width)

        # Grab the message to appear after the justified columns.
        # Default to the Python log message
        message_text = log.record.message.rstrip()

        # If fields are populated, grab the msg field.
        if hasattr(log.metadata, 'fields'):
            message_text = log.metadata.fields.get('msg', message_text)
        ansi_stripped_message_text = strip_ansi(message_text)

        # Add to columns for width calculations with ansi sequences removed.
        columns['message'] = ansi_stripped_message_text

        index_modifier = 0
        # Go through columns and convert to FormattedText where needed.
        for i, column in enumerate(columns.items()):
            column_name, column_value = column

            # Skip the message column in this loop.
            if column_name == 'message':
                continue

            if i in [0, 1] and column_name in ['time', 'level']:
                index_modifier -= 1
            # For raw strings that don't have their own ANSI colors, apply the
            # theme color style for this column.
            if isinstance(column_value, str):
                fallback_style = (
                    'class:log-table-column-{}'.format(i + index_modifier)
                    if 0 <= i <= 7
                    else default_style
                )

                style = self.prefs.column_style(
                    column_name,
                    column_value.rstrip(),
                    default=fallback_style,
                    window_config=self.log_pane.startup_window_options,
                )

                table_fragments.append((style, column_value))
                table_fragments.append(padding_formatted_text)
            # Add this tuple to table_fragments.
            elif isinstance(column, tuple):
                table_fragments.append(column_value)
                # Add padding if not the last column.
                if i < len(columns) - 1:
                    table_fragments.append(padding_formatted_text)

        # Handle message column.
        if self.prefs.recolor_log_lines_to_match_level:
            message_style = default_style
            if log.record.levelno >= 30:  # Warning, Error and Critical
                # Style the whole message to match the level
                message_style = 'class:log-level-{}'.format(log.record.levelno)

            message: OneStyleAndTextTuple = (
                message_style,
                ansi_stripped_message_text,
            )
            table_fragments.append(message)
        else:
            # Format the message preserving any ANSI color sequences.
            message_fragments = ANSI(message_text).__pt_formatted_text__()
            table_fragments.extend(message_fragments)

        # Add the final new line for this row.
        table_fragments.append(('', '\n'))
        return table_fragments
