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
"""Handles for in-progress RPCs and the errors they raise.

Each type of RPC has its own call class, which only offers the operations that
make sense for it:

=========================  ===============================================
RPC type                   Call class
=========================  ===============================================
Unary                      :py:class:`UnaryCall`
Server streaming           :py:class:`ServerStreamingCall`
Client streaming           :py:class:`ClientStreamingCall`
Bidirectional streaming    :py:class:`BidirectionalStreamingCall`
=========================  ===============================================

Calls are started through method clients, never instantiated directly.
Operations that wait for a response from the server take an optional ``timeout``
in seconds; ``None`` uses the call's default timeout, and ``math.inf`` waits
forever. Outgoing writes block only on transport backpressure and do not time
out.
"""

from __future__ import annotations

import asyncio
import collections
import enum
import functools
import math
import threading
import time
from typing import Any, Callable, Iterator, Protocol, TypeVar

from google.protobuf.message import DecodeError as ProtoDecodeError, Message

from pw_transport import SocketClosed

from pw_rpc2._descriptors import Method
from pw_rpc2._packets import (
    DecodeError,
    Packet,
    ProtocolError,
    RpcStatus,
    UnknownProtocolError,
)

_T = TypeVar('_T')
_CallT = TypeVar('_CallT', bound='Call')


class RpcError(Exception):
    """An RPC failed with an error status.

    Attributes:
      method: The RPC that failed.
      status: The :py:class:`RpcStatus` that ended the call.
      detail: A description of the failure, which may be empty.
    """

    def __init__(
        self,
        method: Method,
        status: RpcStatus,
        detail: str = '',
        *,
        protocol_error: ProtocolError | UnknownProtocolError | None = None,
    ) -> None:
        if status.ok():
            raise ValueError('RpcError requires a non-OK status')
        code_desc = status.name
        if protocol_error is not None:
            error = protocol_error.name
            if error != status.name:
                code_desc = f'{status.name} ({error})'
        message = f'{method.full_name} failed with {code_desc}'
        if detail:
            message += f': {detail}'
        super().__init__(message)

        self.method = method
        self.status = status
        self.detail = detail
        self._protocol_error = protocol_error


class RpcTimeout(TimeoutError):
    """An RPC operation did not complete before its timeout expired.

    Timing out does not end the call, except for unary RPCs invoked by calling
    the method client directly, whose call object is never exposed.

    Attributes:
      method: The RPC that timed out.
      timeout: The timeout that expired, in seconds.
    """

    def __init__(self, method: Method, timeout: float) -> None:
        super().__init__(f'{method.full_name} timed out after {timeout:g} s')
        self.method = method
        self.timeout = timeout


class StreamClosedError(RuntimeError):
    """The client's request stream is already closed.

    Raised when calling ``write()`` or ``BidirectionalStreamingCall.finish()``
    after the request stream has already been finished or the call has
    completed.

    Attributes:
      status: Always :py:attr:`RpcStatus.WRITE_CLOSED`.
    """

    status: RpcStatus = RpcStatus.WRITE_CLOSED


def resolve_timeout(timeout: float | None, default: float | None) -> float:
    """Returns the effective timeout; ``math.inf`` means wait forever.

    Raises:
      ValueError: The timeout is negative.
    """
    if timeout is None:
        timeout = default
    if timeout is None:
        return math.inf
    if timeout < 0:
        raise ValueError(f'Timeouts cannot be negative, got {timeout}')
    return timeout


class _Pending(enum.Enum):
    """The result of polling a call whose result is not available yet."""

    PENDING = enum.auto()


class CallConnection(Protocol):
    """Connection operations used by CallState.

    ``send_best_effort`` must not block on transport backpressure, as it is
    called from ``cancel()`` and from the connection's receive thread.
    """

    def send(self, call: CallState, packet: Packet) -> None: ...

    def send_best_effort(
        self, packet: Packet, *, call: CallState | None = None
    ) -> None: ...

    def end_call(
        self,
        call: CallState,
        status: RpcStatus,
        *,
        final_message: bytes | None = None,
        detail: str = '',
        protocol_error: ProtocolError | UnknownProtocolError | None = None,
        notify_server: bool = False,
    ) -> bool: ...

    def abort_call(self, call: CallState, error: Exception) -> bool: ...


class CallState:  # pylint: disable=too-many-instance-attributes
    """The shared, thread-safe state of a call.

    The connection's receive thread records inbound messages and completion;
    the synchronous and asyncio call handles consume them. Waiters of either
    kind are woken on every change.
    """

    def __init__(
        self, connection: CallConnection, method: Method, call_id: int
    ) -> None:
        self.connection = connection
        self.method = method
        self.call_id = call_id

        self._changed = threading.Condition()
        # Serializes write() and finish_stream(). This is separate from _changed
        # (and never acquired by cancel() or the connection's receive thread) so
        # a write blocked on transport backpressure never stalls receiving
        # packets or cancelling calls. Lock order: _write_lock before _changed
        # or connection locks; _changed is never held across connection calls.
        self._write_lock = threading.Lock()
        self._active_sends = 0
        self._pending_error: Packet | None = None
        # Unbounded: the host client buffers unread stream messages in memory
        # rather than stalling the connection's receive thread.
        self._messages: collections.deque[bytes] = collections.deque()
        self._response: Message | None = None
        self._async_waiters: list[
            tuple[asyncio.AbstractEventLoop, asyncio.Future[None]]
        ] = []
        self._status: RpcStatus | None = None
        self._error: Exception | None = None
        self._stream_finished = False

        # Set by the connection's receive thread once the server half-closes
        # its stream. The server sends no more messages, but the call remains
        # active until the terminal packet that must follow.
        self.server_stream_ended = False

    @property
    def status(self) -> RpcStatus | None:
        return self._status

    @property
    def protocol_error(
        self,
    ) -> ProtocolError | UnknownProtocolError | None:
        if isinstance(self._error, RpcError):
            # pylint: disable-next=protected-access
            return self._error._protocol_error
        return None

    @property
    def completed(self) -> bool:
        return self._status is not None or self._error is not None

    @property
    def stream_finished(self) -> bool:
        """True once the client finished its request stream."""
        return self._stream_finished

    def begin_send(self) -> None:
        """Marks an outbound request send in progress.

        If the call ends with a client error while a send is in progress, the
        error packet is deferred until :py:meth:`finish_send` so that no request
        packet is sent after the terminal packet, without making ``cancel()`` or
        the receive thread wait on ``_write_lock``.
        """
        with self._changed:
            self._active_sends += 1

    def finish_send(self) -> None:
        """Completes an outbound request send, flushing any deferred error."""
        reply: Packet | None = None
        with self._changed:
            assert self._active_sends > 0
            self._active_sends -= 1
            if self._active_sends == 0:
                reply, self._pending_error = self._pending_error, None
        if reply is not None:
            self.connection.send_best_effort(reply, call=self)

    def abandon_send(self) -> None:
        """Completes a failed request send without flushing deferred errors."""
        with self._changed:
            assert self._active_sends > 0
            self._active_sends -= 1
            if self._active_sends == 0:
                self._pending_error = None

    # Updates from the connection. Each returns False if the call was already
    # complete, in which case it had no effect.

    def add_message(self, payload: bytes) -> bool:
        with self._changed:
            if self.completed:
                return False
            self._messages.append(payload)
            self._notify_locked()
            return True

    def complete(
        self,
        status: RpcStatus,
        *,
        final_message: bytes | None = None,
        detail: str = '',
        protocol_error: ProtocolError | UnknownProtocolError | None = None,
        notify_server: bool = False,
    ) -> bool:
        """Marks the call complete, optionally queueing a client error reply."""
        reply: Packet | None = None
        with self._changed:
            if self.completed:
                return False
            if final_message is not None:
                self._messages.append(final_message)
            self._status = status
            if not status.ok():
                self._error = RpcError(
                    self.method,
                    status,
                    detail,
                    protocol_error=protocol_error,
                )
            if notify_server:
                assert isinstance(protocol_error, ProtocolError)
                packet = Packet.client_error(self.call_id, protocol_error)
                if self._active_sends:
                    self._pending_error = packet
                else:
                    reply = packet
            self._notify_locked()

        if reply is not None:
            self.connection.send_best_effort(reply, call=self)
        return True

    def abort(self, error: Exception) -> bool:
        """Ends the call due to a local transport or protocol error."""
        with self._changed:
            if self.completed:
                return False
            self._error = error
            self._pending_error = None
            self._notify_locked()
            return True

    # Operations for call handles.

    def write(self, request: Message) -> None:
        """Sends a request on the client's stream.

        Raises:
          StreamClosedError: The call is complete or the stream is finished.
          RpcError: The call ended with an error.
          ValueError: The request is too large for the socket.
          SocketClosed: The connection is closed.
        """
        with self._changed:
            self._check_can_write_locked()

        packet = Packet.client_message(
            self.call_id, request.SerializeToString()
        )
        with self._write_lock:
            with self._changed:
                self._check_can_write_locked()
                self._active_sends += 1

            try:
                self.connection.send(self, packet)
            except SocketClosed as error:
                self.connection.abort_call(self, error)
                self.abandon_send()
                raise
            except BaseException:
                self.finish_send()
                raise
            self.finish_send()

    def finish_stream(self) -> None:
        """Ends the request stream.

        Raises:
          StreamClosedError: The call is complete or the stream is finished.
        """
        with self._changed:
            self._check_can_finish_locked()

        with self._write_lock:
            with self._changed:
                self._check_can_finish_locked()
                self._stream_finished = True
                self._active_sends += 1

            try:
                self.connection.send(
                    self, Packet.client_stream_end(self.call_id)
                )
            except SocketClosed as error:
                self.connection.abort_call(self, error)
                self.abandon_send()
                raise StreamClosedError(
                    f'Cannot finish {self.method.full_name}: '
                    'the connection is closed'
                ) from error
            except BaseException:
                self.finish_send()
                raise
            self.finish_send()

    def cancel(self) -> bool:
        """Cancels the call; returns False if it was already complete."""
        return self.connection.end_call(
            self,
            RpcStatus.CANCELLED,
            detail='cancelled by the client',
            protocol_error=ProtocolError.CANCELLED,
            notify_server=True,
        )

    def read(self, timeout: float) -> Any | None:
        """Blocks for the next response; returns None at the end of the call.

        Raises:
          RpcError: The call failed.
          RpcTimeout: No response arrived in time.
          DecodeError: The response could not be decoded.
        """
        payload = self._wait(self._poll_message_locked, timeout)
        return None if payload is None else self._decode(payload)

    def response(self, timeout: float) -> Any:
        """Blocks for the single response of a call.

        Raises:
          RpcError: The call failed.
          RpcTimeout: The response did not arrive in time.
          DecodeError: The response could not be decoded.
        """
        return self._wait(self._poll_response_locked, timeout)

    async def read_async(self, timeout: float) -> Any | None:
        """Awaits the next response; returns None at the end of the call."""
        payload = await self._wait_async(self._poll_message_locked, timeout)
        return None if payload is None else self._decode(payload)

    async def response_async(self, timeout: float) -> Any:
        """Awaits the single response of a call."""
        return await self._wait_async(self._poll_response_locked, timeout)

    def _check_can_write_locked(self) -> None:
        # A completed call reports how it ended, which is more useful than
        # noting that the request stream was finished.
        if self._error is not None:
            raise self._error
        if self._status is not None:
            raise StreamClosedError(
                f'Cannot send on {self.method.full_name}: '
                'the call is already complete'
            )
        if self._stream_finished:
            raise StreamClosedError(
                f'Cannot write to {self.method.full_name}: '
                'the request stream is already finished'
            )

    def _check_can_finish_locked(self) -> None:
        if self._stream_finished or self.completed:
            raise StreamClosedError(
                f'Cannot finish {self.method.full_name}: '
                'the request stream is already closed'
            )

    def _poll_message_locked(self) -> bytes | None | _Pending:
        """Pops the next payload; returns None once the call completes.

        Must be called with the lock held.
        """
        if self._messages:
            return self._messages.popleft()
        if not self.completed:
            return _Pending.PENDING
        if self._error is not None:
            raise self._error
        return None

    def _poll_response_locked(self) -> Message | _Pending:
        """Returns the decoded single response once the call completes.

        Must be called with the lock held. Retains the response so that
        concurrent or repeated callers observe the same result or error.
        """
        if not self.completed:
            return _Pending.PENDING
        if self._error is not None:
            raise self._error
        if self._response is None:
            if not self._messages:
                raise RpcError(
                    self.method,
                    RpcStatus.PROTOCOL_ERROR,
                    'the server completed the call without a response',
                )
            self._response = self._decode(self._messages[0])
        return self._response

    def _wait(self, poll: Callable[[], _T | _Pending], timeout: float) -> _T:
        """Blocks until poll(), called with the lock held, has a result."""
        deadline = time.monotonic() + timeout
        with self._changed:
            while isinstance(result := poll(), _Pending):
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise RpcTimeout(self.method, timeout)
                self._changed.wait(None if math.isinf(timeout) else remaining)
            return result

    async def _wait_async(
        self, poll: Callable[[], _T | _Pending], timeout: float
    ) -> _T:
        """Awaits until poll(), called with the lock held, has a result."""
        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout

        while True:
            with self._changed:
                if not isinstance(result := poll(), _Pending):
                    return result
                waiter: asyncio.Future[None] = loop.create_future()
                self._async_waiters.append((loop, waiter))

            try:
                await asyncio.wait_for(
                    waiter,
                    None if math.isinf(timeout) else deadline - loop.time(),
                )
            except asyncio.TimeoutError:
                raise RpcTimeout(self.method, timeout) from None
            finally:
                with self._changed:
                    if (loop, waiter) in self._async_waiters:
                        self._async_waiters.remove((loop, waiter))

    def _decode(self, payload: bytes) -> Message:
        try:
            return self.method.response_type.FromString(payload)
        except ProtoDecodeError as error:
            raise DecodeError(
                f'Failed to decode {self.method.response_type.__name__} '
                f'for {self.method.full_name}: {error}',
                status=RpcStatus.DATA_ERROR,
            ) from error

    def _notify_locked(self) -> None:
        self._changed.notify_all()
        for loop, waiter in self._async_waiters:
            try:
                loop.call_soon_threadsafe(_wake, waiter)
            except RuntimeError:
                pass  # The event loop is closed, so nothing is waiting.
        self._async_waiters.clear()

    def __repr__(self) -> str:
        with self._changed:
            if not self.completed:
                state = 'active'
            elif self._status is not None:
                state = self._status.name
            else:
                state = type(self._error).__name__
        return f'<{self.method.full_name} call {self.call_id}: {state}>'


def _wake(waiter: asyncio.Future[None]) -> None:
    if not waiter.done():
        waiter.set_result(None)


def bind_write_signature(call: BaseCall) -> None:
    """Replaces ``call.write`` with a version that has the request's fields.

    This is for help text and IDEs: ``help(call.write)`` and
    ``inspect.signature(call.write)`` list the request's fields as keyword
    arguments, like the method clients do.
    """
    unbound = getattr(type(call), 'write')
    write = getattr(call, 'write')

    @functools.wraps(unbound)
    def write_with_signature(*args: Any, **kwargs: Any) -> Any:
        return write(*args, **kwargs)

    # pylint: disable-next=protected-access
    signature = call._method.request_signature()
    setattr(write_with_signature, '__signature__', signature)
    setattr(call, 'write', write_with_signature)


class BaseCall:
    """The state of an RPC, common to synchronous and asyncio calls."""

    def __init__(self, state: CallState, timeout: float) -> None:
        self._state = state
        self._timeout = timeout

    @property
    def _method(self) -> Method:
        return self._state.method

    @property
    def _call_id(self) -> int:
        return self._state.call_id

    @property
    def status(self) -> RpcStatus | None:
        """The :py:class:`RpcStatus` of the call, or ``None`` while active.

        A call that ended due to a local error, such as a lost connection, has
        no status.
        """
        return self._state.status

    @property
    def _protocol_error(self) -> ProtocolError | UnknownProtocolError | None:
        return self._state.protocol_error

    @property
    def completed(self) -> bool:
        """True once the call has ended, successfully or not."""
        return self._state.completed

    def _timeout_for(self, timeout: float | None) -> float:
        return resolve_timeout(timeout, self._timeout)

    def __repr__(self) -> str:
        return repr(self._state)


class Call(BaseCall):
    """Base class for handles to RPCs.

    Using a call as a context manager cancels it on exit if it is still
    active, which ensures that the server does not keep working on a call that
    the client abandoned:

    .. code-block:: python

       with service.StreamThings() as stream:
           for thing in stream:
               if is_what_i_wanted(thing):
                   break  # The call is cancelled here.
    """

    def cancel(self) -> bool:
        """Cancels the call, notifying the server.

        Returns:
          True if the call was cancelled; False if it had already completed.
        """
        return self._state.cancel()

    def __enter__(self: _CallT) -> _CallT:
        return self

    def __exit__(self, *exc_info: object) -> None:
        self.cancel()


class _ResponseStream(Call):
    """A call that receives a stream of responses."""

    def read(self, timeout: float | None = None) -> Any | None:
        """Waits for the next response.

        Returns:
          The next response, or ``None`` if the server finished the stream.

        Raises:
          RpcError: The call failed with a protocol error.
          RpcTimeout: No response arrived in time.
          DecodeError: The response or an incoming packet was malformed.
          SocketClosed: The connection was lost.
        """
        return self._state.read(self._timeout_for(timeout))

    def __iter__(self) -> Iterator[Any]:
        """Yields responses until the server finishes the stream.

        Each response is subject to the call's default timeout.
        """
        while (response := self.read()) is not None:
            yield response


class _RequestStream(Call):
    """A call that sends a stream of requests."""

    def __init__(self, state: CallState, timeout: float) -> None:
        super().__init__(state, timeout)
        bind_write_signature(self)

    def write(self, request: Message | None = None, /, **fields: Any) -> None:
        """Sends a request, given as a message or as keyword arguments.

        Raises:
          StreamClosedError: The call is already complete or the request stream
            is already finished.
          ValueError: The request is too large for the socket.
          RpcError: The call ended with a protocol error.
          SocketClosed: The connection is closed.
        """
        self._state.write(self._method.request(request, fields))


class UnaryCall(Call):
    """A unary RPC.

    Calling a unary method client waits for its response. Use
    :py:meth:`~pw_rpc2.MethodClient.invoke` to obtain a ``UnaryCall``
    instead, for example to issue several RPCs before waiting on any.
    """

    def response(self, timeout: float | None = None) -> Any:
        """Waits for and returns the response.

        Raises:
          RpcError: The call failed.
          RpcTimeout: The response did not arrive in time.
        """
        return self._state.response(self._timeout_for(timeout))


class ServerStreamingCall(_ResponseStream):
    """A server streaming RPC.

    Iterate over the call, or call :py:meth:`read`, to receive responses.
    """


class ClientStreamingCall(_RequestStream):
    """A client streaming RPC.

    Send requests with :py:meth:`write`, then call :py:meth:`finish` to end the
    request stream and receive the response.
    """

    def finish(self, timeout: float | None = None) -> Any:
        """Ends the request stream and waits for the response.

        The server may respond before the request stream ends, in which case
        this returns that response. ``timeout`` applies to waiting for the
        response after sending the stream end.

        Raises:
          RpcError: The call failed.
          RpcTimeout: The response did not arrive in time.
          DecodeError: The response or an incoming packet was malformed.
          SocketClosed: The connection was lost.
        """
        try:
            self._state.finish_stream()
        except StreamClosedError:
            pass
        return self._state.response(self._timeout_for(timeout))


class BidirectionalStreamingCall(_RequestStream, _ResponseStream):
    """A bidirectional streaming RPC.

    Send requests with :py:meth:`write` and receive responses with
    :py:meth:`read` or by iterating. Call :py:meth:`finish` once all requests
    are sent; the server may keep responding until it finishes the call.
    """

    def finish(self) -> None:
        """Ends the request stream.

        Raises:
          StreamClosedError: The call is already complete or the request stream
            is already finished.
        """
        self._state.finish_stream()
