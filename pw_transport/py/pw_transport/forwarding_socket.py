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
"""In-memory connected pairs of ReliableSocket endpoints."""

from __future__ import annotations

import collections
import threading

from pw_transport._socket import Deadline, ReliableSocket, SocketClosed

__all__ = ['socket_pair']


class _ForwardingSocket(ReliableSocket):
    """One endpoint of an in-memory socket pair."""

    # Both endpoints of a pair inspect and mutate each other's state under the
    # shared condition variable.
    # pylint: disable=protected-access

    def __init__(
        self,
        cond: threading.Condition,
        *,
        queue_depth: int | None,
        max_message_size: int | None,
    ) -> None:
        self._cond = cond
        self._queue_depth = queue_depth
        self._max_message_size = max_message_size
        self._incoming: collections.deque[bytes] = collections.deque()
        self._local_closed = False
        self._reading = False
        self._peer: _ForwardingSocket

    @property
    def max_read_message_size(self) -> int | None:
        return self._max_message_size

    @property
    def max_write_message_size(self) -> int | None:
        return self._max_message_size

    def _is_closed(self) -> bool:
        return self._local_closed or (
            self._peer._local_closed and not self._incoming
        )

    @property
    def closed(self) -> bool:
        with self._cond:
            return self._is_closed()

    def _read(self, deadline: Deadline) -> bytes:
        with self._cond:
            if self._reading:
                raise RuntimeError(
                    'Another read() is already in progress on this socket'
                )
            if self._is_closed():
                raise SocketClosed('The socket is closed')

            self._reading = True
            try:
                while not self._incoming:
                    if self._is_closed():
                        raise SocketClosed('The socket is closed')
                    if deadline.expired():
                        raise TimeoutError('Timed out waiting for datagram')
                    self._cond.wait(deadline.remaining())

                datagram = self._incoming.popleft()
                self._cond.notify_all()
                return datagram
            finally:
                self._reading = False

    def _write(self, data: bytes, deadline: Deadline) -> None:
        payload = bytes(data)
        peer = self._peer

        with self._cond:
            if self._local_closed or peer._local_closed:
                raise SocketClosed('The socket is closed')

            while (
                peer._queue_depth is not None
                and len(peer._incoming) >= peer._queue_depth
            ):
                if deadline.expired():
                    raise TimeoutError('Timed out waiting to write datagram')
                self._cond.wait(deadline.remaining())
                if self._local_closed or peer._local_closed:
                    raise SocketClosed('The socket is closed')

            peer._incoming.append(payload)
            self._cond.notify_all()

    def close(self) -> None:
        with self._cond:
            self._local_closed = True
            self._incoming.clear()
            self._cond.notify_all()


def socket_pair(
    *,
    queue_depth: int | None = None,
    max_message_size: int | None = None,
) -> tuple[ReliableSocket, ReliableSocket]:
    """Returns a connected ``(first, second)`` pair of in-memory sockets.

    Writes on one socket are delivered as reads on the other, mirroring
    ``pw::transport::ForwardingSocketPair``. Closing one socket prevents further
    writes in both directions while allowing the peer to finish reading
    datagrams that were already written.

    Args:
      queue_depth: Maximum number of unread datagrams buffered in each direction
        before ``write()`` blocks, or ``None`` for an unbounded queue. Must be
        at least 1 when set.
      max_message_size: Maximum datagram size in bytes in each direction, or
        ``None`` if unlimited.

    Raises:
      ValueError: ``queue_depth`` is less than 1 or ``max_message_size`` is
        negative.
    """
    if queue_depth is not None and queue_depth < 1:
        raise ValueError(f'queue_depth must be at least 1, got {queue_depth}')
    if max_message_size is not None and max_message_size < 0:
        raise ValueError(
            f'max_message_size must be non-negative, got {max_message_size}'
        )

    cond = threading.Condition(threading.Lock())
    first = _ForwardingSocket(
        cond, queue_depth=queue_depth, max_message_size=max_message_size
    )
    second = _ForwardingSocket(
        cond, queue_depth=queue_depth, max_message_size=max_message_size
    )
    # pylint: disable=protected-access
    first._peer = second
    second._peer = first
    return first, second
