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
"""Pigweed Console built-in tools.

Launch an empty UI with only a Python repl:

.. code-block:: shell

   pw-console

Launch the test mode user interface:

.. code-block:: shell

   pw-console --test-mode

Open files for viewing in log windows:

.. code-block:: shell

   pw-console --open-files logfile1.txt logfile2.txt

Open an Android bugreport zipfile and view persistent logcat files:

.. code-block:: shell

   pw-console --open-bugreport android-bugreport.zip
"""

import argparse
import inspect
import logging
from pathlib import Path
import sys

from pw_cli import log as pw_cli_log
from pw_cli import argument_types
from pw_console import PwConsoleEmbed
from pw_console.background_command_log_parsers import (
    COMMAND_LOG_PARSING_CLASSES,
)
from pw_console.log_file_loader import LogFileLoader
from pw_console.log_store import LogStore
from pw_console.plugins.calc_pane import CalcPane
from pw_console.plugins.clock_pane import ClockPane
from pw_console.plugins.twenty48_pane import Twenty48Pane
from pw_console.python_logging import create_temp_log_file
from pw_console.test_mode import FAKE_DEVICE_LOGGER_NAME
from pw_console.web import PwConsoleWeb

_LOG = logging.getLogger(__package__)
_ROOT_LOG = logging.getLogger('')


def _build_argument_parser() -> argparse.ArgumentParser:
    """Setup argparse."""
    parser = argparse.ArgumentParser(
        prog='python -m pw_console',
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )

    parser.add_argument(
        '-l',
        '--loglevel',
        type=argument_types.log_level,
        default=logging.DEBUG,
        help='Set the log level' '(debug, info, warning, error, critical)',
    )

    parser.add_argument('--logfile', help='Pigweed Console log file.')

    extra_modes = parser.add_mutually_exclusive_group()
    extra_modes.add_argument(
        '--test-mode',
        action='store_true',
        help='Enable fake log messages for testing purposes.',
    )
    extra_modes.add_argument(
        '--open-files',
        nargs='+',
        action='extend',
        type=Path,
        help='Paths to text log file to open.',
    )
    extra_modes.add_argument(
        '--open-bugreport',
        type=Path,
        help=(
            'Open an Android bugreport zip file and view persistent logcat '
            'file contents. This option makes use of preset values for '
            '--file-parser, --zipfile-globs, --reverse-file-order and '
            '--merge-open-files. Any additional passed in values are ignored.'
        ),
    )

    file_options_group = parser.add_argument_group(title='Open File Options')
    file_options_group.add_argument(
        '--file-parser',
        choices=COMMAND_LOG_PARSING_CLASSES.keys(),
        default='basic',
    )
    file_options_group.add_argument(
        '--merge-open-files',
        action='store_true',
        help=(
            'Multiple open-file arguments will be merged into a single '
            'continuous view in the order they are specified. '
            'Otherwise each file will be shown in a separate tab.'
        ),
    )
    file_options_group.add_argument(
        '--zipfile-globs',
        nargs='+',
        type=str,
        help='Globs to match when opening zipfile contents.',
    )
    file_options_group.add_argument(
        '--reverse-file-order',
        action='store_true',
        help=(
            'When opening multiple files reverse the order in which they '
            'appear. Normally they will appear in ascending order of filename.'
        ),
    )

    parser.add_argument(
        '--config-file',
        type=Path,
        help='Path to a pw_console yaml config file.',
    )
    parser.add_argument(
        '--console-debug-log-file',
        help='Log file to send console debug messages to.',
    )
    parser.add_argument(
        '--browser',
        action='store_true',
        help='Start browser-based console instead of terminal.',
    )

    return parser


def main(args: argparse.Namespace | None = None) -> int:
    """Pigweed Console."""

    parser = _build_argument_parser()

    if args is None:
        args = parser.parse_args()

    user_specified_logfile = False
    if args.logfile:
        user_specified_logfile = True
    else:
        # Create a temp logfile to prevent logs from appearing over stdout. This
        # would corrupt the prompt toolkit UI.
        args.logfile = create_temp_log_file()

    pw_cli_log.install(
        level=args.loglevel,
        use_color=True,
        hide_timestamp=False,
        log_file=args.logfile,
    )

    if args.console_debug_log_file:
        pw_cli_log.install(
            level=logging.DEBUG,
            use_color=True,
            hide_timestamp=False,
            log_file=args.console_debug_log_file,
            logger=logging.getLogger('pw_console'),
        )

    global_vars = None
    default_loggers = {}
    if args.test_mode:
        root_log_store = LogStore()
        _ROOT_LOG.addHandler(root_log_store)
        _ROOT_LOG.debug('pw_console test-mode starting...')

        fake_logger = logging.getLogger(FAKE_DEVICE_LOGGER_NAME)
        default_loggers = {
            # Don't include pw_console package logs (_LOG) in the log pane UI.
            # Add the fake logger for test_mode.
            'Fake Device': [fake_logger],
            'PwConsole Debug': [logging.getLogger('pw_console')],
            'All Logs': root_log_store,
        }
        # Give access to adding log messages from the repl via: `LOG.warning()`
        global_vars = dict(LOG=fake_logger)

    help_text = None
    app_title = None
    if args.test_mode:
        app_title = 'Console Test Mode'
        help_text = inspect.cleandoc(
            """
            Welcome to the Pigweed Console Test Mode!

            Example commands:

              rpcs.pw.rpc.EchoService.Echo(msg='hello!')

              LOG.warning('Message appears console log window.')
        """
        )

    overridden_window_config: dict | None = None

    if args.open_bugreport:
        args.open_files = [args.open_bugreport]
        args.file_parser = 'android-persistent-logcat-text'
        args.merge_open_files = True
        args.zipfile_globs = ['FS/data/misc/logd/logcat*']
        args.reverse_file_order = True

    if args.open_files:
        loader = LogFileLoader(
            files=args.open_files,
            log_parser=args.file_parser,
            merge_files=args.merge_open_files,
            zip_globs=args.zipfile_globs,
            reverse_file_order=args.reverse_file_order,
        )
        loader.load_files()
        default_loggers.update(loader.default_loggers())
        overridden_window_config = loader.window_config()

    if args.browser:
        loggers = {
            'Fake Device': [fake_logger],
            'PwConsole Debug': [logging.getLogger('pw_console')],
            'All Logs': [_ROOT_LOG],
        }
        webserver = PwConsoleWeb(
            global_vars=global_vars,
            loggers=loggers,
            test_mode=args.test_mode,
        )
        webserver.start()
    else:
        console = PwConsoleEmbed(
            global_vars=global_vars,
            loggers=default_loggers,
            test_mode=args.test_mode,
            help_text=help_text,
            app_title=app_title,
            config_file_path=args.config_file,
        )

        # Add example plugins and log panes used to validate behavior in the
        # Pigweed Console manual test procedure:
        # https://pigweed.dev/pw_console/testing.html
        if args.test_mode:
            fake_logger.propagate = False
            console.setup_python_logging(
                loggers_with_no_propagation=[fake_logger]
            )

            _ROOT_LOG.debug('pw_console.PwConsoleEmbed init complete')
            _ROOT_LOG.debug('Adding plugins...')
            console.add_window_plugin(ClockPane())
            console.add_window_plugin(CalcPane())
            console.add_floating_window_plugin(
                Twenty48Pane(include_resize_handle=False), left=4
            )
            _ROOT_LOG.debug(
                'Starting prompt_toolkit full-screen application...'
            )

            overridden_window_config = {
                'Group 1 stacked': {
                    'Fake Device': {
                        'column_colors': {
                            'module': {
                                'default': '#fe4450',
                                'BAT': '#2ee2fa',
                                'RADIO': '#fede5d',
                                'CPU': '#ff7edb',
                                'APP': '#72f1b8 bold',
                                'USB': '#03edf9 bold',
                            }
                        },
                        'column_width': {
                            'timestamp': 9,
                            'module': 6,
                        },
                    },
                    'Fake Keys': {
                        'duplicate_of': 'Fake Device',
                        'filters': {
                            'keys': {'regex': '[^ ]+'},
                        },
                        'table_mode': True,
                        'wrap_lines': True,
                        'column_order': [
                            'time',
                            'keys',
                            'level',
                        ],
                        'column_visibility': {
                            'timestamp': False,
                            'module': False,
                        },
                        'column_width': {
                            'level': 6,
                        },
                    },
                    'Fake USB': {
                        'duplicate_of': "Fake Device",
                        'filters': {
                            'module': {'regex': 'USB'},
                        },
                        'column_colors': {
                            'module': {
                                'default': '',
                                'USB': '#03edf9',
                            }
                        },
                        'column_width': {
                            'timestamp': 9,
                            'module': 6,
                        },
                    },
                },
                'Group 2 tabbed': {
                    'Python Repl': None,
                    'All Logs': None,
                    'PwConsole Debug': None,
                    'Calculator': None,
                    'Clock': None,
                },
            }

        console.embed(override_window_config=overridden_window_config)

    if user_specified_logfile:
        print(f'Logs saved to: {args.logfile}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
