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
"""Reliable framed datagram transport over TCP.

Each datagram is sent on the wire as a 4-byte little-endian unsigned payload
length followed by the payload bytes, matching
``pw::transport::FramedTcpListener`` and ``pw::transport::FramedTcpConnector``.

This module is intended for host tests and tools in upstream Pigweed.
"""

from __future__ import annotations

from collections.abc import Iterator
import contextlib
import errno
import selectors
import socket
import struct
import threading
from typing import Final

from pw_transport._socket import (
    Deadline,
    ReliableListener,
    ReliableSocket,
    SocketClosed,
)

__all__ = [
    'DEFAULT_MAX_MESSAGE_SIZE',
    'FramedTcpListener',
    'connect',
]

#: Default maximum datagram size in bytes, matching
#: ``pw::transport::FramedTcpOptions::max_message_size_bytes``.
DEFAULT_MAX_MESSAGE_SIZE: Final[int] = 64 * 1024

_MAX_WIRE_MESSAGE_SIZE: Final[int] = 0xFFFFFFFF
_PORT_BIND_ATTEMPTS: Final[int] = 10
_LENGTH_PREFIX = struct.Struct('<I')


def _validate_max_message_size(max_message_size: int) -> None:
    if max_message_size < 0 or max_message_size > _MAX_WIRE_MESSAGE_SIZE:
        raise ValueError(
            f'max_message_size must be between 0 and '
            f'{_MAX_WIRE_MESSAGE_SIZE}, got {max_message_size}'
        )


class _InterruptibleSocket:
    """A non-blocking socket whose waits are interrupted by ``close()``.

    ``close()`` takes effect immediately, but the file descriptors stay open
    until operations in progress on other threads exit. Closing a file
    descriptor that another thread is waiting on would not reliably wake it.
    """

    def __init__(self, sock: socket.socket, description: str) -> None:
        try:
            sock.setblocking(False)
            # close() writes to this socketpair to wake waits in other threads.
            self._wake_read, self._wake_write = socket.socketpair()
        except BaseException:
            sock.close()
            raise
        self.sock = sock
        self._closed_message = f'The {description} is closed'
        self._lock = threading.Lock()
        self._closed = False
        # Counts the operations in progress, plus one until close() is called.
        self._users = 1

    @property
    def closed(self) -> bool:
        return self._closed

    def check_open(self) -> None:
        if self._closed:
            raise SocketClosed(self._closed_message)

    @contextlib.contextmanager
    def operation(self) -> Iterator[None]:
        """Keeps the file descriptors open while an operation is in progress.

        Raises:
          SocketClosed: ``close()`` was already called.
        """
        with self._lock:
            self.check_open()
            self._users += 1
        try:
            yield
        finally:
            self._release()

    def wait(
        self, events: int, deadline: Deadline, timeout_message: str
    ) -> None:
        """Blocks until the socket may be ready; the caller retries its I/O.

        Raises:
          SocketClosed: ``close()`` was called.
          TimeoutError: The deadline expired.
        """
        if deadline.expired():
            raise TimeoutError(timeout_message)
        with selectors.DefaultSelector() as selector:
            selector.register(self.sock, events)
            selector.register(self._wake_read, selectors.EVENT_READ)
            selector.select(deadline.remaining())
        self.check_open()

    def close(self) -> None:
        with self._lock:
            if self._closed:
                return
            self._closed = True
        self._wake_write.send(b'\0')
        self._release()

    def _release(self) -> None:
        with self._lock:
            self._users -= 1
            if self._users:
                return
        self.sock.close()
        self._wake_read.close()
        self._wake_write.close()


class _FramedTcpSocket(ReliableSocket):
    """Sends and receives length-prefixed datagrams over a TCP connection."""

    def __init__(self, sock: socket.socket, max_message_size: int) -> None:
        try:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except OSError:
            # Some platforms report an error if the peer already disconnected.
            pass

        self._io = _InterruptibleSocket(sock, 'socket')
        self._max_message_size = max_message_size
        self._read_lock = threading.Lock()
        self._write_lock = threading.Lock()
        # The bytes of the incoming frame received so far. These are kept when
        # a read times out so that the next read resumes the frame.
        self._rx_frame = bytearray()

    @property
    def max_read_message_size(self) -> int:
        return self._max_message_size

    @property
    def max_write_message_size(self) -> int:
        return self._max_message_size

    @property
    def closed(self) -> bool:
        return self._io.closed

    def _write(self, data: bytes, deadline: Deadline) -> None:
        frame = memoryview(_LENGTH_PREFIX.pack(len(data)) + data)
        remaining = deadline.remaining()
        with self._io.operation():
            if not self._write_lock.acquire(
                timeout=-1 if remaining is None else remaining
            ):
                raise TimeoutError('Timed out waiting to write datagram')
            try:
                # The previous write may have closed the socket mid-frame.
                self._io.check_open()
                self._send(frame, deadline)
            finally:
                self._write_lock.release()

    def _send(self, frame: memoryview, deadline: Deadline) -> None:
        sent = 0
        try:
            while sent < len(frame):
                try:
                    sent += self._io.sock.send(frame[sent:])
                except BlockingIOError:
                    self._io.wait(
                        selectors.EVENT_WRITE,
                        deadline,
                        'Timed out waiting to write datagram',
                    )
                except OSError as error:
                    self.close()
                    raise SocketClosed(
                        f'The connection failed: {error}'
                    ) from error
        except BaseException:
            if sent:  # A partially sent frame corrupts the stream.
                self.close()
            raise

    def _read(self, deadline: Deadline) -> bytes:
        if not self._read_lock.acquire(blocking=False):
            raise RuntimeError(
                'Another read() is already in progress on this socket'
            )
        try:
            with self._io.operation():
                self._receive(_LENGTH_PREFIX.size, deadline)
                (length,) = _LENGTH_PREFIX.unpack_from(self._rx_frame)
                if length > self._max_message_size:
                    self.close()
                    raise SocketClosed(
                        f'Incoming {length}-byte datagram exceeds the '
                        f'{self._max_message_size}-byte limit'
                    )
                self._receive(_LENGTH_PREFIX.size + length, deadline)
                payload = bytes(self._rx_frame[_LENGTH_PREFIX.size :])
                self._rx_frame.clear()
                return payload
        finally:
            self._read_lock.release()

    def _receive(self, size: int, deadline: Deadline) -> None:
        """Receives until ``size`` bytes of the incoming frame are buffered."""
        while len(self._rx_frame) < size:
            try:
                chunk = self._io.sock.recv(size - len(self._rx_frame))
            except BlockingIOError:
                self._io.wait(
                    selectors.EVENT_READ,
                    deadline,
                    'Timed out waiting for datagram',
                )
                continue
            except OSError as error:
                self.close()
                raise SocketClosed(f'The connection failed: {error}') from error

            if not chunk:
                self.close()
                raise SocketClosed('The peer closed the connection')
            self._rx_frame += chunk

    def close(self) -> None:
        self._io.close()


def connect(
    host: str,
    port: int,
    *,
    timeout: float | None = None,
    max_message_size: int = DEFAULT_MAX_MESSAGE_SIZE,
) -> ReliableSocket:
    """Opens a framed TCP :py:class:`~pw_transport.ReliableSocket`.

    Args:
      host: Hostname or IPv4/IPv6 address of the peer.
      port: TCP port of the peer.
      timeout: Maximum time to wait in seconds when establishing the TCP
        connection, or ``None`` (or ``math.inf``) to wait indefinitely. If
        ``host`` resolves to multiple addresses, the timeout applies to each
        connection attempt.
      max_message_size: Largest datagram to read or write, in bytes.

    Raises:
      TimeoutError: Establishing the TCP connection timed out.
      OSError: The host could not be resolved or the connection failed.
      ValueError: ``timeout`` or ``max_message_size`` is invalid.
    """
    _validate_max_message_size(max_message_size)
    try:
        sock = socket.create_connection(
            (host, port), Deadline(timeout).remaining()
        )
    except BlockingIOError:  # A timeout of 0 makes the connection non-blocking.
        raise TimeoutError('Timed out connecting') from None
    return _FramedTcpSocket(sock, max_message_size)


def _bind_dualstack_socket(port: int, backlog: int | None) -> socket.socket:
    sock = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
    try:
        sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
        sock.bind(('', port))
        if backlog is None:
            sock.listen()
        else:
            sock.listen(backlog)
        return sock
    except BaseException:
        sock.close()
        raise


def _open_dualstack_socket(port: int, backlog: int | None) -> socket.socket:
    if port != 0:
        return socket.create_server(
            ('', port),
            family=socket.AF_INET6,
            backlog=backlog,
            dualstack_ipv6=True,
        )

    # On some systems, such as macOS, binding a dual-stack socket to port 0 may
    # pick a port that another socket uses for a specific IPv4 address, such as
    # 127.0.0.1. Find a port that no IPv4 socket uses, then bind without
    # SO_REUSEADDR so binding fails if any socket uses the port.
    for _ in range(_PORT_BIND_ATTEMPTS):
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as ipv4_sock:
                ipv4_sock.bind(('', 0))
                candidate_port: int = ipv4_sock.getsockname()[1]
        except OSError:
            break
        try:
            return _bind_dualstack_socket(candidate_port, backlog)
        except OSError as error:
            if error.errno != errno.EADDRINUSE:
                raise
    return _bind_dualstack_socket(0, backlog)


def _create_listening_socket(
    host: str,
    port: int,
    backlog: int | None,
) -> socket.socket:
    if not host and socket.has_dualstack_ipv6():
        try:
            return _open_dualstack_socket(port, backlog)
        except OSError as error:
            # Fall back to IPv4 if IPv6 is unavailable, as in some containers.
            if error.errno not in (
                errno.EAFNOSUPPORT,
                errno.EPROTONOSUPPORT,
                errno.EADDRNOTAVAIL,
            ):
                raise

    # Prefer IPv4 for hosts with both IPv4 and IPv6 addresses, such as
    # localhost, so that peers connecting to 127.0.0.1 can reach the listener.
    families = {
        addrinfo[0]
        for addrinfo in socket.getaddrinfo(
            host or None,
            port,
            type=socket.SOCK_STREAM,
            flags=socket.AI_PASSIVE,
        )
    }
    return socket.create_server(
        (host, port),
        family=(
            socket.AF_INET if socket.AF_INET in families else socket.AF_INET6
        ),
        backlog=backlog,
    )


class FramedTcpListener(ReliableListener):
    """Accepts incoming :py:class:`~pw_transport.ReliableSocket` connections.

    This is the Python counterpart of ``pw::transport::FramedTcpListener``.
    """

    def __init__(
        self,
        port: int = 0,
        *,
        host: str = '',
        backlog: int | None = None,
        max_message_size: int = DEFAULT_MAX_MESSAGE_SIZE,
    ) -> None:
        """Binds and starts listening on ``(host, port)``.

        Args:
          port: TCP port to listen on, or ``0`` to let the OS pick a free port.
          host: Interface address or hostname to bind to, or ``''`` for all
            local interfaces. A hostname with both IPv4 and IPv6 addresses,
            such as ``localhost``, is bound to its IPv4 address only.
          backlog: Optional listen backlog passed to ``socket.create_server``.
          max_message_size: Largest datagram to read or write on accepted
            sockets, in bytes.

        Raises:
          OSError: Binding or listening on ``(host, port)`` failed.
          ValueError: ``max_message_size`` is invalid.
        """
        _validate_max_message_size(max_message_size)
        self._max_message_size = max_message_size
        self._io = _InterruptibleSocket(
            _create_listening_socket(host, port, backlog), 'listener'
        )
        self._port: int = self._io.sock.getsockname()[1]

    @property
    def port(self) -> int:
        """The local TCP port this listener is bound to."""
        return self._port

    @property
    def max_message_size(self) -> int:
        """Largest datagram to read or write on accepted sockets, in bytes."""
        return self._max_message_size

    @property
    def closed(self) -> bool:
        """True if :py:meth:`close` has been called on this listener."""
        return self._io.closed

    def _accept(self, deadline: Deadline) -> ReliableSocket:
        with self._io.operation():
            while True:
                try:
                    conn, _ = self._io.sock.accept()
                except ConnectionAbortedError:
                    continue  # The peer disconnected before it was accepted.
                except BlockingIOError:
                    self._io.wait(
                        selectors.EVENT_READ,
                        deadline,
                        'Timed out waiting for connection',
                    )
                    continue
                except OSError as error:
                    # Any other error closes the listener, even if it could be
                    # transient; create a new listener to continue.
                    self.close()
                    raise SocketClosed(
                        f'The listener failed: {error}'
                    ) from error

                return _FramedTcpSocket(conn, self._max_message_size)

    def close(self) -> None:
        """Stops listening and unblocks pending :py:meth:`accept` calls."""
        self._io.close()
