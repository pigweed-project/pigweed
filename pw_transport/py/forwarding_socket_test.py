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
"""Tests for pw_transport.forwarding_socket."""

from __future__ import annotations

import math
import threading
import unittest

import pw_transport
from pw_transport.forwarding_socket import socket_pair


class ForwardingSocketTest(unittest.TestCase):
    """Tests in-memory socket_pair() semantics."""

    def test_bidirectional_forwarding_and_zero_length(self) -> None:
        first, second = socket_pair()
        with first, second:
            buf = bytearray(b'hello')
            first.write(buf)
            buf[0] = ord('X')  # Mutating caller buffer must not mutate queue.
            first.write(b'')
            second.write(b'reply')

            self.assertEqual(second.read(), b'hello')
            self.assertEqual(second.read(), b'')
            self.assertEqual(first.read(), b'reply')

    def test_max_message_size(self) -> None:
        first, second = socket_pair(max_message_size=4)
        self.assertEqual(first.max_write_message_size, 4)
        self.assertEqual(first.max_read_message_size, 4)
        self.assertEqual(second.max_write_message_size, 4)
        self.assertEqual(second.max_read_message_size, 4)

        with self.assertRaises(ValueError):
            first.write(b'12345')
        self.assertFalse(first.closed)

        first.write(b'1234')
        self.assertEqual(second.read(), b'1234')

    def test_invalid_options(self) -> None:
        with self.assertRaises(ValueError):
            socket_pair(queue_depth=0)
        with self.assertRaises(ValueError):
            socket_pair(max_message_size=-1)

    def test_peer_close_drains_written_datagrams_and_blocks_writes(
        self,
    ) -> None:
        first, second = socket_pair()
        first.write(b'one')
        first.write(b'two')
        second.write(b'discarded')

        first.close()
        first.close()  # Repeated close() must not disturb peer's queued reads.
        self.assertTrue(first.closed)
        # second still has two unread datagrams queued before closing.
        self.assertFalse(second.closed)

        # Writes fail in both directions once one endpoint closes.
        with self.assertRaises(pw_transport.SocketClosed):
            first.write(b'nope')
        with self.assertRaises(pw_transport.SocketClosed):
            second.write(b'nope')
        with self.assertRaises(pw_transport.SocketClosed):
            first.read()

        # second drains committed datagrams, then closes.
        self.assertEqual(second.read(), b'one')
        self.assertFalse(second.closed)
        self.assertEqual(second.read(), b'two')
        self.assertTrue(second.closed)
        with self.assertRaises(pw_transport.SocketClosed):
            second.read()

    def test_both_closed_discards_unread_immediately(self) -> None:
        first, second = socket_pair()
        with first, second:
            first.write(b'unclaimed')
        self.assertTrue(first.closed)
        self.assertTrue(second.closed)
        with self.assertRaises(pw_transport.SocketClosed):
            second.read()

    def test_queue_depth_backpressure_and_close_wakeup(self) -> None:
        first, second = socket_pair(queue_depth=1)
        first.write(b'slot-1')

        second_write_done = threading.Event()
        writer_error: list[pw_transport.SocketClosed] = []

        def blocked_writer() -> None:
            try:
                first.write(b'slot-2')
                second_write_done.set()
                first.write(b'slot-3-blocked-until-close')
            except pw_transport.SocketClosed as err:
                writer_error.append(err)

        thread = threading.Thread(target=blocked_writer)
        thread.start()
        try:
            self.assertFalse(second_write_done.wait(timeout=0.05))
            self.assertEqual(second.read(), b'slot-1')
            self.assertTrue(second_write_done.wait(timeout=5.0))
            self.assertEqual(second.read(), b'slot-2')
        finally:
            second.close()
            thread.join(timeout=5.0)

        self.assertFalse(thread.is_alive())
        self.assertEqual(len(writer_error), 1)

    def test_read_and_write_timeouts(self) -> None:
        first, second = socket_pair(queue_depth=1)
        with first, second:
            for bad in (-1.0, math.nan):
                with self.assertRaises(ValueError):
                    second.read(timeout=bad)
                with self.assertRaises(ValueError):
                    first.write(b'x', timeout=bad)

            with self.assertRaises(TimeoutError):
                second.read(timeout=0.0)
            with self.assertRaises(TimeoutError):
                second.read(timeout=0.01)
            self.assertFalse(second.closed)

            first.write(b'slot-1', timeout=0.0)
            with self.assertRaises(TimeoutError):
                first.write(b'blocked', timeout=0.0)
            with self.assertRaises(TimeoutError):
                first.write(b'blocked', timeout=0.01)
            self.assertFalse(first.closed)

            self.assertEqual(second.read(timeout=0.0), b'slot-1')
            first.write(b'slot-2', timeout=math.inf)
            self.assertEqual(second.read(timeout=math.inf), b'slot-2')

    def test_concurrent_reader_guard(self) -> None:
        first, _ = socket_pair()
        errors: list[type[BaseException]] = []

        def reader() -> None:
            try:
                first.read()
            except RuntimeError as err:
                errors.append(type(err))
                first.close()
            except pw_transport.SocketClosed as err:
                errors.append(type(err))

        thread = threading.Thread(target=reader)
        thread.start()
        try:
            reader()
        finally:
            first.close()
            thread.join(timeout=5.0)

        self.assertFalse(thread.is_alive())
        self.assertCountEqual(errors, [RuntimeError, pw_transport.SocketClosed])


if __name__ == '__main__':
    unittest.main()
