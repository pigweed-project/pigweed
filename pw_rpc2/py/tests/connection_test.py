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
"""Tests pw_rpc2._connection against FakeServer."""

import inspect
import math
import threading
import time
from typing import Any, Callable
import unittest
from unittest import mock

from pw_transport.forwarding_socket import socket_pair
import pw_rpc2
from pw_rpc2 import _descriptors as descriptors
from pw_rpc2._call import (
    BidirectionalStreamingCall,
    ClientStreamingCall,
    ServerStreamingCall,
    UnaryCall,
    resolve_timeout,
)
from pw_rpc2._connection import DEFAULT_HANDSHAKE_TIMEOUT, Connection
from pw_rpc2._descriptors import Method
from pw_rpc2._packets import (
    CloseMode,
    Handshake,
    HandshakeType,
    Packet,
    PacketFlag,
    PacketType,
    ProtocolError,
    RpcStatus,
    UnknownProtocolError,
    to_protocol_error,
)
from pw_rpc2_test_pb import pw_rpc2_test_pb2

from fake_server import FakeServer

# pylint: disable=protected-access

Request = pw_rpc2_test_pb2.EchoRequest
Response = pw_rpc2_test_pb2.EchoResponse

_SERVICE_ID = 0xFE6FE65C
_UNARY_ID = 0x8BA299DF

_TEST_SERVICE = next(iter(descriptors.services([pw_rpc2_test_pb2])))
_UNARY_METHOD = _TEST_SERVICE.methods['EchoUnary']
_SERVER_STREAM_METHOD = _TEST_SERVICE.methods['CountUpServerStream']
_CLIENT_STREAM_METHOD = _TEST_SERVICE.methods['AccumulateClientStream']
_BIDI_STREAM_METHOD = _TEST_SERVICE.methods['EchoBidiStream']


def _response(val: int) -> bytes:
    return Response(val=val).SerializeToString()


def _wait_until(condition: Callable[[], bool], timeout: float = 5.0) -> None:
    deadline = time.monotonic() + timeout
    while not condition():
        if time.monotonic() > deadline:
            raise AssertionError('Timed out waiting for condition')
        time.sleep(0.001)


class HandshakeTest(unittest.TestCase):
    """Tests the connection handshake."""

    def test_handshake(self) -> None:
        server = FakeServer()
        connection = Connection(server.socket, DEFAULT_HANDSHAKE_TIMEOUT)
        try:
            server.expect_handshake()
            self.assertFalse(connection.closed)
        finally:
            connection.close()
        self.assertTrue(connection.closed)
        self.assertTrue(server.socket.closed)

    def test_wrong_reply_type(self) -> None:
        server = FakeServer(Handshake(HandshakeType.SYN).encode())
        with self.assertRaisesRegex(
            pw_rpc2.HandshakeError, 'SYN_ACK'
        ) as context:
            Connection(server.socket, DEFAULT_HANDSHAKE_TIMEOUT)
        self.assertIs(context.exception.status, RpcStatus.PROTOCOL_ERROR)
        self.assertTrue(server.socket.closed)

    def test_not_a_handshake(self) -> None:
        server = FakeServer(Packet.response(1).encode())
        with self.assertRaisesRegex(
            pw_rpc2.HandshakeError, 'invalid reply'
        ) as context:
            Connection(server.socket, DEFAULT_HANDSHAKE_TIMEOUT)
        self.assertIs(context.exception.status, RpcStatus.PROTOCOL_ERROR)
        self.assertTrue(server.socket.closed)

    def test_unsupported_versions(self) -> None:
        for version in (0, 2):
            with self.subTest(version=version):
                server = FakeServer(
                    Handshake(HandshakeType.SYN_ACK, version).encode()
                )
                with self.assertRaisesRegex(
                    pw_rpc2.HandshakeError, 'version'
                ) as context:
                    Connection(server.socket, DEFAULT_HANDSHAKE_TIMEOUT)
                self.assertIs(
                    context.exception.status, RpcStatus.PROTOCOL_ERROR
                )

    def test_no_reply(self) -> None:
        server = FakeServer(handshake_reply=None)
        with self.assertRaisesRegex(
            pw_rpc2.HandshakeError, 'no reply'
        ) as context:
            Connection(server.socket, handshake_timeout=0.05)
        self.assertIs(context.exception.status, RpcStatus.PROTOCOL_ERROR)
        self.assertTrue(server.socket.closed)

    def test_handshake_write_timeout(self) -> None:
        client_sock, server_sock = socket_pair(queue_depth=1)
        self.addCleanup(server_sock.close)
        client_sock.write(b'occupy-queue')
        with self.assertRaisesRegex(
            pw_rpc2.HandshakeError, 'timed out sending SYN'
        ) as context:
            Connection(client_sock, handshake_timeout=0.02)
        self.assertIs(context.exception.status, RpcStatus.PROTOCOL_ERROR)
        self.assertTrue(client_sock.closed)

        # SYN fills the 1-message queue; if the server never reads it, sending
        # ACK after receiving SYN_ACK times out.
        client_sock, server_sock = socket_pair(queue_depth=1)
        self.addCleanup(server_sock.close)
        server_sock.write(Handshake(HandshakeType.SYN_ACK).encode())
        with self.assertRaisesRegex(
            pw_rpc2.HandshakeError, 'timed out sending ACK'
        ) as context:
            Connection(client_sock, handshake_timeout=0.02)
        self.assertIs(context.exception.status, RpcStatus.PROTOCOL_ERROR)
        self.assertTrue(client_sock.closed)

    def test_disconnected(self) -> None:
        server = FakeServer(handshake_reply=None)
        server.disconnect()
        with self.assertRaises(pw_rpc2.HandshakeError) as context:
            Connection(server.socket, DEFAULT_HANDSHAKE_TIMEOUT)
        self.assertIs(context.exception.status, RpcStatus.DISCONNECTED)

        server = FakeServer(handshake_reply=None)
        server.socket.close()
        with self.assertRaises(pw_rpc2.HandshakeError) as context:
            Connection(server.socket, DEFAULT_HANDSHAKE_TIMEOUT)
        self.assertIs(context.exception.status, RpcStatus.DISCONNECTED)

    def test_negative_handshake_timeout(self) -> None:
        server = FakeServer()
        self.addCleanup(server.socket.close)
        with self.assertRaises(ValueError):
            Connection(server.socket, handshake_timeout=-1)
        self.assertTrue(server.nothing_sent())


class ConnectionTestCase(unittest.TestCase):
    """Base for tests with an active Connection."""

    default_timeout: float | None = 5.0

    def setUp(self) -> None:
        self.server = FakeServer()
        self.connection = Connection(
            self.server.socket, DEFAULT_HANDSHAKE_TIMEOUT
        )
        self.server.expect_handshake()

    def tearDown(self) -> None:
        self.connection.close()

    def _start(
        self,
        method: Method,
        payload: bytes = b'',
        timeout: float | None = None,
    ) -> tuple[pw_rpc2._call.CallState, float]:
        resolved = resolve_timeout(timeout, self.default_timeout)
        return self.connection.start_call(method, payload), resolved

    def start_unary(
        self,
        request: Request | None = None,
        *,
        val: int | None = None,
        timeout: float | None = None,
    ) -> UnaryCall:
        if request is None:
            request = Request() if val is None else Request(val=val)
        state, resolved = self._start(
            _UNARY_METHOD, request.SerializeToString(), timeout
        )
        return UnaryCall(state, resolved)

    def start_server_stream(
        self,
        *,
        val: int = 0,
        timeout: float | None = None,
    ) -> ServerStreamingCall:
        state, resolved = self._start(
            _SERVER_STREAM_METHOD, Request(val=val).SerializeToString(), timeout
        )
        return ServerStreamingCall(state, resolved)

    def start_client_stream(
        self, *, timeout: float | None = None
    ) -> ClientStreamingCall:
        state, resolved = self._start(_CLIENT_STREAM_METHOD, timeout=timeout)
        return ClientStreamingCall(state, resolved)

    def start_bidi_stream(
        self, *, timeout: float | None = None
    ) -> BidirectionalStreamingCall:
        state, resolved = self._start(_BIDI_STREAM_METHOD, timeout=timeout)
        return BidirectionalStreamingCall(state, resolved)


class UnaryCallTest(ConnectionTestCase):
    """Tests unary call handles and single-response semantics."""

    def test_invoke_and_respond(self) -> None:
        call = self.start_unary(val=5)

        self.assertEqual(
            self.server.receive(),
            Packet.request(
                1,
                _SERVICE_ID,
                _UNARY_ID,
                Request(val=5).SerializeToString(),
            ),
        )
        self.assertIsNone(call.status)
        self.assertFalse(call.completed)

        self.server.send(Packet.response(call._call_id, _response(6)))

        self.assertEqual(call.response(), Response(val=6))
        self.assertIs(call.status, RpcStatus.OK)
        self.assertIsNone(call._protocol_error)
        self.assertTrue(call.completed)
        self.assertEqual(call.response(), Response(val=6))  # Cached
        self.assertIn('OK', repr(call))

    def test_concurrent_response_waiters(self) -> None:
        call = self.start_unary(val=5)
        self.server.receive()

        results: list[int] = []
        threads = [
            threading.Thread(target=lambda: results.append(call.response().val))
            for _ in range(4)
        ]
        for thread in threads:
            thread.start()

        self.server.send(Packet.response(call._call_id, _response(42)))
        for thread in threads:
            thread.join()

        self.assertEqual(results, [42, 42, 42, 42])

    def test_call_ids_count_up(self) -> None:
        ids = [self.start_unary()._call_id for _ in range(3)]
        self.assertEqual(ids, [1, 2, 3])
        self.assertEqual(
            [self.server.receive().call_id for _ in range(3)], [1, 2, 3]
        )

    def test_empty_response(self) -> None:
        call = self.start_unary()
        self.server.send(Packet.response(call._call_id))
        self.assertEqual(call.response(), Response())

    def test_server_error(self) -> None:
        call = self.start_unary()
        self.server.send(
            Packet.server_error(call._call_id, ProtocolError.UNKNOWN_METHOD)
        )

        with self.assertRaises(pw_rpc2.RpcError) as context:
            call.response()

        self.assertIs(context.exception.status, RpcStatus.UNIMPLEMENTED)
        self.assertIs(
            context.exception._protocol_error, ProtocolError.UNKNOWN_METHOD
        )
        self.assertEqual(context.exception.method, _UNARY_METHOD)
        self.assertIn('UNIMPLEMENTED', str(context.exception))
        self.assertIn('UNKNOWN_METHOD', str(context.exception))
        self.assertIs(call.status, RpcStatus.UNIMPLEMENTED)
        self.assertIs(call._protocol_error, ProtocolError.UNKNOWN_METHOD)

    def test_server_error_with_undefined_code(self) -> None:
        call = self.start_unary()
        self.server.send(
            Packet(
                PacketType(PacketFlag.SERVER, CloseMode.ERROR_TERMINAL),
                call._call_id,
                error=to_protocol_error(0x3F),
            )
        )

        with self.assertRaises(pw_rpc2.RpcError) as context:
            call.response()

        self.assertIs(context.exception.status, RpcStatus.PROTOCOL_ERROR)
        self.assertEqual(
            context.exception._protocol_error, UnknownProtocolError(0x3F)
        )
        self.assertNotIsInstance(
            context.exception._protocol_error, ProtocolError
        )
        self.assertIn('PROTOCOL_ERROR (UNKNOWN_0x3f)', str(context.exception))

    def _assert_method_type_mismatch(
        self, call: pw_rpc2.UnaryCall, *, reported: bool = True
    ) -> None:
        with self.assertRaises(pw_rpc2.RpcError) as context:
            call.response()
        self.assertIs(context.exception.status, RpcStatus.UNIMPLEMENTED)
        self.assertIs(
            context.exception._protocol_error,
            ProtocolError.METHOD_TYPE_MISMATCH,
        )

        self.server.receive()  # The request
        if reported:
            self.assertEqual(
                self.server.receive(),
                Packet.client_error(
                    call._call_id, ProtocolError.METHOD_TYPE_MISMATCH
                ),
            )
        else:
            self.assertTrue(self.server.nothing_sent())

    def test_server_finish_is_method_type_mismatch(self) -> None:
        call = self.start_unary()
        self.server.send(Packet.server_finish(call._call_id))
        # The packet ended the RPC, so there is nothing to report.
        self._assert_method_type_mismatch(call, reported=False)

    def test_server_stream_end_is_method_type_mismatch(self) -> None:
        call = self.start_unary()
        self.server.send(Packet.server_stream_end(call._call_id))
        self._assert_method_type_mismatch(call)

    def test_stream_message_is_method_type_mismatch(self) -> None:
        call = self.start_unary()
        self.server.send(Packet.server_message(call._call_id, _response(1)))
        # The call already failed, so the response is dropped.
        self.server.send(Packet.response(call._call_id, _response(2)))
        self._assert_method_type_mismatch(call)

    def test_half_close_is_method_type_mismatch(self) -> None:
        call = self.start_unary()
        self.server.send(
            Packet(
                PacketType(
                    PacketFlag.SERVER | PacketFlag.HAS_PAYLOAD,
                    CloseMode.STREAM_END,
                ),
                call._call_id,
                payload=_response(1),
            )
        )
        self._assert_method_type_mismatch(call)

    def test_undecodable_response(self) -> None:
        call = self.start_unary()
        self.server.send(Packet.response(call._call_id, b'\xff\xff\xff'))
        for _ in range(2):
            with self.assertRaises(pw_rpc2.DecodeError) as context:
                call.response()
            self.assertIs(context.exception.status, RpcStatus.DATA_ERROR)

    def test_timeout_does_not_end_invoked_calls(self) -> None:
        call = self.start_unary()
        with self.assertRaises(pw_rpc2.RpcTimeout) as context:
            call.response(timeout=0)
        self.assertIs(context.exception.method, _UNARY_METHOD)
        self.assertEqual(context.exception.timeout, 0)
        self.assertIsInstance(context.exception, TimeoutError)
        self.assertFalse(call.completed)

        self.server.send(Packet.response(call._call_id, _response(1)))
        self.assertEqual(call.response().val, 1)

    def test_default_timeouts(self) -> None:
        self.default_timeout = 0.01
        call = self.start_unary()
        with self.assertRaises(pw_rpc2.RpcTimeout):
            call.response()

        call = self.start_unary(timeout=0.02)
        with self.assertRaises(pw_rpc2.RpcTimeout) as context:
            call.response()
        self.assertEqual(context.exception.timeout, 0.02)

        call = self.start_unary(timeout=math.inf)
        self.server.send(Packet.response(call._call_id, _response(1)))
        self.assertEqual(call.response().val, 1)

    def test_request_too_large(self) -> None:
        server = FakeServer(max_packet_size=16)
        connection = Connection(server.socket, DEFAULT_HANDSHAKE_TIMEOUT)
        self.addCleanup(connection.close)
        server.expect_handshake()

        with self.assertRaisesRegex(ValueError, 'exceeds'):
            connection.start_call(
                _UNARY_METHOD, Request(val=0xFFFFFFFF).SerializeToString()
            )

        self.assertTrue(server.nothing_sent())

        # The connection remains usable.
        state = connection.start_call(
            _UNARY_METHOD, Request(val=1).SerializeToString()
        )
        self.assertEqual(server.receive().call_id, state.call_id)


class CancelTest(ConnectionTestCase):
    """Tests cancelling calls."""

    def test_cancel(self) -> None:
        call = self.start_server_stream()
        self.server.receive()

        self.assertTrue(call.cancel())
        self.assertIs(call.status, RpcStatus.CANCELLED)
        self.assertIs(call._protocol_error, ProtocolError.CANCELLED)
        self.assertEqual(
            self.server.receive(),
            Packet.client_error(call._call_id, ProtocolError.CANCELLED),
        )

        with self.assertRaises(pw_rpc2.RpcError) as context:
            call.read()
        self.assertIs(context.exception.status, RpcStatus.CANCELLED)
        self.assertIs(
            context.exception._protocol_error, ProtocolError.CANCELLED
        )

        self.assertFalse(call.cancel())
        self.assertTrue(self.server.nothing_sent())

    def test_cancel_completed_call(self) -> None:
        call = self.start_unary()
        self.server.receive()
        self.server.send(Packet.response(call._call_id, _response(1)))
        call.response()

        self.assertFalse(call.cancel())
        self.assertIs(call.status, RpcStatus.OK)
        self.assertTrue(self.server.nothing_sent())

    def test_context_manager_cancels(self) -> None:
        with self.start_bidi_stream() as call:
            self.server.receive()
        self.assertIs(call.status, RpcStatus.CANCELLED)
        self.assertEqual(
            self.server.receive().type,
            PacketType(close_mode=CloseMode.ERROR_TERMINAL),
        )

    def test_packets_after_cancel_are_ignored(self) -> None:
        call = self.start_server_stream()
        self.server.receive()
        call.cancel()
        self.assertEqual(
            self.server.receive(),
            Packet.client_error(call._call_id, ProtocolError.CANCELLED),
        )

        # A stray non-terminal packet for an inactive call gets a
        # RECEIVED_PACKET_FOR_UNKNOWN_CALL reply; a terminal packet is dropped
        # silently.
        self.server.send(Packet.server_message(call._call_id, _response(1)))
        self.assertEqual(
            self.server.receive(),
            Packet.client_error(
                call._call_id, ProtocolError.RECEIVED_PACKET_FOR_UNKNOWN_CALL
            ),
        )
        self.server.send(Packet.server_finish(call._call_id))

        # Use another call to know that the packets have been processed.
        other = self.start_unary()
        self.server.receive()
        self.server.send(Packet.response(other._call_id, _response(2)))
        other.response()

        self.assertIs(call.status, RpcStatus.CANCELLED)
        self.assertTrue(self.server.nothing_sent())

    def test_cancel_during_blocked_write(self) -> None:
        """Cancels without blocking when a write is stalled on backpressure."""
        server = FakeServer(queue_depth=2)
        connection = Connection(server.socket, DEFAULT_HANDSHAKE_TIMEOUT)
        writer: threading.Thread | None = None
        try:
            server.expect_handshake()
            call = BidirectionalStreamingCall(
                connection.start_call(_BIDI_STREAM_METHOD, b''), 5.0
            )
            other = UnaryCall(
                connection.start_call(
                    _UNARY_METHOD, Request(val=9).SerializeToString()
                ),
                5.0,
            )
            # Drain START packets so the 2-slot queue is empty.
            server.receive()
            server.receive()

            call.write(val=1)
            call.write(val=2)

            entered_write = threading.Event()
            original_write = server.socket._write

            def notify_write(data: bytes, deadline: Any) -> None:
                entered_write.set()
                original_write(data, deadline)

            def write_blocked() -> None:
                try:
                    call.write(val=3)
                except pw_rpc2.SocketClosed:
                    pass

            # Queue is now full; wait until the writer enters _socket._write().
            with mock.patch.object(
                server.socket, '_write', side_effect=notify_write
            ):
                writer = threading.Thread(target=write_blocked)
                writer.start()
                self.assertTrue(entered_write.wait(timeout=5.0))

            self.assertTrue(call.cancel())
            self.assertIs(call.status, RpcStatus.CANCELLED)
            self.assertTrue(writer.is_alive())

            # An inbound message arriving while CANCELLED is still deferred
            # behind the blocked write must be dropped without queueing a
            # premature RECEIVED_PACKET_FOR_UNKNOWN_CALL ahead of CANCELLED.
            server.send(Packet.server_message(call._call_id, _response(99)))
            server.send(Packet.response(other._call_id, _response(9)))
            self.assertEqual(other.response(timeout=1.0), Response(val=9))

            # Drain the queue; CANCELLED arrives after the in-flight write.
            self.assertEqual(
                server.receive(),
                Packet.client_message(
                    call._call_id, Request(val=1).SerializeToString()
                ),
            )
            self.assertEqual(
                server.receive(),
                Packet.client_message(
                    call._call_id, Request(val=2).SerializeToString()
                ),
            )
            writer.join(timeout=5.0)
            self.assertFalse(writer.is_alive())
            self.assertEqual(
                server.receive(),
                Packet.client_message(
                    call._call_id, Request(val=3).SerializeToString()
                ),
            )
            self.assertEqual(
                server.receive(),
                Packet.client_error(call._call_id, ProtocolError.CANCELLED),
            )
            self.assertTrue(server.nothing_sent())

            # Once CANCELLED has been queued, stray packets get UNKNOWN_CALL.
            server.send(Packet.server_message(call._call_id, _response(100)))
            self.assertEqual(
                server.receive(),
                Packet.client_error(
                    call._call_id,
                    ProtocolError.RECEIVED_PACKET_FOR_UNKNOWN_CALL,
                ),
            )
        finally:
            connection.close()
            if writer is not None:
                writer.join(timeout=5.0)


class ServerStreamingTest(ConnectionTestCase):
    """Tests server streaming calls."""

    def test_stream(self) -> None:
        call = self.start_server_stream(val=10)
        request = self.server.receive()
        self.assertEqual(
            request.type,
            PacketType(
                PacketFlag.START | PacketFlag.HAS_PAYLOAD, CloseMode.STREAM_END
            ),
        )
        self.assertEqual(request.payload, Request(val=10).SerializeToString())

        for value in (11, 12, 13):
            self.server.send(
                Packet.server_message(call._call_id, _response(value))
            )
        self.server.send(Packet.server_finish(call._call_id))

        self.assertEqual([r.val for r in call], [11, 12, 13])
        self.assertIs(call.status, RpcStatus.OK)
        self.assertIsNone(call.read())

    def test_read_one_at_a_time(self) -> None:
        call = self.start_server_stream()
        self.server.send(Packet.server_message(call._call_id, _response(1)))
        self.assertEqual(call.read(), Response(val=1))

        with self.assertRaises(pw_rpc2.RpcTimeout):
            call.read(timeout=0.01)

        self.server.send(Packet.server_finish(call._call_id))
        self.assertIsNone(call.read())

    def test_error_after_messages(self) -> None:
        call = self.start_server_stream()
        self.server.send(Packet.server_message(call._call_id, _response(1)))
        self.server.send(
            Packet.server_error(call._call_id, ProtocolError.CANCELLED)
        )

        responses = []
        with self.assertRaises(pw_rpc2.RpcError) as context:
            for response in call:
                responses.append(response.val)

        self.assertEqual(responses, [1])
        self.assertIs(context.exception.status, RpcStatus.CANCELLED)
        self.assertIs(
            context.exception._protocol_error, ProtocolError.CANCELLED
        )

    def test_final_response(self) -> None:
        call = self.start_server_stream()
        self.server.send(Packet.server_message(call._call_id, _response(1)))
        self.server.send(Packet.response(call._call_id, _response(2)))
        self.assertEqual([r.val for r in call], [1, 2])

    def test_final_message_then_finish(self) -> None:
        call = self.start_server_stream()
        self.server.send(
            Packet(
                PacketType(
                    PacketFlag.SERVER | PacketFlag.HAS_PAYLOAD,
                    CloseMode.STREAM_END,
                ),
                call._call_id,
                payload=_response(1),
            )
        )
        self.assertEqual(call.read(), Response(val=1))
        self.assertFalse(call.completed)

        self.server.send(Packet.server_finish(call._call_id))
        self.assertIsNone(call.read())
        self.assertIs(call.status, RpcStatus.OK)

    def test_stream_end_waits_for_terminal_packet(self) -> None:
        call = self.start_server_stream()
        self.server.send(Packet.server_message(call._call_id, _response(1)))
        self.server.send(Packet.server_stream_end(call._call_id))
        # The server sends no more messages after ending its stream.
        self.server.send(Packet.server_message(call._call_id, _response(2)))

        self.assertEqual(call.read(), Response(val=1))
        with self.assertRaises(pw_rpc2.RpcTimeout):
            call.read(timeout=0.01)
        self.assertFalse(call.completed)

        self.server.send(
            Packet.server_error(call._call_id, ProtocolError.CANCELLED)
        )
        with self.assertRaises(pw_rpc2.RpcError) as context:
            call.read()
        self.assertIs(context.exception.status, RpcStatus.CANCELLED)

    def test_stream_end_drops_payload_in_ok_terminal(self) -> None:
        call = self.start_server_stream()
        self.server.send(Packet.server_message(call._call_id, _response(1)))
        self.server.send(Packet.server_stream_end(call._call_id))
        self.server.send(Packet.response(call._call_id, _response(2)))

        self.assertEqual([r.val for r in call], [1])
        self.assertIs(call.status, RpcStatus.OK)

    def test_no_write_method(self) -> None:
        call = self.start_server_stream()
        self.assertFalse(hasattr(call, 'write'))


class ClientStreamingTest(ConnectionTestCase):
    """Tests client streaming calls."""

    def test_stream(self) -> None:
        call = self.start_client_stream()
        self.assertEqual(
            list(inspect.signature(call.write).parameters),
            ['request', 'val'],
        )
        self.assertEqual(
            self.server.receive(),
            Packet.open_stream(1, _SERVICE_ID, 0xA0C83643),
        )

        call.write(val=1)
        call.write(Request(val=2))
        self.assertEqual(
            self.server.receive(),
            Packet.client_message(1, Request(val=1).SerializeToString()),
        )
        self.assertEqual(
            self.server.receive(),
            Packet.client_message(1, Request(val=2).SerializeToString()),
        )

        self.server.send(Packet.response(1, _response(3)))
        # finish() waits for the response, which the server may send before or
        # after the stream end.
        self.assertEqual(call.finish().val, 3)
        self.assertEqual(call.finish().val, 3)

    def test_finish_sends_stream_end(self) -> None:
        call = self.start_client_stream()
        self.server.receive()

        def respond() -> None:
            self.assertEqual(
                self.server.receive(), Packet.client_stream_end(call._call_id)
            )
            self.server.send(Packet.response(call._call_id, _response(0)))

        thread = threading.Thread(target=respond)
        thread.start()
        self.assertEqual(call.finish(), Response())
        thread.join()

    def test_finish_timeout(self) -> None:
        call = self.start_client_stream()
        self.server.receive()

        with self.assertRaises(pw_rpc2.RpcTimeout):
            call.finish(0.01)
        self.assertEqual(
            self.server.receive(), Packet.client_stream_end(call._call_id)
        )
        self.assertFalse(call.completed)

        self.server.send(Packet.response(call._call_id, _response(42)))
        self.assertEqual(call.finish().val, 42)
        self.assertTrue(self.server.nothing_sent())

    def test_early_response_skips_stream_end(self) -> None:
        call = self.start_client_stream()
        self.server.receive()
        self.server.send(Packet.response(call._call_id, _response(7)))
        _wait_until(lambda: call.completed)

        self.assertEqual(call.finish().val, 7)
        self.assertTrue(self.server.nothing_sent())

    def test_stream_message_is_method_type_mismatch(self) -> None:
        call = self.start_client_stream()
        self.server.receive()
        self.server.send(Packet.server_message(call._call_id, _response(1)))
        _wait_until(lambda: call.completed)

        self.assertEqual(
            self.server.receive(),
            Packet.client_error(
                call._call_id, ProtocolError.METHOD_TYPE_MISMATCH
            ),
        )
        with self.assertRaises(pw_rpc2.RpcError) as context:
            call.finish()
        self.assertIs(context.exception.status, RpcStatus.UNIMPLEMENTED)
        self.assertIs(
            context.exception._protocol_error,
            ProtocolError.METHOD_TYPE_MISMATCH,
        )
        self.assertTrue(self.server.nothing_sent())

    def test_write_after_completion(self) -> None:
        call = self.start_client_stream()
        self.server.send(Packet.response(call._call_id, _response(7)))
        call.finish()

        with self.assertRaisesRegex(
            pw_rpc2.StreamClosedError, 'already complete'
        ) as context:
            call.write(val=1)
        self.assertIsInstance(context.exception, RuntimeError)
        self.assertIs(context.exception.status, RpcStatus.WRITE_CLOSED)

    def test_write_after_error(self) -> None:
        call = self.start_client_stream()
        self.server.send(
            Packet.server_error(call._call_id, ProtocolError.INTERNAL)
        )

        with self.assertRaises(pw_rpc2.RpcError):
            call.finish()
        with self.assertRaises(pw_rpc2.RpcError) as context:
            call.write(val=1)
        self.assertIs(context.exception.status, RpcStatus.PROTOCOL_ERROR)
        self.assertIs(context.exception._protocol_error, ProtocolError.INTERNAL)


class BidirectionalStreamingTest(ConnectionTestCase):
    """Tests bidirectional streaming calls."""

    def test_stream(self) -> None:
        """Exercises sending, receiving, and closing a bidirectional stream."""
        call = self.start_bidi_stream()
        self.assertEqual(
            self.server.receive(),
            Packet.open_stream(1, _SERVICE_ID, _BIDI_STREAM_METHOD._id),
        )

        call.write(val=1)
        self.assertEqual(
            self.server.receive(),
            Packet.client_message(1, Request(val=1).SerializeToString()),
        )
        self.server.send(Packet.server_message(1, _response(1)))
        response = call.read()
        assert response is not None
        self.assertEqual(response.val, 1)

        call.finish()
        self.assertEqual(self.server.receive(), Packet.client_stream_end(1))

        with self.assertRaisesRegex(
            pw_rpc2.StreamClosedError, 'already closed'
        ) as context:
            call.finish()
        self.assertIsInstance(context.exception, RuntimeError)
        self.assertIs(context.exception.status, RpcStatus.WRITE_CLOSED)
        self.assertTrue(self.server.nothing_sent())

        with self.assertRaisesRegex(
            pw_rpc2.StreamClosedError, 'already finished'
        ) as context:
            call.write(val=2)
        self.assertIsInstance(context.exception, RuntimeError)
        self.assertIs(context.exception.status, RpcStatus.WRITE_CLOSED)

        self.server.send(Packet.server_message(1, _response(2)))
        self.server.send(Packet.server_finish(1))
        self.assertEqual([r.val for r in call], [2])
        self.assertIs(call.status, RpcStatus.OK)

    def test_server_finishes_first(self) -> None:
        call = self.start_bidi_stream()
        self.server.receive()
        self.server.send(Packet.server_finish(call._call_id))

        self.assertIsNone(call.read())
        with self.assertRaisesRegex(
            pw_rpc2.StreamClosedError, 'already closed'
        ) as context:
            call.finish()
        self.assertIs(context.exception.status, RpcStatus.WRITE_CLOSED)
        self.assertTrue(self.server.nothing_sent())

    def test_finish_on_closed_socket(self) -> None:
        call = self.start_bidi_stream()
        self.server.receive()

        with mock.patch.object(
            self.server.socket,
            'write',
            side_effect=pw_rpc2.SocketClosed('closed'),
        ):
            with self.assertRaises(pw_rpc2.StreamClosedError) as context:
                call.finish()
        self.assertIs(context.exception.status, RpcStatus.WRITE_CLOSED)
        with self.assertRaises(pw_rpc2.SocketClosed):
            call.read()


class ConnectionProtocolTest(ConnectionTestCase):
    """Tests connection-level packet routing, errors, and concurrency."""

    def test_unknown_call_packets(self) -> None:
        # Terminal packets for unknown calls are dropped without a reply.
        self.server.send(Packet.response(99, _response(1)))
        self.server.send(Packet.server_finish(99))
        self.server.send(Packet.server_error(99, ProtocolError.INTERNAL))

        # Non-terminal packets for unknown calls get a
        # RECEIVED_PACKET_FOR_UNKNOWN_CALL reply.
        self.server.send(Packet.server_message(98, _response(1)))
        self.server.send(Packet.server_stream_end(97))
        self.assertEqual(
            self.server.receive(),
            Packet.client_error(
                98, ProtocolError.RECEIVED_PACKET_FOR_UNKNOWN_CALL
            ),
        )
        self.assertEqual(
            self.server.receive(),
            Packet.client_error(
                97, ProtocolError.RECEIVED_PACKET_FOR_UNKNOWN_CALL
            ),
        )

        call = self.start_unary()
        self.server.receive()
        self.server.send(Packet.response(call._call_id, _response(2)))
        self.assertEqual(call.response().val, 2)
        self.assertFalse(self.connection.closed)

    def test_unknown_packet_type_closes_connection(self) -> None:
        call = self.start_unary()
        self.server.send(bytes([call._call_id, 0, 0, 0, 0x7F]))

        with self.assertRaises(pw_rpc2.DecodeError) as context:
            call.response()
        self.assertIs(context.exception.status, RpcStatus.PROTOCOL_ERROR)
        self.assertTrue(self.connection.closed)

    def test_client_packets_rejected(self) -> None:
        call = self.start_unary()
        self.server.receive()

        # Non-terminal client packets receive RECEIVED_PACKET_FOR_WRONG_ENDPOINT
        # and complete any matching local call with PROTOCOL_ERROR.
        self.server.send(Packet.client_message(call._call_id, b''))
        self.server.send(Packet.client_stream_end(49))
        self.server.send(Packet.request(50, 1, 2))
        self.server.send(Packet.open_stream(51, 1, 2))
        # Terminal client error packet does not trigger a reply.
        self.server.send(Packet.client_error(52, ProtocolError.CANCELLED))

        for call_id in (call._call_id, 49, 50, 51):
            self.assertEqual(
                self.server.receive(),
                Packet.client_error(
                    call_id,
                    ProtocolError.RECEIVED_PACKET_FOR_WRONG_ENDPOINT,
                ),
            )
        self.assertTrue(self.server.nothing_sent())

        with self.assertRaises(pw_rpc2.RpcError) as context:
            call.response()
        self.assertIs(context.exception.status, RpcStatus.PROTOCOL_ERROR)
        self.assertIs(
            context.exception._protocol_error,
            ProtocolError.RECEIVED_PACKET_FOR_WRONG_ENDPOINT,
        )

    def test_malformed_packet_closes_connection(self) -> None:
        call = self.start_unary()
        self.server.send(b'\x01\x00')

        with self.assertRaises(pw_rpc2.DecodeError) as context:
            call.response()
        self.assertIs(context.exception.status, RpcStatus.PROTOCOL_ERROR)
        self.assertTrue(self.connection.closed)

        # Later calls report the protocol error that closed the connection.
        with self.assertRaises(pw_rpc2.DecodeError) as context:
            self.start_unary()
        self.assertIs(context.exception.status, RpcStatus.PROTOCOL_ERROR)

    def test_receive_thread_does_not_stall_on_tx_backpressure(self) -> None:
        """Keeps receiving packets when outbound error replies hit TX full."""
        server = FakeServer(queue_depth=2)
        connection = Connection(server.socket, DEFAULT_HANDSHAKE_TIMEOUT)
        writer: threading.Thread | None = None
        try:
            server.expect_handshake()
            stream = ClientStreamingCall(
                connection.start_call(_CLIENT_STREAM_METHOD, b''), 5.0
            )
            unary = UnaryCall(
                connection.start_call(
                    _UNARY_METHOD, Request(val=1).SerializeToString()
                ),
                5.0,
            )
            server.receive()
            server.receive()

            # Fill the outbound queue and block a write on `stream`.
            stream.write(val=10)
            stream.write(val=20)

            entered_write = threading.Event()
            original_write = server.socket._write

            def notify_write(data: bytes, deadline: Any) -> None:
                entered_write.set()
                original_write(data, deadline)

            def write_blocked() -> None:
                try:
                    stream.write(val=30)
                except pw_rpc2.SocketClosed:
                    pass

            with mock.patch.object(
                server.socket, '_write', side_effect=notify_write
            ):
                writer = threading.Thread(target=write_blocked)
                writer.start()
                self.assertTrue(entered_write.wait(timeout=5.0))

            # Trigger outbound error replies from the receive thread while TX
            # is blocked, then answer `unary` to prove receiving keeps running.
            server.send(Packet.server_message(999, _response(0)))
            server.send(Packet.server_stream_end(stream._call_id))
            server.send(Packet.response(unary._call_id, _response(42)))

            self.assertEqual(unary.response(timeout=1.0), Response(val=42))
            with self.assertRaises(pw_rpc2.RpcError) as ctx:
                stream.finish(timeout=1.0)
            self.assertIs(ctx.exception.status, RpcStatus.UNIMPLEMENTED)

            # Drain all 5 client->server packets; `stream`'s error reply must
            # follow its in-flight request.
            received = [server.receive() for _ in range(5)]
            writer.join(timeout=5.0)
            self.assertIn(
                Packet.client_error(
                    999, ProtocolError.RECEIVED_PACKET_FOR_UNKNOWN_CALL
                ),
                received,
            )
            self.assertEqual(
                [p for p in received if p.call_id == stream._call_id],
                [
                    Packet.client_message(
                        stream._call_id, Request(val=10).SerializeToString()
                    ),
                    Packet.client_message(
                        stream._call_id, Request(val=20).SerializeToString()
                    ),
                    Packet.client_message(
                        stream._call_id, Request(val=30).SerializeToString()
                    ),
                    Packet.client_error(
                        stream._call_id, ProtocolError.METHOD_TYPE_MISMATCH
                    ),
                ],
            )
        finally:
            connection.close()
            if writer is not None:
                writer.join(timeout=5.0)

    def test_disconnect(self) -> None:
        closed_events = []
        self.connection.set_on_close(lambda: closed_events.append(True))

        call = self.start_server_stream()
        self.server.disconnect()

        with self.assertRaises(pw_rpc2.SocketClosed):
            call.read()
        self.assertTrue(self.connection.closed)
        _wait_until(lambda: len(closed_events) == 1)

        # Registering on_close on an already-closed connection runs immediately.
        self.connection.set_on_close(lambda: closed_events.append(True))
        self.assertEqual(len(closed_events), 2)

        with self.assertRaises(pw_rpc2.SocketClosed):
            self.start_unary()

    def test_close(self) -> None:
        call = self.start_bidi_stream()
        self.connection.close()
        self.connection.close()

        self.assertIs(call.status, RpcStatus.CLOSED)
        self.assertTrue(self.server.socket.closed)
        with self.assertRaises(pw_rpc2.RpcError) as context:
            call.write(val=1)
        self.assertIs(context.exception.status, RpcStatus.CLOSED)

        with self.assertRaises(pw_rpc2.SocketClosed):
            self.start_unary()

    def test_call_id_exhaustion_closes_connection(self) -> None:
        self.connection._next_call_id = 0xFFFFFFFF
        call = self.start_unary()
        self.assertEqual(call._call_id, 0xFFFFFFFF)

        with self.assertRaisesRegex(pw_rpc2.SocketClosed, 'exhausted'):
            self.start_unary()
        self.assertTrue(self.connection.closed)
        with self.assertRaises(pw_rpc2.SocketClosed):
            call.response()

    def test_concurrent_calls(self) -> None:
        calls = [self.start_unary(val=i) for i in range(50)]
        for call in reversed(calls):
            self.server.send(
                Packet.response(call._call_id, _response(call._call_id))
            )
        self.assertEqual(
            [call.response().val for call in calls], list(range(1, 51))
        )

    def test_many_threads(self) -> None:
        """Runs calls from many threads at once."""
        results: list[int] = []
        lock = threading.Lock()

        def run(value: int) -> None:
            with self.start_bidi_stream() as call:
                call.write(val=value)
                response = call.read()
                assert response is not None
                with lock:
                    results.append(response.val)

        def serve() -> None:
            # Echo each request back.
            while len(results) < 20:
                try:
                    packet = self.server.receive(timeout=5)
                except Exception:  # pylint: disable=broad-except
                    return
                if packet.type == PacketType(PacketFlag.HAS_PAYLOAD):
                    self.server.send(
                        Packet.server_message(
                            packet.call_id,
                            _response(Request.FromString(packet.payload).val),
                        )
                    )

        server_thread = threading.Thread(target=serve)
        server_thread.start()
        threads = [threading.Thread(target=run, args=(i,)) for i in range(20)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        server_thread.join()

        self.assertEqual(sorted(results), list(range(20)))


if __name__ == '__main__':
    unittest.main()
