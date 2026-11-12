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
"""Tests for the core pw_transport ABCs."""

from __future__ import annotations

import math
import unittest
from unittest import mock

import pw_transport
from pw_transport import _socket


def _remaining(deadline: pw_transport.Deadline) -> float:
    remaining = deadline.remaining()
    assert remaining is not None
    return remaining


class _StubSocket(pw_transport.ReliableSocket):
    def __init__(
        self, incoming: list[bytes], max_write_message_size: int | None = None
    ) -> None:
        self._incoming = list(incoming)
        self._max_write_message_size = max_write_message_size
        self.outgoing: list[bytes] = []
        self.deadlines: list[pw_transport.Deadline] = []
        self._closed = False

    @property
    def max_write_message_size(self) -> int | None:
        return self._max_write_message_size

    @property
    def closed(self) -> bool:
        return self._closed

    def _read(self, deadline: pw_transport.Deadline) -> bytes:
        self.deadlines.append(deadline)
        if self._closed or not self._incoming:
            self._closed = True
            raise pw_transport.SocketClosed('closed')
        return self._incoming.pop(0)

    def _write(self, data: bytes, deadline: pw_transport.Deadline) -> None:
        self.deadlines.append(deadline)
        if self._closed:
            raise pw_transport.SocketClosed('closed')
        self.outgoing.append(data)

    def close(self) -> None:
        self._closed = True


class _StubListener(pw_transport.ReliableListener):
    def __init__(self) -> None:
        self.deadlines: list[pw_transport.Deadline] = []
        self._closed = False

    @property
    def closed(self) -> bool:
        return self._closed

    def _accept(
        self, deadline: pw_transport.Deadline
    ) -> pw_transport.ReliableSocket:
        self.deadlines.append(deadline)
        raise pw_transport.SocketClosed('closed')

    def close(self) -> None:
        self._closed = True


class SocketInterfaceTest(unittest.TestCase):
    """Tests Socket and Listener concrete methods and helpers."""

    def test_socket_context_manager_and_iteration(self) -> None:
        with _StubSocket([b'first', b'', b'last']) as sock:
            self.assertFalse(sock.closed)
            self.assertEqual(list(sock), [b'first', b'', b'last'])
            self.assertTrue(sock.closed)

        with _StubSocket([b'unused']) as sock2:
            sock2.write(b'hello')
        self.assertTrue(sock2.closed)
        sock2.close()  # Repeated close() is a no-op.
        self.assertTrue(sock2.closed)

    def test_default_message_size_limits_are_read_only(self) -> None:
        sock = _StubSocket([])
        self.assertIsNone(sock.max_read_message_size)
        self.assertIsNone(sock.max_write_message_size)
        with self.assertRaises(AttributeError):
            setattr(sock, 'max_read_message_size', 16)
        with self.assertRaises(AttributeError):
            setattr(sock, 'max_write_message_size', 16)

    def test_listener_context_manager(self) -> None:
        listener: pw_transport.Listener[pw_transport.Socket] = _StubListener()
        with listener:
            self.assertFalse(listener.closed)
        self.assertTrue(listener.closed)
        listener.close()  # Repeated close() is a no-op.
        self.assertTrue(listener.closed)

    def test_timeouts_are_converted_to_deadlines(self) -> None:
        sock = _StubSocket([b'first', b'second'])
        listener = _StubListener()

        self.assertEqual(sock.read(), b'first')
        self.assertEqual(sock.read(timeout=1.5), b'second')
        sock.write(b'hello', timeout=0)
        with self.assertRaises(pw_transport.SocketClosed):
            listener.accept(timeout=math.inf)

        self.assertEqual(len(sock.deadlines), 3)
        self.assertIsNone(sock.deadlines[0].remaining())
        self.assertGreater(_remaining(sock.deadlines[1]), 0)
        self.assertLessEqual(_remaining(sock.deadlines[1]), 1.5)
        self.assertTrue(sock.deadlines[2].expired())
        self.assertEqual(len(listener.deadlines), 1)
        self.assertIsNone(listener.deadlines[0].remaining())

    def test_invalid_timeouts_never_reach_implementation(self) -> None:
        sock = _StubSocket([b'unread'])
        listener = _StubListener()

        for invalid in (-0.001, -math.inf, math.nan):
            with self.assertRaises(ValueError):
                sock.read(timeout=invalid)
            with self.assertRaises(ValueError):
                sock.write(b'unsent', timeout=invalid)
            with self.assertRaises(ValueError):
                listener.accept(timeout=invalid)

        self.assertEqual(sock.deadlines, [])
        self.assertEqual(sock.outgoing, [])
        self.assertEqual(listener.deadlines, [])

    def test_oversized_writes_never_reach_implementation(self) -> None:
        sock = _StubSocket([], max_write_message_size=4)

        with self.assertRaises(ValueError):
            sock.write(b'12345')
        self.assertEqual(sock.outgoing, [])
        self.assertFalse(sock.closed)

        sock.write(b'1234')
        sock.write(b'')
        self.assertEqual(sock.outgoing, [b'1234', b''])


class DeadlineTest(unittest.TestCase):
    """Tests Deadline timeout validation and wait calculations."""

    def test_no_timeout_never_expires(self) -> None:
        for timeout in (None, math.inf):
            deadline = pw_transport.Deadline(timeout)
            self.assertFalse(deadline.expired())
            self.assertIsNone(deadline.remaining())

        self.assertIsNone(pw_transport.Deadline().remaining())

    def test_invalid_timeout(self) -> None:
        for invalid in (-0.001, -math.inf, math.nan):
            with self.assertRaises(ValueError):
                pw_transport.Deadline(invalid)

    def test_zero_timeout_expires_immediately(self) -> None:
        deadline = pw_transport.Deadline(0)
        self.assertTrue(deadline.expired())
        self.assertEqual(deadline.remaining(), 0)

    def test_counts_down_until_expired(self) -> None:
        with mock.patch.object(_socket.time, 'monotonic') as now:
            now.return_value = 100.0
            deadline = pw_transport.Deadline(1.5)
            self.assertFalse(deadline.expired())
            self.assertEqual(deadline.remaining(), 1.5)

            now.return_value = 101.0
            self.assertFalse(deadline.expired())
            self.assertEqual(deadline.remaining(), 0.5)

            now.return_value = 101.5
            self.assertTrue(deadline.expired())
            self.assertEqual(deadline.remaining(), 0)

            now.return_value = 200.0
            self.assertTrue(deadline.expired())
            self.assertEqual(deadline.remaining(), 0)


if __name__ == '__main__':
    unittest.main()
