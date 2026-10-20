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
"""Device classes to interact with targets via RPC."""


import logging
import os
from pathlib import Path
import tempfile
from types import ModuleType
from collections.abc import Iterable
from typing import Any, Callable, Sequence

from pw_file import file_pb2
from pw_hdlc import rpc
from pw_hdlc.decode import Frame
from pw_log import log_decoder
from pw_log_rpc import rpc_log_stream
from pw_metric import metric_parser
import pw_rpc
from pw_rpc import callback_client, console_tools, client_utils
import pw_transfer
from pw_transfer import transfer_pb2
from pw_stream import stream_readers
from pw_system import snapshot
from pw_thread import thread_analyzer
from pw_thread_protos import thread_pb2
from pw_tokenizer import detokenize
from pw_tokenizer.proto import decode_optionally_tokenized
from pw_unit_test.rpc import run_tests as pw_unit_test_run_tests, TestRecord


# Internal log for troubleshooting this tool (the console).
_LOG = logging.getLogger(__package__)

DEFAULT_DEVICE_LOGGER = logging.getLogger('rpc_device')


class Device:
    """Represents an RPC Client for a device running a Pigweed target.

    The target must have RPC support for the following services:
     - logging
     - file
     - transfer

    Note: use this class as a base for specialized device representations.
    """

    def __init__(
        # pylint: disable=too-many-arguments
        self,
        channel_id: int,
        reader: stream_readers.CancellableReader,
        write: Callable[[bytes], Any],
        proto_library: Iterable[ModuleType | Path],
        detokenizer: detokenize.Detokenizer | None = None,
        timestamp_decoder: Callable[[int], str] | None = None,
        rpc_timeout_s: float = 5,
        use_rpc_logging: bool = True,
        use_hdlc_encoding: bool = True,
        logger: logging.Logger | logging.LoggerAdapter = DEFAULT_DEVICE_LOGGER,
        extra_frame_handlers: (
            dict[int, Callable[[bytes, Any], Any]] | None
        ) = None,
    ):
        self.channel_id = channel_id
        self.protos = list(proto_library)
        self.detokenizer = detokenizer
        self.rpc_timeout_s = rpc_timeout_s

        self.logger = logger
        self.logger.setLevel(logging.DEBUG)  # Allow all device logs through.

        callback_client_impl = callback_client.Impl(
            default_unary_timeout_s=self.rpc_timeout_s,
            default_stream_timeout_s=None,
        )

        def detokenize_and_log_output(data: bytes, _detokenizer=None):
            log_messages = data.decode(
                encoding='utf-8', errors='surrogateescape'
            )

            if self.detokenizer:
                log_messages = decode_optionally_tokenized(
                    self.detokenizer, data
                )

            for line in log_messages.splitlines():
                self.logger.info(line)

        # Device has a hard dependency on transfer_pb2, so ensure it's
        # always been added to the list of compiled protos, rather than
        # requiring all clients to include it.
        if transfer_pb2 not in self.protos:
            self.protos.append(transfer_pb2)

        self.client: client_utils.RpcClient
        if use_hdlc_encoding:
            channels = [
                pw_rpc.Channel(self.channel_id, rpc.channel_output(write))
            ]

            def create_frame_handler_wrapper(
                handler: Callable[[bytes, Any], Any]
            ) -> Callable[[Frame], Any]:
                def handler_wrapper(frame: Frame):
                    handler(frame.data, self)

                return handler_wrapper

            extra_frame_handlers_wrapper: rpc.FrameHandlers = {}
            if extra_frame_handlers is not None:
                for address, handler in extra_frame_handlers.items():
                    extra_frame_handlers_wrapper[address] = (
                        create_frame_handler_wrapper(handler)
                    )

            self.client = rpc.HdlcRpcClient(
                reader,
                self.protos,
                channels,
                detokenize_and_log_output,
                client_impl=callback_client_impl,
                extra_frame_handlers=extra_frame_handlers_wrapper,
            )
        else:
            channel = pw_rpc.Channel(self.channel_id, write)
            self.client = client_utils.NoEncodingSingleChannelRpcClient(
                reader,
                self.protos,
                channel,
                client_impl=callback_client_impl,
            )

        if use_rpc_logging:
            # Create the log decoder used by the LogStreamHandler.

            def decoded_log_handler(log: log_decoder.Log) -> None:
                log_decoder.log_decoded_log(log, self.logger)

            self._log_decoder = log_decoder.LogStreamDecoder(
                decoded_log_handler=decoded_log_handler,
                detokenizer=self.detokenizer,
                source_name='RpcDevice',
                timestamp_parser=(
                    timestamp_decoder
                    if timestamp_decoder
                    else log_decoder.timestamp_parser_ns_since_boot
                ),
            )

            # Start listening to logs as soon as possible.
            self.log_stream_handler = rpc_log_stream.LogStreamHandler(
                self.rpcs, self._log_decoder
            )
            self.log_stream_handler.start_logging()

        # Create the transfer manager
        self.transfer_service = self.rpcs.pw.transfer.Transfer
        self.transfer_manager = pw_transfer.Manager(
            self.transfer_service,
            default_response_timeout_s=self.rpc_timeout_s,
            initial_response_timeout_s=self.rpc_timeout_s,
            default_protocol_version=pw_transfer.ProtocolVersion.LATEST,
        )

    def __enter__(self):
        return self

    def __exit__(self, *exc_info):
        self.close()

    def close(self) -> None:
        self.client.close()

    def info(self) -> console_tools.ClientInfo:
        return console_tools.ClientInfo('device', self.rpcs, self.client.client)

    @property
    def rpcs(self) -> Any:
        """Returns an object for accessing services on the specified channel."""
        return next(iter(self.client.client.channels())).rpcs

    def run_tests(
        self,
        timeout_s: float | None = 5,
        test_suites: Iterable[str] = (),
        test_filters: Iterable[Any] = (),
    ) -> TestRecord:
        """Runs the unit tests on this device."""
        return pw_unit_test_run_tests(
            self.rpcs,
            timeout_s=timeout_s,
            test_suites=test_suites,
            test_filters=test_filters,
        )

    def echo(self, msg: str) -> str:
        """Sends a string to the device and back, returning the result."""
        return self.rpcs.pw.rpc.EchoService.Echo(msg=msg).unwrap_or_raise().msg

    def reboot(self):
        """Triggers a reboot to run asynchronously on the device.

        This function *does not* wait for the reboot to complete."""
        # `invoke` rather than call in order to ignore the result. No result
        # will be sent when the device reboots.
        self.rpcs.pw.system.proto.DeviceService.Reboot.invoke()

    def crash(self):
        """Triggers a crash to run asynchronously on the device.

        This function *does not* wait for the crash to complete."""
        # `invoke` rather than call in order to ignore the result. No result
        # will be sent when the device crashes.
        self.rpcs.pw.system.proto.DeviceService.Crash.invoke()

    def get_and_log_metrics(self) -> dict:
        """Retrieves the parsed metrics and logs them to the console."""
        metrics = metric_parser.parse_metrics(
            self.rpcs, self.detokenizer, self.rpc_timeout_s
        )

        def print_metrics(metrics, path):
            """Traverses dictionaries, until a non-dict value is reached."""
            for path_name, metric in metrics.items():
                if isinstance(metric, dict):
                    print_metrics(metric, path + '/' + path_name)
                else:
                    _LOG.info('%s/%s: %s', path, path_name, str(metric))

        print_metrics(metrics, '')
        return metrics

    def snapshot_peak_stack_usage(self, thread_name: str | None = None):
        snapshot_service = self.rpcs.pw.thread.proto.ThreadSnapshotService
        _, rsp = snapshot_service.GetPeakStackUsage(name=thread_name)

        thread_info = thread_pb2.SnapshotThreadInfo()
        for thread_info_block in rsp:
            for thread in thread_info_block.threads:
                thread_info.threads.append(thread)
        for line in str(
            thread_analyzer.ThreadSnapshotAnalyzer(thread_info)
        ).splitlines():
            _LOG.info('%s', line)

    def list_files(self) -> Sequence[file_pb2.ListResponse]:
        """Lists all files on this device.
        Returns:
            A sequence of responses from the List() RPC.
        """
        fs_service = self.rpcs.pw.file.FileSystem
        stream_response = fs_service.List()
        if not stream_response.status.ok():
            _LOG.error('Failed to list files %s', stream_response.status)
            return []

        return stream_response.responses

    def delete_file(self, path: str) -> bool:
        """Delete a file on this device.
        Args:
            path: The path of the file to delete.
        Returns:
            True on successful deletion, False on failure.
        """

        fs_service = self.rpcs.pw.file.FileSystem
        req = file_pb2.DeleteRequest(path=path)
        stream_response = fs_service.Delete(req)
        if not stream_response.status.ok():
            _LOG.error(
                'Failed to delete file %s file: %s',
                path,
                stream_response.status,
            )
            return False

        return True

    def transfer_file(self, file_id: int, dest_path: str) -> bool:
        """Transfer a file on this device to the host.
        Args:
            file_id: The file_id of the file to transfer from device.
            dest_path: The destination path to save the file to on the host.
        Returns:
            True on successful transfer, False on failure.
        Raises:
            pw_transfer.Error the transfer failed.
        """
        try:
            data = self.transfer_manager.read(file_id)
            with open(dest_path, "wb") as bin_file:
                bin_file.write(data)
            _LOG.info(
                'Successfully wrote file to %s', os.path.abspath(dest_path)
            )
        except pw_transfer.Error:
            _LOG.exception('Failed to transfer file_id %i', file_id)
            return False

        return True

    def get_crash_snapshots(self, crash_log_path: str | None = None) -> bool:
        r"""Transfer any crash snapshots on this device to the host.
        Args:
            crash_log_path: The host path to store the crash files.
              If not specified, defaults to `/tmp` or `C:\TEMP` on Windows.
        Returns:
            True on successful download of snapshot, or no snapshots
            on device.  False on failure to download snapshot.
        """
        if crash_log_path is None:
            crash_log_path = tempfile.gettempdir()

        snapshot_paths: list[file_pb2.Path] = []
        for response in self.list_files():
            for snapshot_path in response.paths:
                if snapshot_path.path.startswith('/snapshots/crash_'):
                    snapshot_paths.append(snapshot_path)

        if len(snapshot_paths) == 0:
            _LOG.info('No crash snapshot on the device.')
            return True

        for snapshot_path in snapshot_paths:
            dest_snapshot = os.path.join(
                crash_log_path, os.path.basename(snapshot_path.path)
            )
            if not self.transfer_file(snapshot_path.file_id, dest_snapshot):
                return False

            decoded_snapshot: str
            with open(dest_snapshot, 'rb') as f:
                decoded_snapshot = snapshot.decode_snapshot(
                    self.detokenizer, f.read()
                )

            dest_text_snapshot = dest_snapshot.replace(".snapshot", ".txt")
            with open(dest_text_snapshot, 'w') as f:
                f.write(decoded_snapshot)
            _LOG.info('Wrote crash snapshot to: %s', dest_text_snapshot)

            if not self.delete_file(snapshot_path.path):
                return False

        return True
