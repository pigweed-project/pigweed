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
"""Core datagram socket and listener interfaces."""

from __future__ import annotations

import abc
from collections.abc import Iterator
import math
import time
from typing import Generic, TypeVar, final

_SelfT = TypeVar('_SelfT')
_SocketT_co = TypeVar('_SocketT_co', bound='Socket', covariant=True)


class Deadline:
    """The point in time at which a blocking operation gives up.

    :py:class:`Socket` and :py:class:`Listener` convert each caller-provided
    timeout to a ``Deadline`` before calling into the implementation, so
    implementations never handle unvalidated timeouts or compute their own
    deadlines.

    Pass :py:meth:`remaining` to blocking calls and raise ``TimeoutError`` once
    the deadline has :py:meth:`expired`:

    .. code-block:: py

       while not ready():
           if deadline.expired():
               raise TimeoutError('Timed out')
           condition.wait(deadline.remaining())
    """

    def __init__(self, timeout: float | None = None) -> None:
        """Starts a deadline that expires ``timeout`` seconds from now.

        Args:
          timeout: Maximum time to wait in seconds, or ``None`` (or
            ``math.inf``) to never expire.

        Raises:
          ValueError: ``timeout`` is negative or NaN.
        """
        self._end: float | None
        if timeout is None or timeout == math.inf:
            self._end = None
            return
        if timeout < 0 or math.isnan(timeout):
            raise ValueError(
                f'timeout must be a non-negative number or None, got {timeout}'
            )
        self._end = time.monotonic() + timeout

    def expired(self) -> bool:
        """True if the deadline has passed; the operation should give up."""
        return self._end is not None and time.monotonic() >= self._end

    def remaining(self) -> float | None:
        """Returns the time left to wait, for use as a timeout.

        Returns:
          ``None`` if the deadline never expires; otherwise the seconds left
          until it does, which is at least 0.
        """
        if self._end is None:
            return None
        return max(0.0, self._end - time.monotonic())


class SocketClosed(ConnectionError):
    """The socket or listener is closed; no more operations can complete."""


class Socket(abc.ABC):
    """A bidirectional datagram-oriented connection to a peer.

    This is the Python counterpart of ``pw::transport::Socket``. Every datagram
    is delivered whole without merging or splitting; framing is the transport's
    responsibility. Unlike :py:class:`ReliableSocket`, a basic ``Socket`` may
    drop, duplicate, or reorder datagrams if the underlying link is unreliable.

    Zero-length datagrams (``b''``) are valid in both directions and are
    distinct from closure, which raises :py:class:`SocketClosed`.

    Only one thread may call :py:meth:`read` at a time, while any number of
    threads may call :py:meth:`write` concurrently. :py:meth:`close` may be
    called from any thread and unblocks pending :py:meth:`read` and
    :py:meth:`write` calls.

    Implementations override :py:meth:`_read` and :py:meth:`_write` rather than
    :py:meth:`read` and :py:meth:`write`, which validate their arguments before
    calling into the implementation.
    """

    @property
    def max_read_message_size(self) -> int | None:
        """Largest datagram that can be read from this socket, in bytes.

        Returns ``None`` if there is no limit.
        """
        return None

    @property
    def max_write_message_size(self) -> int | None:
        """Largest datagram that can be written to this socket, in bytes.

        Returns ``None`` if there is no limit. :py:meth:`write` rejects larger
        datagrams with ``ValueError``.
        """
        return None

    @property
    @abc.abstractmethod
    def closed(self) -> bool:
        """True if the socket is closed."""

    @final
    def read(self, timeout: float | None = None) -> bytes:
        """Blocks until one whole datagram is received and returns it.

        Only one thread may call ``read()`` at a time. If ``timeout`` expires,
        ``TimeoutError`` is raised without closing the socket or discarding any
        in-flight datagram.

        Args:
          timeout: Maximum time to wait in seconds, or ``None`` (or
            ``math.inf``) to wait indefinitely.

        Raises:
          SocketClosed: The socket is closed.
          TimeoutError: No complete datagram arrived before ``timeout``.
          RuntimeError: Another ``read()`` is already in progress.
          ValueError: ``timeout`` is negative or NaN.
        """
        return self._read(Deadline(timeout))

    @final
    def write(self, data: bytes, timeout: float | None = None) -> None:
        """Writes one whole datagram, blocking while backpressure applies.

        Args:
          data: Outgoing datagram payload.
          timeout: Maximum time to wait in seconds while backpressure applies,
            or ``None`` (or ``math.inf``) to wait indefinitely.

        Raises:
          SocketClosed: The socket is closed.
          TimeoutError: The datagram could not be sent before ``timeout``.
            Stream-backed transports that have already transmitted part of the
            frame close the socket before raising ``TimeoutError``.
          ValueError: ``data`` exceeds :py:attr:`max_write_message_size`, or
            ``timeout`` is negative or NaN.
        """
        deadline = Deadline(timeout)
        max_size = self.max_write_message_size
        if max_size is not None and len(data) > max_size:
            raise ValueError(
                f'Outgoing {len(data)}-byte datagram exceeds the '
                f'{max_size}-byte limit'
            )
        self._write(data, deadline)

    @abc.abstractmethod
    def _read(self, deadline: Deadline) -> bytes:
        """Implements :py:meth:`read`, which documents the required behavior.

        Args:
          deadline: When to give up and raise ``TimeoutError``.
        """

    @abc.abstractmethod
    def _write(self, data: bytes, deadline: Deadline) -> None:
        """Implements :py:meth:`write`, which documents the required behavior.

        Args:
          data: Outgoing datagram payload, which is no larger than
            :py:attr:`max_write_message_size`.
          deadline: When to give up and raise ``TimeoutError``.
        """

    @abc.abstractmethod
    def close(self) -> None:
        """Closes the socket and unblocks pending operations.

        Calling ``close()`` on an already-closed socket is a no-op.
        """

    def __enter__(self: _SelfT) -> _SelfT:
        return self

    def __exit__(self, *exc_info: object) -> None:
        self.close()

    def __iter__(self) -> Iterator[bytes]:
        """Yields received datagrams until the socket closes."""
        while True:
            try:
                yield self.read()
            except SocketClosed:
                return


class ReliableSocket(Socket):
    """A reliable, ordered, datagram-oriented connection to a peer.

    This is the Python counterpart of ``pw::transport::ReliableSocket``. Unless
    the connection fails and raises :py:class:`SocketClosed`, datagrams written
    by one peer are delivered whole, in order, and without duplication.
    """


class Listener(abc.ABC, Generic[_SocketT_co]):
    """Accepts incoming :py:class:`Socket` connections from peers.

    This is the Python counterpart of ``pw::transport::Listener``. Closing a
    listener stops accepting new connections without closing sockets that were
    already accepted.

    Multiple threads may call :py:meth:`accept` concurrently; when multiple
    calls are pending, connections are distributed in an unspecified order.
    :py:meth:`close` may be called from any thread and unblocks pending
    :py:meth:`accept` calls.

    Implementations override :py:meth:`_accept` rather than :py:meth:`accept`,
    which validates its arguments before calling into the implementation.
    """

    @property
    @abc.abstractmethod
    def closed(self) -> bool:
        """True if the listener is closed."""

    @final
    def accept(self, timeout: float | None = None) -> _SocketT_co:
        """Blocks until a peer connects and returns the accepted socket.

        Multiple threads may call ``accept()`` concurrently; the order in which
        pending calls receive connections is unspecified.

        Args:
          timeout: Maximum time to wait in seconds, or ``None`` (or
            ``math.inf``) to wait indefinitely.

        Raises:
          SocketClosed: The listener is closed.
          TimeoutError: No connection arrived before ``timeout``.
          ValueError: ``timeout`` is negative or NaN.
        """
        return self._accept(Deadline(timeout))

    @abc.abstractmethod
    def _accept(self, deadline: Deadline) -> _SocketT_co:
        """Implements :py:meth:`accept`, which documents the required behavior.

        Args:
          deadline: When to give up and raise ``TimeoutError``.
        """

    @abc.abstractmethod
    def close(self) -> None:
        """Stops listening and unblocks pending :py:meth:`accept` calls.

        Calling ``close()`` on an already-closed listener is a no-op.
        """

    def __enter__(self: _SelfT) -> _SelfT:
        return self

    def __exit__(self, *exc_info: object) -> None:
        self.close()


class ReliableListener(Listener[ReliableSocket]):
    """Accepts incoming :py:class:`ReliableSocket` connections from peers.

    This is the Python counterpart of ``pw::transport::ReliableListener``.
    """
