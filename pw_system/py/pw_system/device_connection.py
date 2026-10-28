# Copyright 2024 The Pigweed Authors
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
"""Device instance creation."""

import argparse
from dataclasses import dataclass
import logging
from pathlib import Path
import time
from types import ModuleType
from typing import Any, Callable, Collection

import serial

from pw_console import pyserial_wrapper
from pw_console import socket_client
from pw_hdlc import rpc
from pw_log.log_decoder import timestamp_parser_ms_since_boot
from pw_tokenizer import detokenize

from pw_system.find_serial_port import interactive_serial_port_select
from pw_system import device as pw_device
from pw_system import device_tracing as pw_device_tracing

# Default proto imports:
# pylint: disable=ungrouped-imports,wrong-import-order
from pw_file import file_pb2
from pw_log.proto import log_pb2
from pw_metric_proto import metric_service_pb2
from pw_rpc import echo_pb2
from pw_rpc import query_pb2
from pw_stream import stream_readers
from pw_thread_protos import thread_snapshot_service_pb2
from pw_trace_protos import trace_service_pb2

from pw_system_protos import device_service_pb2
from pw_unit_test_proto import unit_test_pb2


# pylint: enable=ungrouped-imports

# Internal log for troubleshooting this tool (the console).
_LOG = logging.getLogger(__package__)
_ROOT_LOG = logging.getLogger()

DEFAULT_DEVICE_LOGGER = logging.getLogger('rpc_device')


@dataclass
class DeviceConnection:
    """Stores a Device client along with the reader and writer."""

    client: pw_device_tracing.DeviceWithTracing | pw_device.Device
    reader: stream_readers.SelectableReader | stream_readers.SerialReader
    writer: Callable[[bytes], int | None]

    def __enter__(self):
        """Enter the reader followed by the client context managers.

        Returns the device client for RPC interaction.
        """
        self.reader.__enter__()
        self.client.__enter__()
        return self.client

    def __exit__(self, *exc_info):
        """Close the device connection followed by the reader."""
        self.client.__exit__()
        self.reader.__exit__()


def create_device_serial_or_socket_connection(
    # pylint: disable=too-many-arguments,too-many-locals
    device: str,
    baudrate: int,
    token_databases: Collection[Path],
    socket_addr: str | None = None,
    ticks_per_second: int | None = None,
    serial_debug: bool = False,
    compiled_protos: list[ModuleType] | None = None,
    rpc_logging: bool = True,
    channel_id: int = rpc.DEFAULT_CHANNEL_ID,
    hdlc_encoding: bool = True,
    device_tracing: bool = True,
    device_class: type[pw_device.Device] | None = pw_device.Device,
    device_tracing_class: type[pw_device_tracing.DeviceWithTracing] | None = (
        pw_device_tracing.DeviceWithTracing
    ),
    timestamp_decoder: Callable[[int], str] | None = None,
    extra_frame_handlers: dict[int, Callable[[bytes, Any], Any]] | None = None,
) -> DeviceConnection:
    """Setup a pw_system.Device client connection.

    Full example of opening a device connection and running an RPC:

    .. code-block:: python

       from pw_system.device_connection import (
           add_device_args,
           create_device_serial_or_socket_connection,
       )

       from pw_protobuf_protos import common_pb2
       from pw_rpc import echo_pb2

       parser = argparse.ArgumentParser(
           prog='MyProductScript',
       )
       parser = add_device_args(parser)
       args = parser.parse_args()

       compiled_protos = [
           common_pb2,
           echo_pb2,
       ]

       device_connection = create_device_serial_or_socket_connection(
           device=args.device,
           baudrate=args.baudrate,
           token_databases=args.token_databases,
           compiled_protos=compiled_protos,
           socket_addr=args.socket_addr,
           ticks_per_second=args.ticks_per_second,
           serial_debug=args.serial_debug,
           rpc_logging=args.rpc_logging,
           hdlc_encoding=args.hdlc_encoding,
           channel_id=args.channel_id,
           device_tracing=args.device_tracing,
           device_class=Device,
           device_tracing_class=DeviceWithTracing,
           timestamp_decoder=timestamp_parser_ms_since_boot,
       )


       # Open the device connction and interact with the Device client.
       with device_connection as device:
           # Make a shortcut to the EchoService.
           echo_service = device.rpcs.pw.rpc.EchoService

           # Call some RPCs and check the results.
           result = echo_service.Echo(msg='Hello')

           if result.status.ok():
               print('The status was', result.status)
               print('The message was', result.response.msg)
           else:
               print('Uh oh, this RPC returned', result.status)

           # The result object can be split into status and payload
           # when assigned.
           status, payload = echo_service.Echo(msg='Goodbye!')

           print(f'{status}: {payload}')
    """

    detokenizer = None
    if token_databases:
        token_databases_with_domains = [] * len(token_databases)
        for token_database in token_databases:
            # Load all domains from token database.
            token_databases_with_domains.append(str(token_database) + "#.*")

        detokenizer = detokenize.AutoUpdatingDetokenizer(
            *token_databases_with_domains
        )
        detokenizer.show_errors = True

    protos: list[ModuleType | Path] = []

    if compiled_protos is None:
        compiled_protos = []

    # Append compiled log.proto library to avoid include errors when
    # manually provided, and shadowing errors due to ordering when the
    # default global search path is used.
    compiled_protos.append(log_pb2)
    compiled_protos.append(unit_test_pb2)
    protos.extend(compiled_protos)
    protos.append(metric_service_pb2)
    protos.append(thread_snapshot_service_pb2)
    protos.append(file_pb2)
    protos.append(echo_pb2)
    protos.append(query_pb2)
    protos.append(trace_service_pb2)
    protos.append(device_service_pb2)

    reader: stream_readers.SelectableReader | stream_readers.SerialReader

    if socket_addr is None:
        serial_impl = (
            pyserial_wrapper.SerialWithLogging
            if serial_debug
            else serial.Serial
        )

        if not device:
            device = interactive_serial_port_select()
        _ROOT_LOG.info('Using serial port: %s', device)
        serial_device = serial_impl(
            device,
            baudrate,
            # Timeout in seconds. This should be a very small value. Setting to
            # zero makes pyserial read() non-blocking which will cause the host
            # machine to busy loop and 100% CPU usage.
            # https://pythonhosted.org/pyserial/pyserial_api.html#serial.Serial
            timeout=0.1,
        )
        reader = stream_readers.SerialReader(serial_device, 8192)
        write = serial_device.write

        # Overwrite decoder for serial device.
        if timestamp_decoder is None:
            timestamp_decoder = timestamp_parser_ms_since_boot
    else:
        socket_impl = (
            socket_client.SocketClientWithLogging
            if serial_debug
            else socket_client.SocketClient
        )

        def disconnect_handler(
            socket_device: socket_client.SocketClient,
        ) -> None:
            """Attempts to reconnect on disconnected socket."""
            _LOG.error('Socket disconnected. Will retry to connect.')
            while True:
                try:
                    socket_device.connect()
                    break
                except:  # pylint: disable=bare-except
                    # Ignore errors and retry to reconnect.
                    time.sleep(1)
            _LOG.info('Successfully reconnected')

        try:
            socket_device = socket_impl(
                socket_addr, on_disconnect=disconnect_handler
            )
            reader = stream_readers.SelectableReader(socket_device, 8192)
            write = socket_device.write
        except ValueError as error:
            raise ValueError(
                f'Failed to initialize socket at {socket_addr}'
            ) from error

    device_args: list[Any] = [channel_id, reader, write]
    device_kwds: dict[str, Any] = {
        'proto_library': protos,
        'detokenizer': detokenizer,
        'timestamp_decoder': timestamp_decoder,
        'rpc_timeout_s': 5,
        'use_rpc_logging': rpc_logging,
        'use_hdlc_encoding': hdlc_encoding,
        'extra_frame_handlers': extra_frame_handlers,
    }

    device_client: pw_device_tracing.DeviceWithTracing | pw_device.Device
    if device_tracing:
        device_kwds['ticks_per_second'] = ticks_per_second
        if device_tracing_class is None:
            device_tracing_class = pw_device_tracing.DeviceWithTracing
        device_client = device_tracing_class(*device_args, **device_kwds)
    else:
        if device_class is None:
            device_class = pw_device.Device
        device_client = device_class(*device_args, **device_kwds)

    return DeviceConnection(device_client, reader, write)


def add_device_args(parser: argparse.ArgumentParser) -> argparse.ArgumentParser:
    """Add device specific args required by the pw_system.Device class."""

    group = parser.add_mutually_exclusive_group(required=False)

    group.add_argument(
        '-d',
        '--device',
        help='the serial port to use',
    )
    group.add_argument(
        '-s',
        '--socket-addr',
        type=str,
        help=(
            'Socket address used to connect to server. Type "default" to use '
            'localhost:33000, pass the server address and port as '
            'address:port, or prefix the path to a forwarded socket with '
            f'"{socket_client.SocketClient.FILE_SOCKET_SERVER}:" as '
            f'{socket_client.SocketClient.FILE_SOCKET_SERVER}:path_to_file.'
        ),
    )

    parser.add_argument(
        '-b',
        '--baudrate',
        type=int,
        default=115200,
        help='the baud rate to use',
    )
    parser.add_argument(
        '--serial-debug',
        action='store_true',
        help=(
            'Enable debug log tracing of all data passed through'
            'pyserial read and write.'
        ),
    )
    parser.add_argument(
        "--token-databases",
        metavar='elf_or_token_database',
        nargs="+",
        type=Path,
        help="Path to tokenizer database csv file(s).",
    )
    parser.add_argument(
        '-f',
        '--ticks_per_second',
        type=int,
        dest='ticks_per_second',
        help=('The clock rate of the trace events.'),
    )
    parser.add_argument(
        '--rpc-logging',
        action=argparse.BooleanOptionalAction,
        default=True,
        help='Use pw_rpc based logging.',
    )
    parser.add_argument(
        '--hdlc-encoding',
        action=argparse.BooleanOptionalAction,
        default=True,
        help='Use HDLC encoding on transfer interfaces.',
    )
    parser.add_argument(
        '--channel-id',
        type=int,
        default=rpc.DEFAULT_CHANNEL_ID,
        help="Channel ID used in RPC communications.",
    )
    parser.add_argument(
        '--device-tracing',
        action=argparse.BooleanOptionalAction,
        default=True,
        help='Use device tracing.',
    )

    return parser
