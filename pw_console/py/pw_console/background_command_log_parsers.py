# Copyright 2025 The Pigweed Authors
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
"""Background command log parsing classes."""

import abc
from datetime import datetime
import functools
import json
import logging
import re
from typing import Any, Callable

from pw_console.text_formatting import strip_incompatible_ansi


_LOG = logging.getLogger(__package__)


def _get_nested_value(d: dict, *keys: Any, default: Any = None) -> Any:
    """Get a deeply nested dict value.

    Returns default if any key in the chain is missing.
    """
    result = default
    try:
        result = functools.reduce(dict.__getitem__, keys, d)
    except (KeyError, TypeError):
        pass
    return result


class BackgroundCommandLogParser(abc.ABC):
    """Base class for background command log parsers."""

    @abc.abstractmethod
    def process_log(self, logger: logging.Logger, line: str) -> None:
        """Convert a background command line into a Python log.

        This function should call logger.log."""


class FuchsiaJsonLogParser(BackgroundCommandLogParser):
    """Fuchsia log parser for json output."""

    LOG_LEVELS = {
        "DEBUG": logging.DEBUG,
        "INFO": logging.INFO,
        "WARN": logging.WARNING,
        "ERROR": logging.ERROR,
    }

    def __init__(self) -> None:
        self.boot_timestamp: float | None = None

    def process_log(self, logger: logging.Logger, line: str) -> None:
        def _fallback_log() -> None:
            logger.info(line)

        try:
            log = json.loads(line)
        except json.decoder.JSONDecodeError:
            # Fall back to logging the raw line.
            _fallback_log()
            return

        # Check for a current_boot_timestamp value. This usually appears on the
        # first log message.
        boot_timestamp_ns = _get_nested_value(
            log,
            'data',
            'TargetLog',
            'payload',
            'root',
            'keys',
            'current_boot_timestamp',
        )
        if boot_timestamp_ns:
            boot_timestamp = boot_timestamp_ns / 1_000_000_000
            # Save for later log processing.
            self.boot_timestamp = boot_timestamp

        level_string = _get_nested_value(
            log, 'data', 'TargetLog', 'metadata', 'severity', default='INFO'
        )
        level = FuchsiaJsonLogParser.LOG_LEVELS.get(level_string, logging.INFO)
        message = _get_nested_value(
            log, 'data', 'TargetLog', 'payload', 'root', 'message', 'value'
        )

        # Check for None here instead of truthy. The json may have an empty
        # string for the message. For example:
        #   "payload":{"root":{"message":{"value":""}}}
        if message is None:
            _fallback_log()
            return

        fields: dict[str, str | int | float | None] = {}
        fields['msg'] = message.rstrip()

        # Check for a timestamp.
        timestamp_ns: int | None = _get_nested_value(
            log, 'data', 'TargetLog', 'metadata', 'timestamp'
        )
        # Set timestamp to the found value or None.
        fields['timestamp'] = timestamp_ns
        if timestamp_ns:
            # If available save the seconds since boot.
            seconds_since_boot = timestamp_ns / 1_000_000_000
            fields['seconds'] = seconds_since_boot

            # Repopulate timestamp with UTC time.
            if self.boot_timestamp:
                fields['timestamp'] = datetime.utcfromtimestamp(
                    self.boot_timestamp + seconds_since_boot
                ).isoformat(sep=' ')

        tag_list = _get_nested_value(
            log, 'data', 'TargetLog', 'metadata', 'tags', default=[]
        )
        fields['tags'] = ', '.join(tag_list)
        pid = _get_nested_value(log, 'data', 'TargetLog', 'metadata', 'pid')
        tid = _get_nested_value(log, 'data', 'TargetLog', 'metadata', 'tid')
        if pid:
            fields['pid'] = pid
        if tid:
            fields['tid'] = tid

        moniker = _get_nested_value(log, 'data', 'TargetLog', 'moniker')
        if moniker:
            fields['moniker'] = moniker

        component_url = _get_nested_value(
            log, 'data', 'TargetLog', 'metadata', 'component_url'
        )
        if component_url:
            fields['component_url'] = component_url

        file_name = _get_nested_value(
            log, 'data', 'TargetLog', 'metadata', 'file'
        )
        file_line = _get_nested_value(
            log, 'data', 'TargetLog', 'metadata', 'line'
        )
        if file_name and file_line:
            fields['file'] = f'{file_name}:{file_line}'

        logger.log(
            level,
            '[%s][%s] %s: %s',
            fields['timestamp'],
            ','.join(tag_list),
            level_string,
            message,
            extra=dict(extra_metadata_fields=fields),
        )


class AndroidLogcatParser(BackgroundCommandLogParser):
    """An adb logcat text output parser.

    Works with 'adb logcat --format threadtime' (the default output).
    """

    def __init__(self, enable_user: bool = False) -> None:
        self.log_levels = {
            'D': logging.DEBUG,
            'I': logging.INFO,
            'W': logging.WARNING,
            'E': logging.ERROR,
        }

        line_regex = (
            # Timestamp capture group
            r'(?P<timestamp>'
            # Month-day
            r'[0-9]{2}-[0-9]{2} '
            # Hour:minute:seconds
            r'[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]+'
            r')'
            r'\s*'
        )
        if enable_user:
            line_regex += (
                # User
                r'(?P<user>\w+)'
                r'\s*'
            )
        line_regex += (
            # Process ID
            r'(?P<pid>[0-9]+)'
            r'\s*'
            # Thread ID
            r'(?P<tid>[0-9]+)'
            r'\s*'
            # Single letter log level.
            r'(?P<log_level_letter>\w)'
            r'\s*'
            # Optionally capture the tag. Tags seem to allow:
            #   Alphanumeric characters
            #   These symbols . / -
            # Followed optionally by 0 or more spaces and a colon character
            r'(?:'
            r'(?P<tag>[\[\]a-zA-Z0-9-_/.]+) *: '
            r')?'
            # The rest is the message.
            r'(?P<msg>.*)'
        )

        self.line_regex = re.compile(line_regex)

    def process_log(self, logger: logging.Logger, line: str) -> None:
        matches = self.line_regex.match(line)
        if matches is None:
            logger.info(line)
            return

        fields = matches.groupdict()
        level = self.log_levels.get(fields['log_level_letter'], logging.DEBUG)
        del fields["log_level_letter"]

        logger.log(
            level,
            # Log the original line for when table mode is off.
            line,
            extra=dict(extra_metadata_fields=fields),
        )


class AndroidPersistentLogcatParser(AndroidLogcatParser):
    """An adb logcat text parser for viewing persistent logcat files.

    Works with 'adb logcat --format threadtime --format uid'
    """

    def __init__(self) -> None:
        super().__init__(enable_user=True)


class BasicLogParser(BackgroundCommandLogParser):
    """A basic log parser that passes line output as-is."""

    def process_log(self, logger: logging.Logger, line: str) -> None:
        logger.log(logging.INFO, strip_incompatible_ansi(line).rstrip())


COMMAND_LOG_PARSING_CLASSES = {
    'basic': BasicLogParser,
    'fuchsia-json': FuchsiaJsonLogParser,
    'android-logcat-text': AndroidLogcatParser,
    'android-persistent-logcat-text': AndroidPersistentLogcatParser,
}


def get_command_log_parser(
    name: str,
) -> Callable[[logging.Logger, str], None] | None:
    parser_class = COMMAND_LOG_PARSING_CLASSES.get(name, None)
    if not parser_class:
        return None

    log_parser = parser_class()
    return log_parser.process_log
