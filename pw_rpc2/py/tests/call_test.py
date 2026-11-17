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
"""Tests pw_rpc2._call state, exceptions, and call handles."""

import inspect
import math
import threading
import unittest

import pw_rpc2
from pw_rpc2 import _descriptors as descriptors
from pw_rpc2._call import (
    BidirectionalStreamingCall,
    CallConnection,
    CallState,
    ClientStreamingCall,
    ServerStreamingCall,
    UnaryCall,
    resolve_timeout,
)
from pw_rpc2._packets import (
    Packet,
    ProtocolError,
    RpcStatus,
    UnknownProtocolError,
)
from pw_rpc2_test_pb import pw_rpc2_test_pb2

# pylint: disable=protected-access

Request = pw_rpc2_test_pb2.EchoRequest
Response = pw_rpc2_test_pb2.EchoResponse

_TEST_SERVICE = next(iter(descriptors.services([pw_rpc2_test_pb2])))
_UNARY_METHOD = _TEST_SERVICE.methods['EchoUnary']
_SERVER_STREAM_METHOD = _TEST_SERVICE.methods['CountUpServerStream']
_CLIENT_STREAM_METHOD = _TEST_SERVICE.methods['AccumulateClientStream']
_BIDI_STREAM_METHOD = _TEST_SERVICE.methods['EchoBidiStream']


def _response(val: int) -> bytes:
    return Response(val=val).SerializeToString()


class _FakeConnection:
    """In-memory CallConnection test double."""

    def __init__(self) -> None:
        self.sent: list[Packet] = []
        self.best_effort: list[Packet] = []
        self.ended: list[CallState] = []
        self.aborted: list[CallState] = []
        self.send_error: Exception | None = None
        self.send_started = threading.Event()
        self.allow_send: threading.Event | None = None

    def send(self, call: CallState, packet: Packet) -> None:
        del call
        self.send_started.set()
        if self.allow_send is not None:
            assert self.allow_send.wait(timeout=5.0)
        if self.send_error is not None:
            raise self.send_error
        self.sent.append(packet)

    def send_best_effort(
        self, packet: Packet, *, call: CallState | None = None
    ) -> None:
        del call
        self.best_effort.append(packet)

    def end_call(
        self,
        call: CallState,
        status: RpcStatus,
        *,
        final_message: bytes | None = None,
        detail: str = '',
        protocol_error: ProtocolError | UnknownProtocolError | None = None,
        notify_server: bool = False,
    ) -> bool:
        completed = call.complete(
            status,
            final_message=final_message,
            detail=detail,
            protocol_error=protocol_error,
            notify_server=notify_server,
        )
        if completed:
            self.ended.append(call)
        return completed

    def abort_call(self, call: CallState, error: Exception) -> bool:
        self.aborted.append(call)
        return call.abort(error)


# Verify at type-check time that _FakeConnection satisfies CallConnection.
_TYPE_CHECK_FAKE: CallConnection = _FakeConnection()


class ExceptionsAndTimeoutsTest(unittest.TestCase):
    """Tests RPC call exceptions and timeout resolution."""

    def test_rpc_error_rejects_ok_status(self) -> None:
        with self.assertRaises(ValueError):
            _ = pw_rpc2.RpcError(_UNARY_METHOD, RpcStatus.OK)

    def test_rpc_error_formatting(self) -> None:
        """Checks RpcError message formatting across protocol error types."""
        err = pw_rpc2.RpcError(
            _UNARY_METHOD,
            RpcStatus.UNIMPLEMENTED,
            'missing handler',
            protocol_error=ProtocolError.UNKNOWN_METHOD,
        )
        self.assertIs(err.method, _UNARY_METHOD)
        self.assertIs(err.status, RpcStatus.UNIMPLEMENTED)
        self.assertEqual(err.detail, 'missing handler')
        self.assertIs(err._protocol_error, ProtocolError.UNKNOWN_METHOD)
        self.assertEqual(
            str(err),
            'pw.rpc2.test.TestEcho.EchoUnary failed with '
            'UNIMPLEMENTED (UNKNOWN_METHOD): missing handler',
        )

        # Matching status and protocol_error names are not duplicated.
        cancelled = pw_rpc2.RpcError(
            _UNARY_METHOD,
            RpcStatus.CANCELLED,
            protocol_error=ProtocolError.CANCELLED,
        )
        self.assertEqual(
            str(cancelled),
            'pw.rpc2.test.TestEcho.EchoUnary failed with CANCELLED',
        )

        # UnknownProtocolError renders its hex code alongside PROTOCOL_ERROR.
        unknown = pw_rpc2.RpcError(
            _UNARY_METHOD,
            RpcStatus.PROTOCOL_ERROR,
            protocol_error=UnknownProtocolError(0x3F),
        )
        self.assertIn('PROTOCOL_ERROR (UNKNOWN_0x3f)', str(unknown))

    def test_rpc_timeout(self) -> None:
        err = pw_rpc2.RpcTimeout(_UNARY_METHOD, 1.5)
        self.assertIsInstance(err, TimeoutError)
        self.assertIs(err.method, _UNARY_METHOD)
        self.assertEqual(err.timeout, 1.5)
        self.assertIn('1.5 s', str(err))

    def test_stream_closed_error(self) -> None:
        err = pw_rpc2.StreamClosedError('closed')
        self.assertIsInstance(err, RuntimeError)
        self.assertIs(err.status, RpcStatus.WRITE_CLOSED)

    def test_resolve_timeout(self) -> None:
        self.assertEqual(resolve_timeout(None, None), math.inf)
        self.assertEqual(resolve_timeout(None, 2.5), 2.5)
        self.assertEqual(resolve_timeout(0.5, 2.5), 0.5)
        self.assertEqual(resolve_timeout(0.0, 2.5), 0.0)
        with self.assertRaises(ValueError):
            resolve_timeout(-0.1, None)
        with self.assertRaises(ValueError):
            resolve_timeout(None, -1.0)


class CallStateTest(unittest.TestCase):
    """Tests CallState synchronization, decoding, and completion."""

    def setUp(self) -> None:
        self.conn = _FakeConnection()

    def test_completion_is_idempotent(self) -> None:
        state = CallState(self.conn, _UNARY_METHOD, 1)
        self.assertIn('active', repr(state))

        self.assertTrue(
            state.complete(RpcStatus.OK, final_message=_response(7))
        )
        self.assertFalse(state.add_message(_response(8)))
        self.assertFalse(state.complete(RpcStatus.CANCELLED))
        self.assertFalse(state.abort(pw_rpc2.SocketClosed('closed')))

        self.assertEqual(state.response(1.0), Response(val=7))
        self.assertIn('OK', repr(state))

    def test_abort_reports_exception_without_status(self) -> None:
        state = CallState(self.conn, _UNARY_METHOD, 2)
        self.assertTrue(state.abort(pw_rpc2.SocketClosed('lost link')))
        self.assertTrue(state.completed)
        self.assertIsNone(state.status)
        self.assertIsNone(state.protocol_error)
        self.assertIn('SocketClosed', repr(state))

        with self.assertRaises(pw_rpc2.SocketClosed):
            state.response(1.0)

    def test_unary_completed_without_response_raises_protocol_error(
        self,
    ) -> None:
        state = CallState(self.conn, _UNARY_METHOD, 3)
        state.complete(RpcStatus.OK)

        with self.assertRaises(pw_rpc2.RpcError) as ctx:
            state.response(1.0)
        self.assertIs(ctx.exception.status, RpcStatus.PROTOCOL_ERROR)

    def test_decode_error_raises_data_error(self) -> None:
        state = CallState(self.conn, _UNARY_METHOD, 4)
        state.complete(RpcStatus.OK, final_message=b'\xff\xff\xff')

        for _ in range(2):
            with self.assertRaises(pw_rpc2.DecodeError) as ctx:
                state.response(1.0)
            self.assertIs(ctx.exception.status, RpcStatus.DATA_ERROR)

    def test_concurrent_sync_waiters_receive_cached_response(self) -> None:
        state = CallState(self.conn, _UNARY_METHOD, 5)
        results: list[object] = []
        threads = [
            threading.Thread(target=lambda: results.append(state.response(5.0)))
            for _ in range(4)
        ]
        for thread in threads:
            thread.start()

        state.complete(RpcStatus.OK, final_message=_response(42))
        for thread in threads:
            thread.join()

        self.assertEqual(results, [Response(val=42)] * 4)

    def test_finish_stream_aborts_on_socket_closed(self) -> None:
        state = CallState(self.conn, _BIDI_STREAM_METHOD, 6)
        self.conn.send_error = pw_rpc2.SocketClosed('disconnected')

        with self.assertRaises(pw_rpc2.StreamClosedError) as ctx:
            state.finish_stream()
        self.assertIs(ctx.exception.status, RpcStatus.WRITE_CLOSED)
        self.assertTrue(state.stream_finished)
        self.assertTrue(state.completed)

        with self.assertRaises(pw_rpc2.SocketClosed):
            state.read(1.0)

    def test_end_call_without_notifying_server(self) -> None:
        state = CallState(self.conn, _UNARY_METHOD, 7)
        self.assertTrue(
            self.conn.end_call(
                state,
                RpcStatus.CANCELLED,
                protocol_error=ProtocolError.CANCELLED,
            )
        )
        self.assertFalse(state.cancel())
        self.assertIs(state.status, RpcStatus.CANCELLED)
        self.assertEqual(self.conn.best_effort, [])
        self.assertEqual(self.conn.ended, [state])

    def test_cancel_during_blocked_write_defers_packet_without_blocking(
        self,
    ) -> None:
        """Cancels immediately while deferring the wire packet behind write."""
        state = CallState(self.conn, _BIDI_STREAM_METHOD, 8)
        allow_send = threading.Event()
        self.conn.allow_send = allow_send

        writer = threading.Thread(target=state.write, args=(Request(val=1),))
        writer.start()
        try:
            self.assertTrue(self.conn.send_started.wait(timeout=5.0))

            # Cancelling completes the call and wakes readers immediately
            # without waiting on the blocked write, while deferring the wire
            # CANCELLED packet until the in-flight request has been sent.
            self.assertTrue(state.cancel())
            self.assertTrue(state.completed)
            self.assertIs(state.status, RpcStatus.CANCELLED)
            with self.assertRaises(pw_rpc2.RpcError) as ctx:
                state.read(0.0)
            self.assertIs(ctx.exception.status, RpcStatus.CANCELLED)
            self.assertEqual(self.conn.best_effort, [])
        finally:
            allow_send.set()
            writer.join(timeout=5.0)

        self.assertFalse(writer.is_alive())
        self.assertEqual(
            self.conn.sent,
            [Packet.client_message(8, Request(val=1).SerializeToString())],
        )
        self.assertEqual(
            self.conn.best_effort,
            [Packet.client_error(8, ProtocolError.CANCELLED)],
        )

        # Later writes fail immediately without sending anything.
        with self.assertRaises(pw_rpc2.RpcError) as ctx:
            state.write(Request(val=2))
        self.assertIs(ctx.exception.status, RpcStatus.CANCELLED)
        self.assertEqual(len(self.conn.sent), 1)

    def test_begin_and_abandon_send_drops_deferred_error(self) -> None:
        state = CallState(self.conn, _UNARY_METHOD, 9)
        state.begin_send()
        self.assertTrue(state.cancel())
        self.assertEqual(self.conn.best_effort, [])

        # If sending the initial request fails, no CANCELLED packet is sent.
        state.abandon_send()
        self.assertEqual(self.conn.best_effort, [])


class CallStateAsyncTest(unittest.IsolatedAsyncioTestCase):
    """Tests CallState's asyncio response and stream waiters."""

    async def test_response_async_woken_from_thread(self) -> None:
        conn = _FakeConnection()
        state = CallState(conn, _UNARY_METHOD, 1)

        def deliver() -> None:
            state.complete(RpcStatus.OK, final_message=_response(99))

        timer = threading.Timer(0.01, deliver)
        timer.start()
        try:
            response = await state.response_async(5.0)
        finally:
            timer.join()

        self.assertEqual(response, Response(val=99))

    async def test_read_async_stream_and_timeout(self) -> None:
        conn = _FakeConnection()
        state = CallState(conn, _SERVER_STREAM_METHOD, 2)

        with self.assertRaises(pw_rpc2.RpcTimeout):
            await state.read_async(0.01)

        state.add_message(_response(10))
        state.add_message(_response(20))
        state.complete(RpcStatus.OK)

        self.assertEqual(await state.read_async(1.0), Response(val=10))
        self.assertEqual(await state.read_async(1.0), Response(val=20))
        self.assertIsNone(await state.read_async(1.0))


class CallHandlesTest(unittest.TestCase):
    """Tests UnaryCall and streaming Call handle classes."""

    def setUp(self) -> None:
        self.conn = _FakeConnection()

    def test_unary_call_and_timeout_override(self) -> None:
        state = CallState(self.conn, _UNARY_METHOD, 1)
        call = UnaryCall(state, timeout=5.0)

        with self.assertRaises(pw_rpc2.RpcTimeout) as ctx:
            call.response(timeout=0.0)
        self.assertEqual(ctx.exception.timeout, 0.0)
        self.assertFalse(call.completed)

        state.complete(RpcStatus.OK, final_message=_response(12))
        self.assertEqual(call.response(), Response(val=12))
        self.assertIs(call.status, RpcStatus.OK)

    def test_server_streaming_call_iteration_and_context_manager(self) -> None:
        state = CallState(self.conn, _SERVER_STREAM_METHOD, 2)
        with ServerStreamingCall(state, timeout=5.0) as call:
            self.assertFalse(hasattr(call, 'write'))
            state.add_message(_response(1))
            state.add_message(_response(2))
            self.assertEqual(call.read(), Response(val=1))
            # Exiting early cancels the active call.

        self.assertIs(call.status, RpcStatus.CANCELLED)
        self.assertEqual(
            self.conn.best_effort,
            [Packet.client_error(2, ProtocolError.CANCELLED)],
        )
        self.assertFalse(call.cancel())

    def test_server_streaming_call_full_iteration(self) -> None:
        state = CallState(self.conn, _SERVER_STREAM_METHOD, 3)
        call = ServerStreamingCall(state, timeout=5.0)
        state.add_message(_response(3))
        state.add_message(_response(4))
        state.complete(RpcStatus.OK)

        self.assertEqual(list(call), [Response(val=3), Response(val=4)])
        self.assertIsNone(call.read())

    def test_client_streaming_call_write_signature_and_finish(self) -> None:
        state = CallState(self.conn, _CLIENT_STREAM_METHOD, 4)
        call = ClientStreamingCall(state, timeout=5.0)

        self.assertEqual(
            list(inspect.signature(call.write).parameters),
            ['request', 'val'],
        )
        call.write(val=5)
        call.write(Request(val=6))
        self.assertEqual(
            self.conn.sent,
            [
                Packet.client_message(4, Request(val=5).SerializeToString()),
                Packet.client_message(4, Request(val=6).SerializeToString()),
            ],
        )

        # If the server already completed the call early, finish() ignores
        # StreamClosedError and returns the response without sending stream end.
        state.complete(RpcStatus.OK, final_message=_response(11))
        self.assertEqual(call.finish(), Response(val=11))
        self.assertEqual(len(self.conn.sent), 2)

        with self.assertRaises(pw_rpc2.StreamClosedError):
            call.write(val=7)

    def test_bidirectional_streaming_call(self) -> None:
        state = CallState(self.conn, _BIDI_STREAM_METHOD, 5)
        call = BidirectionalStreamingCall(state, timeout=5.0)

        call.write(val=1)
        state.add_message(_response(10))
        self.assertEqual(call.read(), Response(val=10))

        call.finish()
        self.assertEqual(
            self.conn.sent[-1],
            Packet.client_stream_end(5),
        )

        with self.assertRaises(pw_rpc2.StreamClosedError):
            call.write(val=2)
        with self.assertRaises(pw_rpc2.StreamClosedError):
            call.finish()

        state.complete(RpcStatus.OK, final_message=_response(20))
        self.assertEqual(list(call), [Response(val=20)])

    def test_context_manager_cancels_during_blocked_write(self) -> None:
        state = CallState(self.conn, _BIDI_STREAM_METHOD, 6)
        allow_send = threading.Event()
        self.conn.allow_send = allow_send
        writer: threading.Thread | None = None

        try:
            with self.assertRaises(pw_rpc2.RpcTimeout):
                with BidirectionalStreamingCall(state, timeout=5.0) as call:
                    writer = threading.Thread(
                        target=call.write, kwargs={'val': 1}
                    )
                    writer.start()
                    self.assertTrue(self.conn.send_started.wait(timeout=5.0))
                    call.read(timeout=0.0)

            # Exiting the context manager cancelled the call immediately even
            # though call.write() is still blocked.
            self.assertIs(call.status, RpcStatus.CANCELLED)
            assert writer is not None
            self.assertTrue(writer.is_alive())
            self.assertEqual(self.conn.best_effort, [])
        finally:
            allow_send.set()
            if writer is not None:
                writer.join(timeout=5.0)

        self.assertEqual(
            self.conn.best_effort,
            [Packet.client_error(6, ProtocolError.CANCELLED)],
        )


if __name__ == '__main__':
    unittest.main()
