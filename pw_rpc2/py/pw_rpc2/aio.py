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
"""An asyncio pw_rpc2 client.

This mirrors :py:class:`pw_rpc2.Client`, with coroutines in place of blocking
operations:

.. code-block:: python

   from pw_rpc2 import aio
   from pw_transport import framed_tcp
   from echo_pb import echo_pb2

   async with await aio.Client.connect(
       lambda: framed_tcp.connect('localhost', 8080), [echo_pb2]
   ) as client:
       echo = client.rpcs.examples.Echo

       # Awaiting a unary method returns its response.
       print((await echo.Echo(msg='hello')).msg)

       # Server streaming methods return a call to iterate over.
       async with echo.Repeat(msg='hi', count=3) as call:
           async for response in call:
               print(response.msg)

       # Client streaming calls are written to, then finished.
       async with echo.Collect() as call:
           await call.write(msg='one')
           await call.write(msg='two')
           print((await call.finish()).msg)

       # Bidirectional streaming calls are written to and read from.
       async with echo.EchoStream() as call:
           await call.write(msg='ping')
           print((await call.read()).msg)
           await call.finish()

Starting a streaming RPC does not block, so streaming method clients return
their call directly.

Cancelling a task that is awaiting a unary RPC, as in
``await echo.Echo(msg='hello')``, cancels the RPC.

Cancelling a task that is awaiting an operation on a call, such as ``read()``,
``write()``, ``finish()``, or ``response()``, only stops the wait; it does not
cancel the RPC. To cancel the RPC, leave the call's ``async with`` block or
call ``cancel()``. A cancelled ``write()`` or ``finish()`` may still be sent,
because a packet that is already being sent cannot be recalled, so
``asyncio.CancelledError`` does not mean that the server did not receive the
request.
"""

from __future__ import annotations

import asyncio
import concurrent.futures
import queue
import threading
from typing import (
    Any,
    AsyncIterator,
    Callable,
    Iterable,
    Iterator,
    TypeVar,
)

from google.protobuf.message import Message
from pw_transport import ReliableSocket, SocketClosed

from pw_rpc2._call import (
    BaseCall,
    CallState,
    StreamClosedError,
    bind_write_signature,
    resolve_timeout,
)
from pw_rpc2._client import BaseMethodClient, ServiceRegistry, Services
from pw_rpc2._connection import DEFAULT_HANDSHAKE_TIMEOUT, Connection
from pw_rpc2._descriptors import Method, MethodType, ProtoSource

__all__ = [
    'BidirectionalStreamingCall',
    'Call',
    'Client',
    'ClientStreamingCall',
    'MethodClient',
    'Reader',
    'ServerStreamingCall',
    'UnaryCall',
    'Writer',
]

# The asyncio front-end shares internals with its call and method client
# classes.
# pylint: disable=protected-access

_T = TypeVar('_T')
_CallT = TypeVar('_CallT', bound='Call')


class Call(BaseCall):
    """Base class for handles to RPCs.

    Using a call as an async context manager cancels it on exit if it is
    still active.
    """

    def __init__(
        self, client: Client, state: CallState, timeout: float
    ) -> None:
        super().__init__(state, timeout)
        self._client = client

    def cancel(self) -> bool:
        """Cancels the call, notifying the server in the background.

        Returns:
          True if the call was cancelled; False if it had already completed.
        """
        return self._state.cancel()

    async def __aenter__(self: _CallT) -> _CallT:
        return self

    async def __aexit__(self, *exc_info: object) -> None:
        self.cancel()


class Reader(Call):
    """A call that receives a stream of responses.

    Both :py:class:`ServerStreamingCall` and
    :py:class:`BidirectionalStreamingCall` are readers, so functions that only
    consume responses can annotate their parameter as ``Reader``.
    """

    async def read(self, timeout: float | None = None) -> Any | None:
        """Waits for the next response.

        Returns:
          The next response, or ``None`` if the server finished the stream.

        Raises:
          RpcError: The call failed with a protocol error.
          RpcTimeout: No response arrived in time.
          DecodeError: The response or an incoming packet was malformed.
          SocketClosed: The connection was lost.
        """
        return await self._state.read_async(self._timeout_for(timeout))

    async def __aiter__(self) -> AsyncIterator[Any]:
        """Yields responses until the server finishes the stream.

        Each response is subject to the call's default timeout.
        """
        while (response := await self.read()) is not None:
            yield response


class Writer(Call):
    """A call that sends a stream of requests.

    Both :py:class:`ClientStreamingCall` and
    :py:class:`BidirectionalStreamingCall` are writers, so functions that only
    send requests can annotate their parameter as ``Writer``.
    """

    def __init__(
        self, client: Client, state: CallState, timeout: float
    ) -> None:
        super().__init__(client, state, timeout)
        bind_write_signature(self)

    async def write(
        self, request: Message | None = None, /, **fields: Any
    ) -> None:
        """Sends a request, given as a message or as keyword arguments.

        If the awaiting task is cancelled, the request may still be sent.

        Raises:
          StreamClosedError: The call is already complete or the request stream
            is already finished.
          ValueError: The request is too large for the socket.
          RpcError: The call ended with a protocol error.
          SocketClosed: The connection is closed.
        """
        message = self._method.request(request, fields)
        # Check here, not only on the sender thread, so that a call that ended
        # reports how it ended even after the client and its sender stopped.
        self._state.check_can_write()
        await self._client._run(self._state.write, message)


class UnaryCall(Call):
    """A unary RPC, as returned by :py:meth:`MethodClient.invoke`."""

    async def response(self, timeout: float | None = None) -> Any:
        """Waits for and returns the response.

        Raises:
          RpcError: The call failed.
          RpcTimeout: The response did not arrive in time.
          DecodeError: The response or an incoming packet was malformed.
          SocketClosed: The connection was lost.
        """
        return await self._state.response_async(self._timeout_for(timeout))


class ServerStreamingCall(Reader):
    """A server streaming RPC; iterate over it to receive responses."""


class ClientStreamingCall(Writer):
    """A client streaming RPC.

    Send requests with :py:meth:`write`, then await :py:meth:`finish` to end
    the request stream and receive the response.
    """

    async def finish(self, timeout: float | None = None) -> Any:
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
        resolved_timeout = self._timeout_for(timeout)
        if not self._state.stream_finished and not self._state.completed:
            try:
                await self._client._run(self._state.finish_stream)
            except StreamClosedError:
                pass
        return await self._state.response_async(resolved_timeout)


class BidirectionalStreamingCall(Writer, Reader):
    """A bidirectional streaming RPC.

    Send requests with :py:meth:`write` and receive responses with
    :py:meth:`read` or by iterating. Await :py:meth:`finish` once all requests
    are sent.
    """

    async def finish(self) -> None:
        """Ends the request stream.

        Raises:
          StreamClosedError: The call is already complete or the request stream
            is already finished.
        """
        self._state.check_can_finish()
        await self._client._run(self._state.finish_stream)


_CALL_TYPES: dict[MethodType, type[Call]] = {
    MethodType.UNARY: UnaryCall,
    MethodType.SERVER_STREAMING: ServerStreamingCall,
    MethodType.CLIENT_STREAMING: ClientStreamingCall,
    MethodType.BIDIRECTIONAL_STREAMING: BidirectionalStreamingCall,
}


class MethodClient(BaseMethodClient):
    """Invokes an RPC method.

    For unary methods, calling the method client returns a coroutine that
    starts the RPC and produces the response. For streaming methods, it starts
    the RPC and returns its call.

    The ``rpc2_timeout`` keyword argument sets the call's timeout. It has a
    prefix so that it does not collide with a request field named ``timeout``;
    a field named ``rpc2_timeout`` can only be set by passing a message.
    """

    def __init__(self, client: Client, method: Method) -> None:
        super().__init__(method, _CALL_TYPES[method.type])
        self._client = client

    def __call__(
        self,
        request: Message | None = None,
        /,
        *,
        rpc2_timeout: float | None = None,
        **fields: Any,
    ) -> Any:
        if self._method.type is MethodType.UNARY:
            payload = self._initial_payload(request, fields)
            timeout = resolve_timeout(
                rpc2_timeout, self._client.default_timeout
            )
            return self._call_unary(payload, timeout)
        return self.invoke(request, rpc2_timeout=rpc2_timeout, **fields)

    def invoke(
        self,
        request: Message | None = None,
        /,
        *,
        rpc2_timeout: float | None = None,
        **fields: Any,
    ) -> Call:
        """Starts the RPC without waiting for a response.

        This never blocks: the packet that starts the RPC is sent in the
        background. If sending it fails, the call ends with the error, which
        the call's next operation (e.g. ``read()``, ``write()``, or
        ``response()``) raises.

        Args:
          request: The request message; streaming requests are instead sent
            with the call's ``write()`` method.
          rpc2_timeout: Default timeout for waiting on a response from the
            server, in seconds. ``None`` uses the client's default.
          **fields: Request fields, as an alternative to ``request``.

        Returns:
          A call object whose type depends on the RPC type.

        Raises:
          TypeError: The request is invalid.
          ValueError: A field name is not in the request type, the request is
            too large for the socket, or the timeout is negative.
          DecodeError: A malformed packet closed the connection.
          SocketClosed: The connection was already closed.
        """
        payload = self._initial_payload(request, fields)
        timeout = resolve_timeout(rpc2_timeout, self._client.default_timeout)
        return self._start_call(payload, timeout)

    def _start_call(self, payload: bytes, timeout: float) -> Call:
        connection = self._client._connection
        state = connection.register_call(self._method)
        data = connection.encode_request(state, payload)
        try:
            self._client._submit(_send_request, connection, state, data)
        except SocketClosed:
            connection.abandon_request(state)
            raise
        return _CALL_TYPES[self._method.type](self._client, state, timeout)

    async def _call_unary(self, payload: bytes, timeout: float) -> Any:
        call = self._start_call(payload, timeout)
        assert isinstance(call, UnaryCall)
        async with call:
            return await call.response()


def _send_request(
    connection: Connection, state: CallState, data: bytes
) -> None:
    # The call may have ended while its request was queued, for example if it
    # was cancelled or timed out. The server never needs to hear of that call.
    if state.completed:
        connection.abandon_request(state)
        return
    try:
        connection.transmit_request(state, data)
    except Exception:  # pylint: disable=broad-except
        pass  # The failure ended the call, which reports it to the caller.


_Job = tuple[
    concurrent.futures.Future[Any], Callable[..., Any], tuple[Any, ...]
]


class _Sender:
    """Runs functions one at a time, in submission order, on its own thread.

    The thread is a daemon so that a send blocked on transport backpressure
    cannot keep the interpreter from exiting if the client is never closed.
    ``concurrent.futures.ThreadPoolExecutor`` workers are joined at exit.
    """

    def __init__(self) -> None:
        self._jobs: queue.SimpleQueue[_Job | None] = queue.SimpleQueue()
        self._lock = threading.Lock()
        self._shut_down = False
        self._thread = threading.Thread(
            target=self._run, name='pw_rpc2 sender', daemon=True
        )
        self._thread.start()

    def submit(
        self, function: Callable[..., _T], *args: Any
    ) -> concurrent.futures.Future[_T]:
        """Queues a function to run on the sender thread.

        Raises:
          SocketClosed: The sender has shut down.
        """
        future: concurrent.futures.Future[_T] = concurrent.futures.Future()
        with self._lock:
            if self._shut_down:
                raise SocketClosed('The client is closed')
            self._jobs.put((future, function, args))
        return future

    def shut_down(self) -> None:
        """Stops accepting functions; those already queued still run.

        This does not block, so it may be called from any thread.
        """
        with self._lock:
            if not self._shut_down:
                self._shut_down = True
                self._jobs.put(None)

    def join(self) -> None:
        """Waits for the sender thread to exit after :py:meth:`shut_down`."""
        self._thread.join()

    def _run(self) -> None:
        while (job := self._jobs.get()) is not None:
            _run_job(*job)
            del job  # Do not keep the job alive while waiting for the next.


def _run_job(
    future: concurrent.futures.Future[Any],
    function: Callable[..., Any],
    args: tuple[Any, ...],
) -> None:
    # Skip functions whose awaiting task was cancelled before they ran.
    if not future.set_running_or_notify_cancel():
        return
    try:
        result = function(*args)
    except BaseException as error:  # pylint: disable=broad-except
        future.set_exception(error)
    else:
        future.set_result(result)


async def _open_socket(
    target: Callable[[], ReliableSocket],
) -> ReliableSocket:
    """Calls ``target`` on a worker thread, closing the socket if cancelled."""
    lock = threading.Lock()
    cancelled = False
    opened_socket: ReliableSocket | None = None

    def open_target() -> ReliableSocket:
        nonlocal opened_socket
        with lock:
            if cancelled:
                raise asyncio.CancelledError
        result = target()
        with lock:
            if cancelled:
                should_close = True
            else:
                opened_socket = result
                should_close = False
        if should_close:
            result.close()
        return result

    try:
        return await asyncio.to_thread(open_target)
    except BaseException:
        with lock:
            cancelled = True
            socket_to_close = opened_socket
            opened_socket = None
        if socket_to_close is not None:
            await asyncio.to_thread(socket_to_close.close)
        raise


class Client:
    """An asyncio client for a pw_rpc2 server.

    Create clients with :py:meth:`connect`; there is no public constructor,
    since connecting requires awaiting the handshake. The client owns its
    :py:class:`~pw_transport.ReliableSocket`; closing the client closes the
    socket and ends any active calls with ``CLOSED``.

    Packets are received on a background thread and delivered to the event
    loop, and sent from a dedicated thread so that socket backpressure never
    blocks the event loop.
    """

    _default_timeout: float | None
    _connection: Connection
    _registry: ServiceRegistry[MethodClient]
    _sender: _Sender

    def __init__(self) -> None:
        raise TypeError(
            'aio.Client has no public constructor; create clients with '
            'aio.Client.connect()'
        )

    @classmethod
    async def connect(
        cls,
        target: ReliableSocket | Callable[[], ReliableSocket],
        protos: Iterable[ProtoSource] = (),
        *,
        default_timeout: float | None = None,
        handshake_timeout: float = DEFAULT_HANDSHAKE_TIMEOUT,
    ) -> Client:
        """Connects (if given a callable) and performs the server handshake.

        Args:
          target: A connected :py:class:`~pw_transport.ReliableSocket` (which
            the client takes ownership of) or a zero-argument callable
            returning one, which is called on a worker thread.
          protos: Generated protobuf modules or descriptors of the services to
            create service clients for.
          default_timeout: The default for how long operations wait for a
            response from the server, in seconds; ``None`` waits forever.
          handshake_timeout: How long to wait for the server to reply to the
            connection handshake, in seconds.

        Raises:
          HandshakeError: The server did not complete the handshake.
          OSError: Opening a socket through ``target`` failed.
          TypeError: Something in ``protos`` does not describe services.
          ValueError: Services or methods with different names have the same ID,
            a service and a package share a name, or a timeout is negative.
        """
        socket: ReliableSocket | None = None if callable(target) else target
        try:
            # __init__ is reserved for rejecting direct construction.
            client = cls.__new__(cls)
            client._default_timeout = None
            client.default_timeout = default_timeout
            resolve_timeout(handshake_timeout, DEFAULT_HANDSHAKE_TIMEOUT)
            client._registry = ServiceRegistry(
                lambda method: MethodClient(client, method)
            )
            # Validate protos (including service/package collisions) before
            # opening a socket or performing the handshake.
            client._registry.add(protos)

            if socket is None:
                assert callable(target)
                socket = await _open_socket(target)

            connection = await asyncio.to_thread(
                Connection, socket, handshake_timeout
            )
        except BaseException:
            if socket is not None:
                await asyncio.to_thread(socket.close)
            raise

        sender = _Sender()
        client._connection = connection
        client._sender = sender
        connection.set_on_close(sender.shut_down)
        return client

    @property
    def default_timeout(self) -> float | None:
        """How long operations wait for a response by default, in seconds.

        ``None`` waits forever.
        """
        return self._default_timeout

    @default_timeout.setter
    def default_timeout(self, timeout: float | None) -> None:
        if timeout is not None:
            resolve_timeout(timeout, None)
        self._default_timeout = timeout

    @property
    def rpcs(self) -> Services[MethodClient]:
        """The service clients, e.g. ``client.rpcs.examples.Echo``."""
        return self._registry.rpcs

    def methods(self) -> Iterator[MethodClient]:
        """Yields every method client across all registered services."""
        return self._registry.methods()

    def add_services(self, protos: Iterable[ProtoSource]) -> None:
        """Creates service clients for more services.

        A service with the same name as an existing service replaces it.

        Raises:
          TypeError: Something in ``protos`` does not describe services.
          ValueError: Services or methods with different names have the same ID,
            or a service and a package share a name.
        """
        self._registry.add(protos)

    @property
    def closed(self) -> bool:
        """True if the connection is closed, by either endpoint."""
        return self._connection.closed

    async def close(self) -> None:
        """Closes the connection, ending all active calls with CLOSED."""
        await asyncio.to_thread(self._close)

    def _close(self) -> None:
        self._connection.close()
        self._sender.shut_down()
        self._sender.join()

    async def __aenter__(self) -> Client:
        return self

    async def __aexit__(self, *exc_info: object) -> None:
        await self.close()

    def __repr__(self) -> str:
        state = 'closed' if self.closed else 'open'
        services = ', '.join(service.full_name for service in self.rpcs)
        return f'<pw_rpc2.aio.Client ({state}): {services}>'

    def _submit(
        self, function: Callable[..., _T], *args: Any
    ) -> concurrent.futures.Future[_T]:
        """Runs a function on the sender thread, in submission order.

        Raises:
          SocketClosed: The client is closed.
        """
        return self._sender.submit(function, *args)

    async def _run(self, function: Callable[..., _T], *args: Any) -> _T:
        return await asyncio.wrap_future(self._submit(function, *args))
