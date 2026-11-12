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
"""Tests for pw_transport.framed_tcp."""

from __future__ import annotations

import math
import socket
import struct
import threading
import unittest

import pw_transport
from pw_transport import framed_tcp
from pw_transport.framed_tcp import FramedTcpListener


def _recv_exact(sock: socket.socket, count: int) -> bytes:
    buf = bytearray()
    while len(buf) < count:
        chunk = sock.recv(count - len(buf))
        if not chunk:
            raise EOFError('Socket closed before receiving expected bytes')
        buf.extend(chunk)
    return bytes(buf)


class FramedTcpWireTest(unittest.TestCase):
    """Tests framed_tcp wire framing, limits, timeouts, and EOF."""

    def setUp(self) -> None:
        with FramedTcpListener(host='127.0.0.1', max_message_size=16) as (
            listener
        ):
            self.server_raw = socket.create_connection(
                ('127.0.0.1', listener.port), timeout=5.0
            )
            self.addCleanup(self.server_raw.close)
            self.sock = listener.accept(timeout=5.0)
            self.addCleanup(self.sock.close)

    def test_write_and_zero_length_datagrams(self) -> None:
        self.sock.write(b'hello')
        self.sock.write(b'')
        self.assertEqual(
            _recv_exact(self.server_raw, 13),
            b'\x05\x00\x00\x00hello\x00\x00\x00\x00',
        )

    def test_read_reassembles_fragmented_frames_and_zero_length(self) -> None:
        self.server_raw.sendall(b'\x00\x00')
        self.server_raw.sendall(b'\x00\x00')
        self.assertEqual(self.sock.read(), b'')

        self.server_raw.sendall(b'\x0b\x00')
        self.server_raw.sendall(b'\x00\x00hello ')
        self.server_raw.sendall(b'world')
        self.assertEqual(self.sock.read(), b'hello world')

    def test_outgoing_size_limit(self) -> None:
        self.assertEqual(self.sock.max_read_message_size, 16)
        self.assertEqual(self.sock.max_write_message_size, 16)

        self.sock.write(b'x' * 16)
        with self.assertRaisesRegex(
            ValueError, r'^Outgoing 17-byte datagram exceeds'
        ):
            self.sock.write(b'x' * 17)
        self.assertFalse(self.sock.closed)

    def test_incoming_size_limit_closes_socket(self) -> None:
        self.server_raw.sendall(struct.pack('<I', 17) + b'x' * 17)
        with self.assertRaisesRegex(
            pw_transport.SocketClosed, r'^Incoming 17-byte datagram exceeds'
        ):
            self.sock.read()
        self.assertTrue(self.sock.closed)

    def test_clean_eof(self) -> None:
        self.server_raw.close()
        with self.assertRaises(pw_transport.SocketClosed):
            self.sock.read()
        self.assertTrue(self.sock.closed)
        with self.assertRaises(pw_transport.SocketClosed):
            self.sock.write(b'after close')

    def test_mid_header_eof(self) -> None:
        self.server_raw.sendall(b'\x04\x00')
        self.server_raw.close()
        with self.assertRaises(pw_transport.SocketClosed):
            self.sock.read()
        self.assertTrue(self.sock.closed)

    def test_mid_payload_eof(self) -> None:
        self.server_raw.sendall(b'\x05\x00\x00\x00hi')
        self.server_raw.close()
        with self.assertRaises(pw_transport.SocketClosed):
            self.sock.read()
        self.assertTrue(self.sock.closed)

    def test_read_timeout_is_resumable_idle_mid_header_and_mid_payload(
        self,
    ) -> None:
        with self.assertRaises(ValueError):
            self.sock.read(timeout=-0.1)
        with self.assertRaises(ValueError):
            self.sock.read(timeout=math.nan)

        # Idle timeout leaves the socket open.
        with self.assertRaises(TimeoutError):
            self.sock.read(timeout=0.01)
        self.assertFalse(self.sock.closed)

        # Mid-header timeout keeps buffered prefix and resumes cleanly.
        self.server_raw.sendall(b'\x05\x00')
        with self.assertRaises(TimeoutError):
            self.sock.read(timeout=0.01)
        self.assertFalse(self.sock.closed)

        # Mid-payload timeout keeps decoded length + partial payload.
        self.server_raw.sendall(b'\x00\x00ab')
        with self.assertRaises(TimeoutError):
            self.sock.read(timeout=0.01)
        self.assertFalse(self.sock.closed)

        # Delivering the remaining bytes completes the datagram, plus a second
        # back-to-back frame.
        self.server_raw.sendall(b'cde\x02\x00\x00\x00ok')
        self.assertEqual(self.sock.read(timeout=1.0), b'abcde')
        self.assertEqual(self.sock.read(timeout=0.0), b'ok')

    def test_write_timeout_under_backpressure(self) -> None:
        """Verifies write(timeout=...) raises TimeoutError when full."""
        with self.assertRaises(ValueError):
            self.sock.write(b'hi', timeout=-1.0)

        with FramedTcpListener(host='127.0.0.1') as listener:
            with (
                framed_tcp.connect('127.0.0.1', listener.port) as client,
                listener.accept(timeout=5.0) as _server,
            ):
                chunk = b'x' * (64 * 1024)
                with self.assertRaises(TimeoutError):
                    while True:
                        client.write(chunk, timeout=0.0)

    def test_close_from_another_thread_unblocks_read(self) -> None:
        started = threading.Event()
        errors: list[pw_transport.SocketClosed] = []

        def reader() -> None:
            started.set()
            try:
                self.sock.read()
            except pw_transport.SocketClosed as err:
                errors.append(err)

        thread = threading.Thread(target=reader)
        thread.start()
        self.assertTrue(started.wait(timeout=5.0))
        threading.Timer(0.02, self.sock.close).start()
        thread.join(timeout=5.0)
        self.assertFalse(thread.is_alive())
        self.assertEqual(len(errors), 1)
        self.assertIsInstance(errors[0], pw_transport.SocketClosed)


class FramedTcpLoopbackTest(unittest.TestCase):
    """Tests FramedTcpListener and connect() over loopback TCP."""

    def test_invalid_options(self) -> None:
        with self.assertRaises(ValueError):
            framed_tcp.connect('127.0.0.1', 80, max_message_size=-1)
        with self.assertRaises(ValueError):
            framed_tcp.connect('127.0.0.1', 80, max_message_size=0x1_0000_0000)
        with self.assertRaises(ValueError):
            framed_tcp.connect('127.0.0.1', 80, timeout=-1.0)
        with self.assertRaises(ValueError):
            FramedTcpListener(max_message_size=-1)

    def test_default_listener_accepts_connections(self) -> None:
        with FramedTcpListener() as listener:
            for host in ('127.0.0.1', 'localhost'):
                with (
                    framed_tcp.connect(
                        host, listener.port, timeout=5.0
                    ) as client,
                    listener.accept(timeout=5.0) as server,
                ):
                    client.write(b'hello')
                    self.assertEqual(server.read(timeout=5.0), b'hello')

    def test_localhost_listener_accepts_ipv4_connections(self) -> None:
        with FramedTcpListener(host='localhost') as listener:
            with (
                framed_tcp.connect(
                    '127.0.0.1', listener.port, timeout=5.0
                ) as client,
                listener.accept(timeout=5.0) as server,
            ):
                client.write(b'hello')
                self.assertEqual(server.read(timeout=5.0), b'hello')

    def test_listen_connect_and_listener_close_keeps_accepted_open(
        self,
    ) -> None:
        with FramedTcpListener(host='127.0.0.1', max_message_size=128) as srv:
            self.assertGreater(srv.port, 0)
            client = framed_tcp.connect(
                '127.0.0.1', srv.port, timeout=5.0, max_message_size=128
            )
            self.addCleanup(client.close)
            accepted = srv.accept(timeout=5.0)
            self.addCleanup(accepted.close)

        # Listener exited its context manager, so it is closed, while the
        # already-accepted connection continues exchanging datagrams.
        self.assertTrue(srv.closed)
        srv.close()  # Repeated close() is a no-op.
        self.assertTrue(srv.closed)
        client.write(b'ping', timeout=5.0)
        self.assertEqual(accepted.read(timeout=5.0), b'ping')
        accepted.write(b'pong', timeout=5.0)
        self.assertEqual(client.read(timeout=5.0), b'pong')
        client.close()
        client.close()  # Repeated close() is a no-op.
        self.assertTrue(client.closed)

    def test_module_connect_helper_and_accept_timeout(self) -> None:
        with FramedTcpListener(host='127.0.0.1') as listener:
            with self.assertRaises(TimeoutError):
                listener.accept(timeout=0.01)
            self.assertFalse(listener.closed)

            with framed_tcp.connect(
                '127.0.0.1', listener.port, timeout=5.0
            ) as client:
                with listener.accept(timeout=5.0) as server:
                    client.write(b'hello')
                    self.assertEqual(server.read(), b'hello')

    def test_concurrent_accept(self) -> None:
        """Verifies multiple threads can call accept() concurrently."""
        with FramedTcpListener(host='127.0.0.1') as listener:
            barrier = threading.Barrier(3)
            received: list[bytes] = []
            lock = threading.Lock()
            errors: list[BaseException] = []

            def acceptor() -> None:
                try:
                    barrier.wait(timeout=5.0)
                    with listener.accept(timeout=5.0) as server:
                        data = server.read(timeout=5.0)
                    with lock:
                        received.append(data)
                except BaseException as err:  # pylint: disable=broad-except
                    with lock:
                        errors.append(err)

            threads = [threading.Thread(target=acceptor) for _ in range(2)]
            for thread in threads:
                thread.start()

            barrier.wait(timeout=5.0)
            with (
                framed_tcp.connect(
                    '127.0.0.1', listener.port, timeout=5.0
                ) as client1,
                framed_tcp.connect(
                    '127.0.0.1', listener.port, timeout=5.0
                ) as client2,
            ):
                client1.write(b'first', timeout=5.0)
                client2.write(b'second', timeout=5.0)
                for thread in threads:
                    thread.join(timeout=5.0)

            self.assertEqual(errors, [])
            for thread in threads:
                self.assertFalse(thread.is_alive())
            self.assertCountEqual(received, [b'first', b'second'])

    def test_listener_close_from_another_thread_unblocks_accept(self) -> None:
        listener = FramedTcpListener(host='127.0.0.1')
        barrier = threading.Barrier(3)
        errors: list[pw_transport.SocketClosed] = []
        lock = threading.Lock()

        def acceptor() -> None:
            barrier.wait(timeout=5.0)
            try:
                listener.accept()
            except pw_transport.SocketClosed as err:
                with lock:
                    errors.append(err)

        threads = [threading.Thread(target=acceptor) for _ in range(2)]
        for thread in threads:
            thread.start()
        barrier.wait(timeout=5.0)
        threading.Timer(0.02, listener.close).start()
        for thread in threads:
            thread.join(timeout=5.0)
            self.assertFalse(thread.is_alive())
        self.assertEqual(len(errors), 2)
        for err in errors:
            self.assertIsInstance(err, pw_transport.SocketClosed)


if __name__ == '__main__':
    unittest.main()
