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
"""Manages a pw_rpc2 client connection and active calls."""

from __future__ import annotations

import collections
import logging
import math
import threading
import time
from typing import Callable

from pw_transport import ReliableSocket, SocketClosed

from pw_rpc2._call import CallState, resolve_timeout
from pw_rpc2._descriptors import Method
from pw_rpc2._packets import (
    PROTOCOL_VERSION,
    CloseMode,
    DecodeError,
    Handshake,
    HandshakeType,
    Packet,
    PacketType,
    ProtocolError,
    RpcStatus,
    UnknownProtocolError,
    peer_error_to_rpc_status,
)

_LOG = logging.getLogger('pw_rpc2')

# Call IDs are unsigned 32-bit integers, and a connection never reuses one.
_MAX_CALL_ID = 2**32 - 1

#: How long to wait for the server to reply to the connection handshake, in
#: seconds, unless specified otherwise. A server that is up replies within
#: milliseconds, but the default leaves ample time for slow links.
DEFAULT_HANDSHAKE_TIMEOUT = 5.0


def _remaining_timeout(deadline: float | None) -> float | None:
    return None if deadline is None else max(0.0, deadline - time.monotonic())


class HandshakeError(ConnectionError):
    """The connection handshake with the server failed.

    Attributes:
      status: The :py:class:`RpcStatus` for the failure:
        :py:attr:`RpcStatus.DISCONNECTED` if the socket closed during the
        handshake, or :py:attr:`RpcStatus.PROTOCOL_ERROR` if the server sent an
        invalid reply or did not complete the handshake in time.
    """

    def __init__(
        self,
        message: str,
        status: RpcStatus = RpcStatus.PROTOCOL_ERROR,
    ) -> None:
        super().__init__(message)
        self.status = status


class Connection:  # pylint: disable=too-many-instance-attributes
    """An RPC connection to a server, shared by the client front-ends.

    A connection performs the handshake, receives and dispatches packets on a
    background thread, and tracks active calls. ``socket`` is a private
    communication channel between this client and one server and cannot be
    shared.

    It is internal to the client implementations; applications use
    :py:class:`Client` or :py:class:`pw_rpc2.aio.Client`.
    """

    def __init__(
        self,
        socket: ReliableSocket,
        handshake_timeout: float | None = DEFAULT_HANDSHAKE_TIMEOUT,
    ) -> None:
        """Performs the handshake with the server.

        Raises:
          HandshakeError: The server did not complete the handshake, in which
            case the socket is closed.
          ValueError: The handshake timeout is negative.
        """
        handshake_timeout = resolve_timeout(
            handshake_timeout, DEFAULT_HANDSHAKE_TIMEOUT
        )
        self._socket = socket
        self._on_close: Callable[[], None] | None = None
        self._lock = threading.Condition()
        self._calls: dict[int, CallState] = {}
        self._next_call_id = 1
        self._closed = False
        # The malformed packet that closed the connection, if any.
        self._protocol_error: DecodeError | None = None
        self._control_packets: collections.deque[Packet] = collections.deque()
        self._control_thread: threading.Thread | None = None

        handshake_deadline = (
            None
            if math.isinf(handshake_timeout)
            else time.monotonic() + handshake_timeout
        )
        self._handshake_done = threading.Event()
        self._handshake_error: HandshakeError | None = None

        self._thread = threading.Thread(
            target=self._receive_loop,
            args=(handshake_timeout, handshake_deadline),
            name='pw_rpc2 receiver',
            daemon=True,
        )

        try:
            # Send SYN before receiving anything, so that the ACK sent from the
            # receive thread always follows it.
            socket.write(
                Handshake(HandshakeType.SYN).encode(),
                timeout=_remaining_timeout(handshake_deadline),
            )
            self._thread.start()
            self._handshake_done.wait()
        except TimeoutError:
            self._handshake_error = HandshakeError(
                'RPC handshake failed: timed out sending SYN after '
                f'{handshake_timeout:g} s',
                status=RpcStatus.PROTOCOL_ERROR,
            )
        except SocketClosed as error:
            self._handshake_error = HandshakeError(
                f'RPC handshake failed: {error}',
                status=RpcStatus.DISCONNECTED,
            )

        if (failure := self._handshake_error) is not None:
            self.close()
            raise failure

    @property
    def closed(self) -> bool:
        return self._closed

    def set_on_close(self, callback: Callable[[], None]) -> None:
        """Registers a callback invoked when the connection closes.

        The callback runs immediately if the connection is already closed.
        """
        with self._lock:
            closed = self._closed
            if not closed:
                self._on_close = callback
        if closed:
            callback()

    def start_call(self, method: Method, payload: bytes) -> CallState:
        """Registers a call and sends its request.

        Raises:
          ValueError: The request is too large for the socket.
          DecodeError: A malformed packet closed the connection.
          SocketClosed: The connection is closed.
        """
        call = self.register_call(method)
        self.transmit_request(call, self.encode_request(call, payload))
        return call

    def register_call(self, method: Method) -> CallState:
        """Registers a new call, without sending its request.

        Raises:
          DecodeError: A malformed packet closed the connection.
          SocketClosed: The connection is closed.
        """
        with self._lock:
            if self._closed:
                # As in C++, a connection closed by a protocol violation keeps
                # reporting it, since reconnecting would not help.
                if (protocol_error := self._protocol_error) is not None:
                    raise DecodeError(
                        'The connection was closed due to a protocol error: '
                        f'{protocol_error}'
                    ) from protocol_error
                raise SocketClosed('The client is closed')
            if self._next_call_id <= _MAX_CALL_ID:
                call = CallState(self, method, self._next_call_id)
                call.begin_send()
                self._next_call_id += 1
                self._calls[call.call_id] = call
                return call

        error = SocketClosed(
            'Call IDs exhausted; reconnect to continue making calls'
        )
        self._shut_down(error)
        raise error

    def encode_request(self, call: CallState, payload: bytes) -> bytes:
        """Encodes the packet that starts a call; ends the call if that fails.

        Raises:
          ValueError: The request is too large for the socket.
        """
        # pylint: disable=protected-access
        method = call.method
        if method.type.client_streaming:
            # Requests are sent with write(), so the stream opens empty.
            assert not payload
            packet = Packet.open_stream(
                call.call_id, method.service._id, method._id
            )
        else:
            packet = Packet.request(
                call.call_id, method.service._id, method._id, payload
            )
        try:
            return self._encode(call, packet)
        except ValueError as error:
            self.abort_call(call, error)
            call.abandon_send()
            raise

    def transmit_request(self, call: CallState, data: bytes) -> None:
        """Sends a call's encoded request; ends the call if that fails."""
        try:
            self._socket.write(data)
        except Exception as error:
            self.abort_call(call, error)
            call.abandon_send()
            raise
        except BaseException:
            self.abandon_request(call)
            raise
        call.finish_send()

    def abandon_request(self, call: CallState) -> None:
        """Drops a call whose request will not be sent.

        The server never learns of the call, so an error packet that was
        deferred until the request was sent (e.g. for a cancellation) is
        dropped rather than sent.
        """
        self._remove(call)
        call.abandon_send()

    def send(self, call: CallState, packet: Packet) -> None:
        """Sends a packet for a call.

        Raises:
          ValueError: The packet is too large for the socket.
          SocketClosed: The connection is closed.
        """
        self._socket.write(self._encode(call, packet))

    def send_best_effort(
        self, packet: Packet, *, call: CallState | None = None
    ) -> None:
        """Queues a control packet to be sent without blocking the caller.

        This is for packets that are courtesies to the server, such as
        cancellations and protocol error replies. Queueing them off the caller's
        thread keeps ``cancel()`` and the receive thread from stalling on
        transport backpressure. If ``call`` is given, it is removed from the
        active calls table in the same critical section that queues ``packet``
        so stray inbound packets cannot queue replies ahead of ``packet``.
        """
        with self._lock:
            if call is not None and self._calls.get(call.call_id) is call:
                del self._calls[call.call_id]
            if self._closed:
                return
            self._control_packets.append(packet)
            if self._control_thread is None:
                self._control_thread = threading.Thread(
                    target=self._send_control_packets,
                    name='pw_rpc2 control',
                    daemon=True,
                )
                self._control_thread.start()
            else:
                self._lock.notify()

    def _send_control_packets(self) -> None:
        while True:
            with self._lock:
                while not self._control_packets and not self._closed:
                    self._lock.wait()
                if self._closed:
                    return
                packet = self._control_packets.popleft()

            _LOG.debug(
                'Sending %s packet for call %d (error=%s)',
                packet.type,
                packet.call_id,
                packet.error,
            )
            try:
                self._socket.write(packet.encode())
            except (SocketClosed, ValueError):
                pass
            except Exception as error:  # pylint: disable=broad-except
                _LOG.exception('Unexpected error in the RPC control thread')
                self._shut_down(error)
                return

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
        """Completes a call; returns False if it was already complete."""
        completed = call.complete(
            status,
            final_message=final_message,
            detail=detail,
            protocol_error=protocol_error,
            notify_server=notify_server,
        )
        if completed and not notify_server:
            self._remove(call)
        return completed

    def abort_call(self, call: CallState, error: Exception) -> bool:
        """Ends a call with a local exception."""
        self._remove(call)
        return call.abort(error)

    def close(self) -> None:
        """Closes the connection, ending all active calls with CLOSED."""
        with self._lock:
            already_closed = self._closed
            if not already_closed:
                self._closed = True
                self._control_packets.clear()
                self._lock.notify_all()
                calls = self._take_calls_locked()
                on_close, self._on_close = self._on_close, None
            control_thread = self._control_thread

        if not already_closed:
            for call in calls:
                call.complete(RpcStatus.CLOSED, detail='the client was closed')
            self._socket.close()
            if on_close is not None:
                on_close()

        for thread in (self._thread, control_thread):
            if (
                thread is not None
                and thread.ident is not None
                and threading.current_thread() is not thread
            ):
                thread.join()

    def _encode(self, call: CallState, packet: Packet) -> bytes:
        """Encodes a packet, checking that the socket can carry it.

        Raises:
          ValueError: The packet is too large for the socket.
        """
        data = packet.encode()

        limit = self._socket.max_write_message_size
        if limit is not None and len(data) > limit:
            raise ValueError(
                f'The {len(data)}-byte packet for {call.method.full_name} '
                f"exceeds the socket's {limit}-byte limit"
            )

        _LOG.debug(
            'Sending %s packet for %s (call %d, %d B payload)',
            packet.type,
            call.method.full_name,
            call.call_id,
            len(packet.payload),
        )
        return data

    def _remove(self, call: CallState) -> None:
        with self._lock:
            if self._calls.get(call.call_id) is call:
                del self._calls[call.call_id]

    def _take_calls_locked(self) -> list[CallState]:
        calls = list(self._calls.values())
        self._calls.clear()
        return calls

    def _shut_down(self, error: Exception) -> None:
        """Closes the connection due to an error, aborting all calls.

        Unlike close(), this may be called from the receive thread.
        """
        with self._lock:
            if self._closed:
                return
            self._closed = True
            self._control_packets.clear()
            self._lock.notify_all()
            calls = self._take_calls_locked()
            on_close, self._on_close = self._on_close, None
            if isinstance(error, DecodeError) and self._protocol_error is None:
                self._protocol_error = error

        if not self._handshake_done.is_set():
            if self._handshake_error is None:
                status = (
                    RpcStatus.DISCONNECTED
                    if isinstance(error, SocketClosed)
                    else RpcStatus.PROTOCOL_ERROR
                )
                self._handshake_error = HandshakeError(
                    f'RPC handshake failed: {error}', status=status
                )
            self._handshake_done.set()

        for call in calls:
            call.abort(error)
        self._socket.close()
        if on_close is not None:
            on_close()

    def _receive_loop(
        self, handshake_timeout: float, handshake_deadline: float | None
    ) -> None:
        try:
            self._receive_handshake(handshake_timeout, handshake_deadline)
            while True:
                self._handle_packet(self._socket.read())
        except SocketClosed as error:
            self._shut_down(error)
        except (HandshakeError, DecodeError) as error:
            _LOG.error('Closing the RPC connection: %s', error)
            self._shut_down(error)
        except Exception as error:  # pylint: disable=broad-except
            _LOG.exception('Unexpected error in the RPC receive thread')
            self._shut_down(error)

    def _receive_handshake(
        self, handshake_timeout: float, handshake_deadline: float | None
    ) -> None:
        try:
            raw_reply = self._socket.read(
                timeout=_remaining_timeout(handshake_deadline)
            )
        except TimeoutError as error:
            raise self._handshake_failure(
                f'no reply from the server after {handshake_timeout:g} s'
            ) from error

        try:
            reply = Handshake.decode(raw_reply)
        except DecodeError as error:
            raise self._handshake_failure(f'invalid reply: {error}') from error

        if reply.type is not HandshakeType.SYN_ACK:
            raise self._handshake_failure(
                f'expected SYN_ACK, received {reply.type.name}'
            )

        # The server replies with the lower of its version and ours.
        if not 1 <= reply.version <= PROTOCOL_VERSION:
            raise self._handshake_failure(
                f'the server selected unsupported version {reply.version}'
            )

        try:
            self._socket.write(
                Handshake(HandshakeType.ACK, reply.version).encode(),
                timeout=_remaining_timeout(handshake_deadline),
            )
        except TimeoutError as error:
            raise self._handshake_failure(
                f'timed out sending ACK after {handshake_timeout:g} s'
            ) from error
        self._handshake_done.set()

    def _handshake_failure(
        self,
        reason: str,
        status: RpcStatus = RpcStatus.PROTOCOL_ERROR,
    ) -> HandshakeError:
        error = HandshakeError(f'RPC handshake failed: {reason}', status=status)
        self._handshake_error = error
        self._handshake_done.set()
        return error

    def _handle_packet(self, data: bytes) -> None:
        try:
            packet = Packet.decode(data)
        except DecodeError as error:
            # A malformed packet cannot be attributed to a call, so the
            # connection is closed rather than risk leaving a call stranded.
            raise DecodeError(
                f'Received a malformed packet: {error}'
            ) from error

        _LOG.debug(
            'Received %s packet for call %d (%d B payload)',
            packet.type,
            packet.call_id,
            len(packet.payload),
        )

        with self._lock:
            call = self._calls.get(packet.call_id)

        if call is not None and call.completed:
            # The call has already ended locally and is waiting for an in-flight
            # send to finish before queueing its client error packet.
            return

        if packet.type.from_client:
            self._reject_client_packet(packet, call)
        elif call is None:
            self._handle_packet_for_unknown_call(packet)
        else:
            self._handle_call_packet(call, packet)

    def _reject_client_packet(
        self, packet: Packet, call: CallState | None
    ) -> None:
        _LOG.warning(
            'Dropping %s packet for call %d, which only clients may send',
            packet.type,
            packet.call_id,
        )
        if call is None:
            if not packet.type.is_terminal:
                self.send_best_effort(
                    Packet.client_error(
                        packet.call_id,
                        ProtocolError.RECEIVED_PACKET_FOR_WRONG_ENDPOINT,
                    )
                )
            return

        self.end_call(
            call,
            RpcStatus.PROTOCOL_ERROR,
            detail=f'received client-only {packet.type} packet from the server',
            protocol_error=ProtocolError.RECEIVED_PACKET_FOR_WRONG_ENDPOINT,
            notify_server=not packet.type.is_terminal,
        )

    def _handle_packet_for_unknown_call(self, packet: Packet) -> None:
        # A terminal packet needs no reply, since it already ended the call.
        if packet.type.is_terminal:
            _LOG.debug(
                'Dropping %s packet for inactive call %d',
                packet.type,
                packet.call_id,
            )
            return

        _LOG.debug(
            'Received %s packet for inactive call %d; replying with '
            'RECEIVED_PACKET_FOR_UNKNOWN_CALL',
            packet.type,
            packet.call_id,
        )
        self.send_best_effort(
            Packet.client_error(
                packet.call_id, ProtocolError.RECEIVED_PACKET_FOR_UNKNOWN_CALL
            )
        )

    def _handle_call_packet(self, call: CallState, packet: Packet) -> None:
        if packet.error is not None:
            self.end_call(
                call,
                peer_error_to_rpc_status(packet.error, from_server=True),
                protocol_error=packet.error,
            )
            return

        # A unary or client streaming call is answered by exactly one packet,
        # which carries the response and ends the RPC. Any other packet means
        # the server treats the method as server or bidirectional streaming,
        # so nothing in it can be trusted to be the response.
        is_single_response = (
            packet.type.has_payload
            and packet.type.close_mode is CloseMode.OK_TERMINAL
        )
        if not call.method.type.server_streaming and not is_single_response:
            self._fail_method_type_mismatch(call, packet.type)
            return

        payload: bytes | None = None
        if packet.type.has_payload:
            if call.server_stream_ended:
                _LOG.warning(
                    'Dropping message in %s packet for call %d; the server '
                    'already ended its stream',
                    packet.type,
                    call.call_id,
                )
            else:
                payload = packet.payload

        if packet.type.close_mode is CloseMode.OK_TERMINAL:
            self.end_call(call, RpcStatus.OK, final_message=payload)
            return

        if payload is not None:
            call.add_message(payload)

        # A server that half-closes its stream sends no more messages, but the
        # RPC continues until the terminal packet that must follow, which
        # completes the call. The C++ server does not currently send a
        # half-close on its own, but the protocol allows it.
        if packet.type.close_mode is CloseMode.STREAM_END:
            call.server_stream_ended = True

    def _fail_method_type_mismatch(
        self, call: CallState, packet_type: PacketType
    ) -> None:
        _LOG.warning(
            'Received %s packet, which does not answer %s RPC %s; the client '
            "and server disagree about the method's type",
            packet_type,
            call.method.type,
            call.method.full_name,
        )
        # A packet that already ended the RPC needs no reply.
        self.end_call(
            call,
            RpcStatus.UNIMPLEMENTED,
            detail=(
                f'the server sent a {packet_type} packet, but '
                f'{call.method.type} calls take a single response that '
                "completes the RPC; the client and server disagree about "
                "the method's type"
            ),
            protocol_error=ProtocolError.METHOD_TYPE_MISMATCH,
            notify_server=not packet_type.is_terminal,
        )
