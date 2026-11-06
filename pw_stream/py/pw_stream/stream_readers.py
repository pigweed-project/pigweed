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
"""Serial reader utilities."""

from abc import ABC, abstractmethod
import concurrent.futures
import logging
import os
import platform
import select
import threading
import time
import socket
import subprocess
from typing import (
    Any,
    Callable,
    Iterable,
    Sequence,
    TypeVar,
)
import warnings

import serial

_LOG = logging.getLogger('pw_stream.stream_readers')
_VERBOSE = logging.DEBUG - 1

FrameTypeT = TypeVar('FrameTypeT')


class CancellableReader(ABC):
    """Wraps communication interfaces used for reading incoming data with the
    guarantee that the read request can be cancelled. Derived classes must
    implement the :py:func:`cancel_read()` method.

    Cancelling a read invalidates ongoing and future reads. The
    :py:func:`cancel_read()` method can only be called once.
    """

    def __init__(self, base_obj: Any, *read_args, **read_kwargs):
        """
        Args:
            base_obj: Object that offers a ``read()`` method with optional args
                and kwargs.
            read_args: Arguments for ``base_obj.read()`` function.
            read_kwargs: Keyword arguments for ``base_obj.read()`` function.
        """
        self._base_obj = base_obj
        self._read_args = read_args
        self._read_kwargs = read_kwargs

    def __enter__(self) -> 'CancellableReader':
        return self

    def __exit__(self, *exc_info) -> None:
        self.cancel_read()

    def read(self) -> bytes:
        """Reads bytes that contain parts of or full RPC packets."""
        return self._base_obj.read(*self._read_args, **self._read_kwargs)

    @abstractmethod
    def cancel_read(self) -> None:
        """Cancels a blocking read request and all future reads.

        Can only be called once.
        """


class SelectableReader(CancellableReader):
    """
    Wraps interfaces that work with ``select()`` to signal when data is
    received.

    These interfaces must provide a ``fileno()`` method.
    WINDOWS ONLY: Only sockets that originate from WinSock can be wrapped. File
    objects are not acceptable.
    """

    _STOP_CMD = b'STOP'

    def __init__(self, base_obj: Any, *read_args, **read_kwargs):
        assert hasattr(base_obj, 'fileno')
        if platform.system() == 'Windows' and not isinstance(
            base_obj, socket.socket
        ):
            raise ValueError('Only socket objects are selectable on Windows')
        super().__init__(base_obj, *read_args, **read_kwargs)
        self._cancel_signal_pipe_r_fd, self._cancel_signal_pipe_w_fd = os.pipe()
        self._waiting_for_read_or_cancel_lock = threading.Lock()

    def __exit__(self, *exc_info) -> None:
        self.cancel_read()
        with self._waiting_for_read_or_cancel_lock:
            if self._cancel_signal_pipe_r_fd > 0:
                os.close(self._cancel_signal_pipe_r_fd)
                self._cancel_signal_pipe_r_fd = -1

    def read(self) -> bytes:
        if self._wait_for_read_or_cancel():
            return super().read()
        return b''

    def _wait_for_read_or_cancel(self) -> bool:
        """Returns ``True`` when ready to read."""
        with self._waiting_for_read_or_cancel_lock:
            if self._base_obj.fileno() < 0 or self._cancel_signal_pipe_r_fd < 0:
                # The interface might've been closed already.
                return False
            ready_to_read, _, exception_list = select.select(
                [self._cancel_signal_pipe_r_fd, self._base_obj],
                [],
                [self._base_obj],
            )
            if self._cancel_signal_pipe_r_fd in ready_to_read:
                # A signal to stop the reading process was received.
                os.read(self._cancel_signal_pipe_r_fd, len(self._STOP_CMD))
                os.close(self._cancel_signal_pipe_r_fd)
                self._cancel_signal_pipe_r_fd = -1
                return False

            if exception_list:
                _LOG.error('Error reading interface')
                return False
        return True

    def cancel_read(self) -> None:
        if self._cancel_signal_pipe_w_fd > 0:
            os.write(self._cancel_signal_pipe_w_fd, self._STOP_CMD)
            os.close(self._cancel_signal_pipe_w_fd)
            self._cancel_signal_pipe_w_fd = -1


class SocketReader(SelectableReader):
    """Wraps a socket ``recv()`` function."""

    def __init__(self, base_obj: socket.socket, *read_args, **read_kwargs):
        super().__init__(base_obj, *read_args, **read_kwargs)

    def read(self) -> bytes:
        if self._wait_for_read_or_cancel():
            return self._base_obj.recv(*self._read_args, **self._read_kwargs)
        return b''

    def __exit__(self, *exc_info) -> None:
        self.cancel_read()
        self._base_obj.close()


class SerialReader(CancellableReader):
    """Wraps a :py:class:`serial.Serial` object."""

    def __init__(self, base_obj: serial.Serial, *read_args, **read_kwargs):
        super().__init__(base_obj, *read_args, **read_kwargs)

    def cancel_read(self) -> None:
        self._base_obj.cancel_read()

    def __exit__(self, *exc_info) -> None:
        self.cancel_read()
        self._base_obj.close()


class DataReaderAndExecutor:
    """Reads incoming bytes, data processor that delegates frame handling.

    Executing callbacks in a ``ThreadPoolExecutor`` decouples reading the input
    stream from handling the data. That way, if a handler function takes a
    long time or crashes, this reading thread is not interrupted.
    """

    def __init__(
        self,
        reader: CancellableReader,
        on_read_error: Callable[[Exception], None],
        data_processor: Callable[[bytes], Iterable[FrameTypeT]],
        frame_handler: Callable[[FrameTypeT], None],
        handler_threads: int | None = 1,
    ):
        """Creates the data reader and frame delegator.

        Args:
            reader: Reads incoming bytes from the given transport, blocks until
              data is available or an exception is raised. Otherwise the reader
              will exit.
            on_read_error: Called when there is an error reading incoming bytes.
            data_processor: Processes read bytes and returns a frame-like object
              that the frame_handler can process.
            frame_handler: Handles a received frame.
            handler_threads: The number of threads in the executor pool.
        """

        self._reader = reader
        self._on_read_error = on_read_error
        self._data_processor = data_processor
        self._frame_handler = frame_handler
        self._handler_threads = handler_threads

        self._reader_thread = threading.Thread(target=self._run)
        self._reader_thread_stop = threading.Event()

    def start(self) -> None:
        """Starts the reading process."""
        _LOG.debug('Starting read process')
        self._reader_thread_stop.clear()
        self._reader_thread.start()

    def stop(self) -> None:
        """Stops the reading process.

        This requests that the reading process stop and waits
        for the background thread to exit.
        """
        _LOG.debug('Stopping read process')
        self._reader_thread_stop.set()
        self._reader.cancel_read()
        self._reader_thread.join(30)
        if self._reader_thread.is_alive():
            warnings.warn(
                'Timed out waiting for read thread to terminate.\n'
                'Tip: Use a `CancellableReader` to cancel reads.'
            )

    def _run(self) -> None:
        """Reads raw data in a background thread."""
        with concurrent.futures.ThreadPoolExecutor(
            max_workers=self._handler_threads
        ) as executor:
            while not self._reader_thread_stop.is_set():
                try:
                    data = self._reader.read()
                except Exception as exc:  # pylint: disable=broad-except
                    # Don't report the read error if the thread is stopping.
                    # The stream or device backing _read was likely closed,
                    # so errors are expected.
                    if not self._reader_thread_stop.is_set():
                        self._on_read_error(exc)
                    _LOG.debug(
                        'DataReaderAndExecutor thread exiting due to exception',
                        exc_info=exc,
                    )
                    return

                if not data:
                    continue

                _LOG.log(_VERBOSE, 'Read %2d B: %s', len(data), data)

                for frame in self._data_processor(data):
                    executor.submit(self._frame_handler, frame)


def _try_connect(port: int, attempts: int = 10) -> socket.socket:
    """Tries to connect to the specified port up to the given number of times.

    This is helpful when connecting to a process that was started by this
    script. The process may need time to start listening for connections, and
    that length of time can vary. This retries with a short delay rather than
    having to wait for the worst case delay every time.
    """
    timeout_s = 0.001
    while True:
        time.sleep(timeout_s)

        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.connect(('localhost', port))
            return sock
        except ConnectionRefusedError:
            sock.close()
            attempts -= 1
            if attempts <= 0:
                raise

            timeout_s *= 2


class SocketSubprocess:
    """Executes a subprocess and connects to it with a socket."""

    def __init__(self, command: Sequence, port: int) -> None:
        self._server_process = subprocess.Popen(command, stdin=subprocess.PIPE)
        self.stdin = self._server_process.stdin

        try:
            self.socket: socket.socket = _try_connect(port)  # 🧦
        except:
            self._server_process.terminate()
            self._server_process.communicate()
            raise

    def close(self) -> None:
        try:
            self.socket.close()
        finally:
            self._server_process.terminate()
            self._server_process.communicate()

    def __enter__(self) -> 'SocketSubprocess':
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> None:
        self.close()
