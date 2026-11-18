#!/usr/bin/env python
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
"""``pw_watch.run`` executes arbitrary commands when watched files change.

One or more commands to run are provided on the command line. These are executed
in order when changes are detected.

Examples:

.. code-block:: sh

   # Run `bazelisk --symlink_prefix=/ build //...`
   run.py --prefix bazelisk --symlink_prefix=/ --pw-watch-commands build //...

Multiple commands may be specified, separated by ``,``:

.. code-block:: sh

   # Run `cowsay` then `cowthink` when watched files change.
   run.py cowsay "Hey, how are you?" , cowthink Not in moood to talk

The Bazel build's ``//:watch`` watch entrypoint invokes ``//pw_watch/py:run``
with ``bazelisk`` as the command prefix.
"""

import argparse
import errno
import logging
import os
from pathlib import Path
import shlex
import subprocess
import sys
import threading
import time
from typing import Iterable, Sequence

from watchdog.events import FileSystemEventHandler

import pw_cli.color
import pw_cli.env
import pw_cli.log
from pw_cli.plural import plural

from pw_watch import common
from pw_watch.debounce import DebouncedFunction, Debouncer

_LOG = logging.getLogger('pw_watch')
_COLOR = pw_cli.color.colors()

_WIDTH = 80
_STEP_START = '━' * (_WIDTH - 1) + '┓'
_STEP_FINISH = '━' * (_WIDTH - 1) + '┛'


def _format_time(time_s: float) -> str:
    minutes, seconds = divmod(time_s, 60)
    if minutes < 60:
        return f' {int(minutes)}:{seconds:04.1f}'
    hours, minutes = divmod(minutes, 60)
    return f'{int(hours):d}:{int(minutes):02}:{int(seconds):02}'


class Watcher(FileSystemEventHandler, DebouncedFunction):
    """Process filesystem events and run commands on changes."""

    def __init__(
        self,
        commands: Iterable[Sequence[str]],
        *,
        patterns: Iterable[str] = (),
        ignore_patterns: Iterable[str] = (),
        keep_going: bool = False,
        clear_screen: bool = True,
    ) -> None:
        super().__init__()

        self.commands = tuple(tuple(cmd) for cmd in commands)
        self.patterns = patterns
        self.ignore_patterns = ignore_patterns
        self.keep_going = keep_going
        self.clear_screen = clear_screen

        self._debouncer = Debouncer(self)
        threading.Thread(None, self._wait_for_enter).start()

    def trigger_run(self, message: str) -> None:
        self._debouncer.press(message)

    def _wait_for_enter(self) -> None:
        try:
            while True:
                _ = input()
                self.trigger_run('Manual run requested')
        # Ctrl-C on Unix generates KeyboardInterrupt
        # Ctrl-Z on Windows generates EOFError
        except (KeyboardInterrupt, EOFError):
            _exit_due_to_interrupt()

    def dispatch(self, event) -> None:
        path = common.handle_watchdog_event(
            event, self.patterns, self.ignore_patterns
        )
        if path is not None:
            self.trigger_run(
                f'File change detected: {os.path.relpath(path)}; '
                'Triggering build...'
            )

    # Implementation of DebouncedFunction.run()
    #
    # Note: This will run on the timer thread created by the Debouncer, rather
    # than on the main thread that's watching file events. This enables the
    # watcher to continue receiving file change events during a build.
    def run(self) -> None:
        if self.clear_screen:  # Conditionally clear the screen
            print('\033c', end='', flush=True)

        for i, command in enumerate(self.commands, 1):
            count = f' {i}/{len(self.commands)}   '
            print(
                f'{_STEP_START}\n{count}{shlex.join(command)}\n',
                flush=True,
            )
            start = time.time()
            code = subprocess.run(command).returncode
            total_time = time.time() - start

            if code == 0:
                result = _COLOR.bold_green('PASSED')
                msg = ''
            else:
                result = _COLOR.bold_red('FAILED')
                msg = f' with exit code {code}'

            remaining_width = _WIDTH - len(count) - 6 - len(msg) - 2
            timestamp = _format_time(total_time).rjust(remaining_width)
            print(
                f'\n{count}{result}{msg}{timestamp}\n{_STEP_FINISH}\n',
                flush=True,
            )

            if code and not self.keep_going and i < len(self.commands):
                _LOG.info(
                    'Skipping %d remaining %s; '
                    'run with --keep-going to continue on errors',
                    len(self.commands) - i,
                    plural(len(self.commands) - i, 'command'),
                )
                break

    def cancel(self) -> bool:
        """No-op implementation of DebouncedFunction.cancel()."""
        return False

    def on_complete(self, cancelled: bool = False) -> None:
        """No-op implementation of DebouncedFunction.on_complete()."""

    def on_keyboard_interrupt(self) -> None:
        """Implementation of DebouncedFunction.on_keyboard_interrupt()."""
        _exit_due_to_interrupt()


def _exit_due_to_interrupt() -> None:
    # To keep the log lines aligned with each other in the presence of
    # a '^C' from the keyboard interrupt, add a newline before the log.
    print('')
    _LOG.info('Got Ctrl-C; exiting...')
    common.exit_immediately(1)


def watch_setup(
    root: Path,
    keep_going: bool,
    commands: Sequence[tuple[str, ...]],
    clear: bool,
    watch_patterns: Sequence[str] = common.WATCH_PATTERNS,
    ignore_patterns: Sequence[str] = (),
    exclude_dirs: Sequence[Path] | None = None,
) -> tuple[Watcher, Sequence[Path]]:
    """Returns a Watcher and list of directories to ignore."""
    os.chdir(root)

    if exclude_dirs is None:
        excludes = tuple(common.get_common_excludes(root))
    else:
        excludes = tuple(exclude_dirs)

    event_handler = Watcher(
        commands,
        patterns=watch_patterns,
        ignore_patterns=ignore_patterns,
        keep_going=keep_going,
        clear_screen=clear,
    )
    return event_handler, excludes


def watch(
    event_handler: Watcher,
    exclude_dirs: Sequence[Path],
    watch_path: Path,
) -> None:
    """Watches files and runs commands when they change."""
    try:
        event_handler.trigger_run('Triggering initial run...')
        wait = common.watch(watch_path, exclude_dirs, event_handler)
        wait()
    # Ctrl-C on Unix generates KeyboardInterrupt
    # Ctrl-Z on Windows generates EOFError
    except (KeyboardInterrupt, EOFError):
        _exit_due_to_interrupt()
    except OSError as err:
        if err.args[0] == common.ERRNO_INOTIFY_LIMIT_REACHED:
            common.log_inotify_watch_limit_reached()
            common.exit_immediately(1)
        if err.errno == errno.EMFILE:
            common.log_inotify_instance_limit_reached()
            common.exit_immediately(1)
        raise err


_PREFIX_ARG = '--prefix'
_COMMANDS_ARG = '--pw-watch-commands'
_CMD_DELIMITER = ','


def _parse_args() -> argparse.Namespace:
    # Remove reST / Sphinx syntax from the docstring for use in --help.
    help_string = __doc__.replace('``', '`').replace('.. code-block:: sh\n', '')
    parser = argparse.ArgumentParser(
        description=help_string,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )

    parser.add_argument(
        '--root',
        type=Path,
        default=pw_cli.env.project_root(),
        help='Directory from which to execute commands',
    )
    parser.add_argument(
        '-k',
        '--keep-going',
        action=argparse.BooleanOptionalAction,
        default=False,
        help='Continue executing commands after errors',
    )
    parser.add_argument(
        '--clear',
        action=argparse.BooleanOptionalAction,
        default=True,
        help='Clear the screen before running commands',
    )
    parser.add_argument(
        _PREFIX_ARG,
        dest='prefix',
        metavar='PREFIX...',
        help=(
            'Prefix to apply to all commands; consumes all arguments, '
            f'including -- args, until the mandatory {_COMMANDS_ARG} argument; '
            f'so must be the final argument before {_COMMANDS_ARG}'
        ),
    )
    parser.add_argument(
        _COMMANDS_ARG,
        dest='commands',
        metavar=f'COMMAND... [{_CMD_DELIMITER} COMMAND...]',
        required=True,
        help=(
            'Command to run; may specify multiple commands separated by '
            f'{_CMD_DELIMITER} (e.g. /bin/cmd1 --arg1 {_CMD_DELIMITER} '
            '/bin/cmd_2 -a2)'
        ),
    )
    parser.add_argument(
        'commands', nargs=argparse.REMAINDER, help='Command to execute'
    )

    # Parse the prefix and command arguments manually, since argparse doesn't
    # support capturing -- arguments in other arguments.
    args = sys.argv[1:]

    try:
        first_cmd_index = args.index(_COMMANDS_ARG)
        raw_commands = args[first_cmd_index + 1 :]
        del args[first_cmd_index:]  # already parsed, remove it from argparse
    except ValueError:
        first_cmd_index = None

    prefix: tuple[str, ...] = ()
    try:
        prefix_index = args.index(_PREFIX_ARG)
        if first_cmd_index is not None and prefix_index < first_cmd_index:
            prefix = tuple(args[prefix_index + 1 : first_cmd_index])
        del args[prefix_index:]  # already parsed, remove it from argparse
    except ValueError:
        pass

    # Parse remaining args, using an empty stand-in for the commands.
    parsed = parser.parse_args(args + [_COMMANDS_ARG, ''])

    if first_cmd_index is None:
        parser.error(f'{_COMMANDS_ARG} must be specified as the final argument')
    if not raw_commands:
        print('No commands were specified!', file=sys.stderr)
        print('\nSpecify a command to run when files change.', file=sys.stderr)

        if prefix:
            print(
                f'\nCommands execute with the prefix "{shlex.join(prefix)}", ',
                file=sys.stderr,
            )
            print('so passing "test //foo" will execute:', file=sys.stderr)
            print(f'\n  {shlex.join(prefix)} test //foo\n', file=sys.stderr)

        sys.exit(1)

    # Account for the arguments that were manually parsed.
    parsed.commands = tuple(_parse_commands(prefix, raw_commands))
    delattr(parsed, 'prefix')
    return parsed


def _parse_commands(
    prefix: Sequence[str], raw_commands: Sequence[str]
) -> Iterable[tuple[str, ...]]:
    start = 0
    while start < len(raw_commands):
        try:
            end = raw_commands.index(_CMD_DELIMITER, start)
        except ValueError:
            end = len(raw_commands)

        yield (*prefix, *raw_commands[start:end])
        start = end + 1


def main() -> int:
    """Watch files for changes and run commands."""
    pw_cli.log.install(level=logging.INFO, use_color=True, hide_timestamp=False)

    parsed_args = _parse_args()
    event_handler, exclude_dirs = watch_setup(**vars(parsed_args))
    watch(event_handler, exclude_dirs, parsed_args.root)

    return 0


if __name__ == '__main__':
    main()
