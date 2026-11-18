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
"""Console for interacting with devices using HDLC.

To start the console, provide a serial port as the --device argument and paths
or globs for .proto files that define the RPC services to support:

  python -m pw_system.console --device /dev/ttyUSB0 --proto-globs pw_rpc/echo.proto

This starts an interactive console for communicating with the connected device.
A few variables are predefined in the console. These include:

    rpcs   - used to invoke RPCs
    device - the serial device used for communication
    client - the pw_rpc.Client
    protos - protocol buffer messages indexed by proto package

An example echo RPC command:

  rpcs.pw.rpc.EchoService.Echo(msg="hello!")
"""  # pylint: disable=line-too-long

import argparse
from inspect import cleandoc
import logging
from pathlib import Path
import sys
from types import ModuleType
from typing import (
    Any,
    Callable,
    Collection,
    Sequence,
    TYPE_CHECKING,
)

import debugpy  # type: ignore

from pw_cli import log as pw_cli_log
from pw_console import embed
from pw_console import web
from pw_console import log_store
from pw_console.plugins import bandwidth_toolbar
from pw_console import python_logging
from pw_hdlc import rpc
from pw_rpc.console_tools.console import flattened_rpc_completions

from pw_system.device_connection import (
    DeviceConnection,
    add_device_args,
    create_device_serial_or_socket_connection,
)

if TYPE_CHECKING:
    from pw_system import device as pw_device
    from pw_system import device_tracing as pw_device_tracing

_LOG = logging.getLogger(__package__)
_DEVICE_LOG = logging.getLogger('rpc_device')
_SERIAL_DEBUG = logging.getLogger('pw_console.serial_debug_logger')
_ROOT_LOG = logging.getLogger()

MKFIFO_MODE = 0o666


def add_logfile_args(
    parser: argparse.ArgumentParser,
) -> argparse.ArgumentParser:
    """Add logfile specific arguments needed by the interactive console."""

    def log_level(arg: str) -> int:
        try:
            return getattr(logging, arg.upper())
        except AttributeError:
            raise argparse.ArgumentTypeError(
                f'{arg.upper()} is not a valid log level'
            )

    parser.add_argument(
        '-v',
        '--verbose',
        action='store_true',
        help='Set the host log level to debug.',
    )
    parser.add_argument(
        '--host-log-level',
        type=log_level,
        default=logging.INFO,
        help='Set the host log level (debug, info, warning, error, critical)',
    )
    parser.add_argument(
        '--device-log-level',
        type=log_level,
        default=logging.DEBUG,
        help='Set the device log level (debug, info, warning, error, critical)',
    )
    parser.add_argument(
        '--json-logfile', help='Device only JSON formatted log file.'
    )
    parser.add_argument(
        '--logfile',
        default='pw_console-logs.txt',
        help=(
            'Default log file. This will contain host side '
            'log messages only unles the '
            '--merge-device-and-host-logs argument is used.'
        ),
    )
    parser.add_argument(
        '--merge-device-and-host-logs',
        action='store_true',
        help=(
            'Include device logs in the default --logfile.'
            'These are normally shown in a separate device '
            'only log file.'
        ),
    )
    parser.add_argument(
        '--host-logfile',
        help=(
            'Additional host only log file. Normally all logs in the '
            'default logfile are host only.'
        ),
    )
    parser.add_argument(
        '--device-logfile',
        default='pw_console-device-logs.txt',
        help='Device only log file.',
    )
    parser.add_argument(
        '--debugger-listen',
        action=argparse.BooleanOptionalAction,
        default=False,
        help='Start the debugpy listener.',
    )
    parser.add_argument(
        '--debugger-wait-for-client',
        action=argparse.BooleanOptionalAction,
        default=False,
        help='Pause console start until a debugger connects.',
    )

    return parser


def add_pw_console_args(
    parser: argparse.ArgumentParser,
) -> argparse.ArgumentParser:
    """Add pw_console startup specific arguments."""
    parser.add_argument(
        '--config-file',
        type=Path,
        help='Path to a pw_console yaml config file.',
    )

    parser.add_argument(
        "--browser",
        action="store_true",
        help="Start browser-based console instead of terminal.",
    )
    return parser


def get_parser() -> argparse.ArgumentParser:
    """Build pw_system.console argparse with both device and logfile args."""
    parser = argparse.ArgumentParser(
        prog='python -m pw_system.console',
        description=__doc__,
    )
    parser = add_device_args(parser)
    parser = add_logfile_args(parser)
    parser = add_pw_console_args(parser)
    return parser


def _parse_args(args: argparse.Namespace | None = None):
    """Parses and returns the command line arguments."""
    if args is not None:
        return args

    parser = get_parser()
    return parser.parse_args()


def _start_python_terminal(  # pylint: disable=too-many-arguments
    device: 'pw_device.Device',
    device_log_store: log_store.LogStore,
    root_log_store: log_store.LogStore,
    serial_debug_log_store: log_store.LogStore,
    log_file: str,
    host_logfile: str,
    device_logfile: str,
    json_logfile: str,
    serial_debug: bool = False,
    config_file_path: Path | None = None,
    browser: bool = False,
) -> None:
    """Starts an interactive Python terminal with preset variables."""
    local_variables = dict(
        client=device.client,
        device=device,
        rpcs=device.rpcs,
        protos=device.client.protos.packages,
        # Include the active pane logger for creating logs in the repl.
        DEVICE_LOG=_DEVICE_LOG,
        LOG=logging.getLogger(),
    )

    welcome_message = cleandoc(
        """
        Welcome to the Pigweed Console!

        Help: Press F1 or click the [Help] menu
        To move focus: Press Shift-Tab or click on a window

        Example Python commands:

          device.rpcs.pw.rpc.EchoService.Echo(msg='hello!')
          LOG.warning('Message appears in Host Logs window.')
          DEVICE_LOG.warning('Message appears in Device Logs window.')
    """
    )

    welcome_message += '\n\nLogs are being saved to:\n  ' + log_file
    if host_logfile:
        welcome_message += '\nHost logs are being saved to:\n  ' + host_logfile
    if device_logfile:
        welcome_message += (
            '\nDevice logs are being saved to:\n  ' + device_logfile
        )
    if json_logfile:
        welcome_message += (
            '\nJSON device logs are being saved to:\n  ' + json_logfile
        )

    client_info = device.info()
    completions = flattened_rpc_completions([client_info])

    log_windows: dict[str, list[logging.Logger] | log_store.LogStore] = {
        'Device Logs': device_log_store,
        'Host Logs': root_log_store,
    }
    if serial_debug:
        log_windows['Serial Debug'] = serial_debug_log_store

    if browser:
        loggers: dict[str, list[logging.Logger]] = {
            "Device Logs": [_DEVICE_LOG],
            "Host Logs": [_ROOT_LOG],
        }
        if serial_debug:
            loggers['Serial Debug'] = [_SERIAL_DEBUG]

        webserver = web.PwConsoleWeb(
            global_vars=local_variables,
            loggers=loggers,
            sentence_completions=completions,
        )
        webserver.start()
    else:
        interactive_console = embed.PwConsoleEmbed(
            global_vars=local_variables,
            local_vars=None,
            loggers=log_windows,
            repl_startup_message=welcome_message,
            help_text=__doc__,
            config_file_path=config_file_path,
        )
        interactive_console.add_sentence_completer(completions)
        if serial_debug:
            interactive_console.add_bottom_toolbar(
                bandwidth_toolbar.BandwidthToolbar()
            )

        # Setup Python logger propagation
        interactive_console.setup_python_logging(
            # Send any unhandled log messages to the external file.
            last_resort_filename=log_file,
            # Don't change propagation for these loggers.
            loggers_with_no_propagation=[_DEVICE_LOG],
        )

        interactive_console.embed()


# pylint: disable=too-many-arguments,too-many-locals
def console(
    device: str,
    baudrate: int,
    ticks_per_second: int | None,
    token_databases: Collection[Path],
    socket_addr: str,
    logfile: str,
    host_logfile: str,
    device_logfile: str,
    json_logfile: str,
    serial_debug: bool = False,
    config_file: Path | None = None,
    verbose: bool = False,
    host_log_level: int = logging.INFO,
    device_log_level: int = logging.DEBUG,
    compiled_protos: Sequence[ModuleType] | None = None,
    merge_device_and_host_logs: bool = False,
    rpc_logging: bool = True,
    channel_id: int = rpc.DEFAULT_CHANNEL_ID,
    hdlc_encoding: bool = True,
    device_tracing: bool = True,
    browser: bool = False,
    timestamp_decoder: Callable[[int], str] | None = None,
    device_connection: DeviceConnection | None = None,
    debugger_listen: bool = False,
    debugger_wait_for_client: bool = False,
    extra_frame_handlers: dict[int, Callable[[bytes, Any], Any]] | None = None,
) -> int:
    """Starts an interactive RPC console for HDLC."""

    if debugger_listen or debugger_wait_for_client:
        debugpy.listen(("localhost", 5678))

    if debugger_wait_for_client:
        debugpy.wait_for_client()

    # Don't send device logs to the root logger.
    _DEVICE_LOG.propagate = False
    # Create pw_console log_store.LogStore handlers. These are the data source
    # for log messages to be displayed in the UI.
    device_log_store = log_store.LogStore()
    root_log_store = log_store.LogStore()
    serial_debug_log_store = log_store.LogStore()
    # Attach the log_store.LogStores as handlers for each log window we want to
    # show. This should be done before device initialization to capture early
    # messages.
    _DEVICE_LOG.addHandler(device_log_store)
    _ROOT_LOG.addHandler(root_log_store)
    _SERIAL_DEBUG.addHandler(serial_debug_log_store)

    if not logfile:
        # Create a temp logfile to prevent logs from appearing over stdout. This
        # would corrupt the prompt toolkit UI.
        logfile = python_logging.create_temp_log_file()

    if verbose:
        host_log_level = logging.DEBUG

    pw_cli_log.install(
        level=host_log_level,
        use_color=False,
        hide_timestamp=False,
        log_file=logfile,
    )

    if device_logfile:
        pw_cli_log.install(
            level=device_log_level,
            use_color=False,
            hide_timestamp=False,
            log_file=device_logfile,
            logger=_DEVICE_LOG,
        )
    if host_logfile:
        pw_cli_log.install(
            level=host_log_level,
            use_color=False,
            hide_timestamp=False,
            log_file=host_logfile,
            logger=_ROOT_LOG,
        )

    if merge_device_and_host_logs:
        # Add device logs to the default logfile.
        pw_cli_log.install(
            level=device_log_level,
            use_color=False,
            hide_timestamp=False,
            log_file=logfile,
            logger=_DEVICE_LOG,
        )

    _LOG.setLevel(host_log_level)
    _DEVICE_LOG.setLevel(device_log_level)
    _ROOT_LOG.setLevel(host_log_level)
    _SERIAL_DEBUG.setLevel(logging.DEBUG)

    if json_logfile:
        json_filehandler = logging.FileHandler(json_logfile, encoding='utf-8')
        json_filehandler.setLevel(device_log_level)
        json_filehandler.setFormatter(python_logging.JsonLogFormatter())
        _DEVICE_LOG.addHandler(json_filehandler)

    if device_connection is None:
        device_connection = create_device_serial_or_socket_connection(
            device=device,
            baudrate=baudrate,
            token_databases=token_databases,
            socket_addr=socket_addr,
            ticks_per_second=ticks_per_second,
            serial_debug=serial_debug,
            compiled_protos=compiled_protos,
            rpc_logging=rpc_logging,
            channel_id=channel_id,
            hdlc_encoding=hdlc_encoding,
            device_tracing=device_tracing,
            timestamp_decoder=timestamp_decoder,
            extra_frame_handlers=extra_frame_handlers,
        )

    with device_connection as device_client:
        _start_python_terminal(
            device=device_client,
            device_log_store=device_log_store,
            root_log_store=root_log_store,
            serial_debug_log_store=serial_debug_log_store,
            log_file=logfile,
            host_logfile=host_logfile,
            device_logfile=device_logfile,
            json_logfile=json_logfile,
            serial_debug=serial_debug,
            config_file_path=config_file,
            browser=browser,
        )
    return 0


def main(
    args: argparse.Namespace | None = None,
    compiled_protos: Sequence[ModuleType] | None = None,
    timestamp_decoder: Callable[[int], str] | None = None,
    device_connection: DeviceConnection | None = None,
    extra_frame_handlers: dict[int, Callable[[bytes, Any], Any]] | None = None,
) -> int:
    """Startup the pw console UI for a pw_system device.

    Example usage:

    .. code-block:: python

       from pw_protobuf_protos import common_pb2
       from pw_rpc import echo_pb2
       from pw_log.log_decoder.timestamp_parser_ms_since_boot
       import pw_system.console

       def main() -> int:
           return pw_system.console.main(
               compiled_protos=[
                   common_pb2,
                   echo_pb2,
               ],
               timestamp_decoder=timestamp_parser_ms_since_boot,
               device_connection=None,
           )

       if __name__ == '__main__':
           sys.exit(main())
    """
    return console(
        **vars(_parse_args(args)),
        compiled_protos=compiled_protos,
        timestamp_decoder=timestamp_decoder,
        device_connection=device_connection,
        extra_frame_handlers=extra_frame_handlers,
    )


# TODO(b/353327855): Deprecated function, remove when no longer used.
def main_with_compiled_protos(
    compiled_protos: Sequence[ModuleType] | None = None,
    args: argparse.Namespace | None = None,
    device_connection: DeviceConnection | None = None,
) -> int:
    return main(
        args=args,
        compiled_protos=compiled_protos,
        device_connection=device_connection,
    )


if __name__ == '__main__':
    sys.exit(main())
