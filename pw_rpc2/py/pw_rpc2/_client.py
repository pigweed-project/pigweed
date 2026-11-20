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
"""A synchronous pw_rpc2 client.

Service and method clients are created at runtime from protobuf descriptors, so
no code generation is needed:

.. code-block:: py

   import pw_rpc2
   from pw_transport import framed_tcp
   from echo_pb import echo_pb2

   with pw_rpc2.Client(
       framed_tcp.connect('localhost', 8000), [echo_pb2]
   ) as client:
       echo = client.rpcs.examples.Echo

       # Calling a unary method returns its response.
       print(echo.Echo(msg='hello').msg)

       # Server streaming methods return a call to iterate over.
       with echo.Repeat(msg='hi', count=3) as call:
           for response in call:
               print(response.msg)

       # Client streaming calls are written to, then finished.
       with echo.Collect() as call:
           call.write(msg='one')
           call.write(msg='two')
           print(call.finish().msg)

       # Bidirectional streaming calls are written to and read from.
       with echo.EchoStream() as call:
           call.write(msg='ping')
           print(call.read().msg)
           call.finish()

All client objects are thread safe.
"""

from __future__ import annotations

import inspect
import threading
from typing import Any, Callable, Generic, Iterable, Iterator, TypeVar

from google.protobuf.descriptor import MethodDescriptor, ServiceDescriptor
from google.protobuf.message import Message

from pw_transport import ReliableSocket

from pw_rpc2 import _descriptors as descriptors
from pw_rpc2._call import (
    BaseCall,
    BidirectionalStreamingCall,
    Call,
    ClientStreamingCall,
    ServerStreamingCall,
    UnaryCall,
    resolve_timeout,
)
from pw_rpc2._connection import DEFAULT_HANDSHAKE_TIMEOUT, Connection
from pw_rpc2._descriptors import (
    MessageClass,
    Method,
    MethodType,
    ProtoSource,
    Service,
)

__all__ = [
    'BaseMethodClient',
    'Client',
    'MethodClient',
    'Package',
    'ServiceClient',
    'ServiceRegistry',
    'Services',
]

MethodClientT = TypeVar('MethodClientT')

# Method clients take the call's timeout and the request's fields as keyword
# arguments. The timeout's prefix keeps it from colliding with a field name.
_TIMEOUT_KEYWORD = 'rpc2_timeout'


class ServiceClient(Generic[MethodClientT]):
    """The method clients of a service, accessible as attributes or by name."""

    def __init__(
        self, service: Service, make_method: Callable[[Method], MethodClientT]
    ) -> None:
        self._service = service
        self._methods = {
            name: make_method(method)
            for name, method in service.methods.items()
        }

    @property
    def name(self) -> str:
        """The service's unqualified protobuf name, e.g. ``'Echo'``."""
        return self._service.name

    @property
    def full_name(self) -> str:
        """The service's fully qualified name, e.g. ``'examples.Echo'``."""
        return self._service.full_name

    @property
    def package(self) -> str:
        """The service's protobuf package name, e.g. ``'examples'``."""
        return self._service.package

    @property
    def descriptor(self) -> ServiceDescriptor:
        """The protobuf ``ServiceDescriptor`` for this service."""
        return self._service.descriptor

    def __getattr__(self, name: str) -> MethodClientT:
        # Protobuf names start with a letter, so names that start with an
        # underscore are never methods. Rejecting them up front also avoids
        # infinite recursion if an attribute is looked up before __init__ runs,
        # as happens when copying.
        if name.startswith('_'):
            raise AttributeError(name)
        try:
            return self._methods[name]
        except KeyError:
            raise AttributeError(
                f'{self._service.full_name} has no method {name!r}'
            ) from None

    def __getitem__(self, name: str) -> MethodClientT:
        try:
            return self._methods[name]
        except KeyError:
            raise KeyError(
                f'{self._service.full_name} has no method {name!r}'
            ) from None

    def __contains__(self, name: object) -> bool:
        return name in self._methods

    def __iter__(self) -> Iterator[MethodClientT]:
        return iter(self._methods.values())

    def __len__(self) -> int:
        return len(self._methods)

    def __dir__(self) -> list[str]:
        return [*super().__dir__(), *self._methods]

    def __repr__(self) -> str:
        methods = ''.join(
            f'\n  {method!r}' for method in self._methods.values()
        )
        return f'<service {self._service.full_name}>{methods}'


class Package(Generic[MethodClientT]):
    """A protobuf package, whose services and subpackages are attributes."""

    def __init__(self, name: str) -> None:
        self._name = name
        self._members: dict[
            str, Package[MethodClientT] | ServiceClient[MethodClientT]
        ] = {}

    def _add(self, service: ServiceClient[MethodClientT]) -> None:
        """Adds a service client, creating its subpackages as needed."""
        # pylint: disable=protected-access
        full_name = service.full_name
        *path, service_name = full_name.split('.')

        package = self
        for name in path:
            member = package._members.setdefault(
                name, Package(_join(package._name, name))
            )
            if not isinstance(member, Package):
                raise ValueError(
                    f'{member.full_name} is both a service and a package'
                )
            package = member

        if isinstance(package._members.get(service_name), Package):
            raise ValueError(f'{full_name} is both a package and a service')
        package._members[service_name] = service

    def __getattr__(self, name: str) -> Any:
        # See ServiceClient.__getattr__.
        if name.startswith('_'):
            raise AttributeError(name)
        try:
            return self._members[name]
        except KeyError:
            raise AttributeError(
                f'{self._name or "rpcs"} has no service or package {name!r}'
            ) from None

    def __dir__(self) -> list[str]:
        return [*super().__dir__(), *self._members]

    def __repr__(self) -> str:
        members = ''.join(f'\n  {name}' for name in self._members)
        return f'<package {self._name}>{members}'


def _join(package: str, name: str) -> str:
    return f'{package}.{name}' if package else name


class Services(Package[MethodClientT]):
    """RPC services, navigable by protobuf package.

    Attribute access walks the package hierarchy (``rpcs.examples.Echo``).
    Services can also be looked up by their full name
    (``rpcs['examples.Echo']``), and iterating yields every service.
    """

    def __init__(
        self, services: Iterable[ServiceClient[MethodClientT]] = ()
    ) -> None:
        super().__init__('')
        self._by_name: dict[str, ServiceClient[MethodClientT]] = {}
        for service in services:
            self._by_name[service.full_name] = service
            self._add(service)

    def __getitem__(self, full_name: str) -> ServiceClient[MethodClientT]:
        try:
            return self._by_name[full_name]
        except KeyError:
            raise KeyError(f'Unknown service {full_name!r}') from None

    def __contains__(self, full_name: object) -> bool:
        return full_name in self._by_name

    def __iter__(self) -> Iterator[ServiceClient[MethodClientT]]:
        return iter(self._by_name.values())

    def __len__(self) -> int:
        return len(self._by_name)

    def __repr__(self) -> str:
        services = ''.join(f'\n  {name}' for name in self._by_name)
        return f'<RPC services{services}>'


class ServiceRegistry(Generic[MethodClientT]):
    """Tracks a client's services and builds their method clients."""

    def __init__(self, make_method: Callable[[Method], MethodClientT]) -> None:
        self._make_method = make_method
        self._lock = threading.Lock()
        # Service clients by service ID.
        self._clients: dict[int, ServiceClient[MethodClientT]] = {}
        self.rpcs: Services[MethodClientT] = Services()

    def add(self, protos: Iterable[ProtoSource]) -> None:
        """Adds services, replacing existing services with the same names.

        The clients of the other services are unchanged.

        Raises:
          TypeError: Something in ``protos`` does not describe services.
          ValueError: Services or methods with different names have the same ID,
            or a service and a package share a name.
        """
        self.add_services(descriptors.services(protos))

    def add_services(self, new_services: Iterable[Service]) -> None:
        """Adds resolved Service objects to the registry."""
        # pylint: disable=protected-access
        new_services = list(new_services)

        with self._lock:
            clients = dict(self._clients)
            for service in new_services:
                other = clients.get(service._id)
                if other is not None and other.full_name != service.full_name:
                    raise ValueError(
                        f'Services {other.full_name} and {service.full_name} '
                        f'have the same ID ({service._id:#010x}); rename one '
                        'of them'
                    )
                clients[service._id] = ServiceClient(service, self._make_method)

            # Rebuild the namespace so that replaced services are dropped.
            # Only the added services get new clients; references to the
            # clients of the other services stay current.
            self.rpcs = Services(clients.values())
            self._clients = clients

    def methods(self) -> Iterator[MethodClientT]:
        """Yields every method client across all services."""
        for service in self.rpcs:
            yield from service


class BaseMethodClient:
    """The parts of a method client common to the sync and asyncio clients."""

    def __init__(self, method: Method, call_type: type[BaseCall]) -> None:
        self._method = method
        self.__signature__ = _method_signature(method)
        self.__doc__ = _method_doc(method, call_type)

    @property
    def name(self) -> str:
        """The method's unqualified protobuf name, e.g. ``'Echo'``."""
        return self._method.name

    @property
    def full_name(self) -> str:
        """The method's fully qualified name, e.g. ``'examples.Echo.Echo'``."""
        return self._method.full_name

    @property
    def type(self) -> MethodType:
        """The method's :py:class:`~pw_rpc2.MethodType` streaming mode."""
        return self._method.type

    @property
    def descriptor(self) -> MethodDescriptor:
        """The protobuf ``MethodDescriptor`` for this method."""
        return self._method.descriptor

    @property
    def request(self) -> MessageClass:
        """The protobuf message class for this method's requests."""
        return self._method.request_type

    @property
    def response(self) -> MessageClass:
        """The protobuf message class for this method's responses."""
        return self._method.response_type

    def _initial_payload(
        self, request: Message | None, fields: dict[str, Any]
    ) -> bytes:
        """Serializes the request that starts the RPC.

        Raises:
          TypeError: The request is invalid, or was given to a client or
            bidirectional streaming RPC.
          ValueError: A field name is not in the request type.
        """
        method = self._method
        if method.type.client_streaming:
            if request is not None or fields:
                raise TypeError(
                    f'{method.full_name} is a {method.type} RPC; send '
                    "requests with the call's write() method"
                )
            return b''

        return method.request(request, fields).SerializeToString()

    def __repr__(self) -> str:
        return f'<{self._method}>'


def _method_signature(method: Method) -> inspect.Signature:
    timeout = inspect.Parameter(
        _TIMEOUT_KEYWORD,
        inspect.Parameter.KEYWORD_ONLY,
        default=None,
        annotation='float | None',
    )
    if method.type.client_streaming:
        # Requests are sent with the call's write() method.
        return inspect.Signature([timeout])

    request, *fields = method.request_signature().parameters.values()
    # The timeout keyword always sets the timeout, so a request field with the
    # same name can only be set by passing a request message. Leave that field
    # out, since a signature cannot have two parameters with the same name.
    fields = [field for field in fields if field.name != _TIMEOUT_KEYWORD]
    return inspect.Signature([request, timeout, *fields])


def _method_doc(method: Method, call_type: type[BaseCall]) -> str:
    if method.type is MethodType.UNARY:
        returns = method.response_type.DESCRIPTOR.name
    else:
        returns = call_type.__name__
    return f'{method.describe()}\n\nReturns the {returns}.'


_CALL_TYPES: dict[MethodType, type[Call]] = {
    MethodType.UNARY: UnaryCall,
    MethodType.SERVER_STREAMING: ServerStreamingCall,
    MethodType.CLIENT_STREAMING: ClientStreamingCall,
    MethodType.BIDIRECTIONAL_STREAMING: BidirectionalStreamingCall,
}


class MethodClient(BaseMethodClient):
    """Invokes an RPC method.

    For unary methods, calling the method client waits for and returns the
    response. For streaming methods, it starts the RPC and returns its call
    object.

    Requests are given either as a message or as keyword arguments for the
    request's fields:

    .. code-block:: py

       echo.Echo(EchoMessage(msg='hello'))
       echo.Echo(msg='hello')

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
        """Calls the RPC: returns the response (unary) or call (streaming)."""
        call = self.invoke(request, rpc2_timeout=rpc2_timeout, **fields)

        if not isinstance(call, UnaryCall):
            return call

        # The caller never sees the call object, so cancel it if waiting for
        # the response is interrupted (e.g. by RpcTimeout or KeyboardInterrupt).
        with call:
            return call.response()

    def invoke(
        self,
        request: Message | None = None,
        /,
        *,
        rpc2_timeout: float | None = None,
        **fields: Any,
    ) -> Call:
        """Starts the RPC without waiting for a response.

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
          SocketClosed: The connection is closed.
        """
        payload = self._initial_payload(request, fields)
        timeout = resolve_timeout(rpc2_timeout, self._client.default_timeout)

        # pylint: disable-next=protected-access
        state = self._client._connection.start_call(self._method, payload)
        return _CALL_TYPES[self._method.type](state, timeout)


class Client:
    """A synchronous client for a pw_rpc2 server.

    The client owns its :py:class:`~pw_transport.ReliableSocket`; closing the
    client closes the socket and ends any active calls with ``CLOSED``. A
    background thread receives packets from the server.
    """

    def __init__(
        self,
        socket: ReliableSocket,
        protos: Iterable[ProtoSource] = (),
        *,
        default_timeout: float | None = None,
        handshake_timeout: float = DEFAULT_HANDSHAKE_TIMEOUT,
    ) -> None:
        """Performs the server handshake over ``socket``.

        Args:
          socket: A connected :py:class:`~pw_transport.ReliableSocket`, which
            the client takes ownership of.
          protos: Generated protobuf modules or descriptors of the services to
            create service clients for.
          default_timeout: The default for how long operations wait for a
            response from the server, in seconds; ``None`` waits forever.
          handshake_timeout: How long to wait for the server to reply to the
            connection handshake, in seconds.

        Raises:
          HandshakeError: The server did not complete the handshake.
          TypeError: Something in ``protos`` does not describe services.
          ValueError: Services or methods with different names have the same ID,
            a service and a package share a name, or a timeout is negative.
        """
        self._default_timeout: float | None = None
        self._registry: ServiceRegistry[MethodClient] = ServiceRegistry(
            lambda method: MethodClient(self, method)
        )

        try:
            self.default_timeout = default_timeout
            self._registry.add(protos)
            self._connection = Connection(socket, handshake_timeout)
        except BaseException:
            socket.close()
            raise

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

    def close(self) -> None:
        """Closes the connection, ending all active calls with CLOSED."""
        self._connection.close()

    def __enter__(self) -> Client:
        return self

    def __exit__(self, *exc_info: object) -> None:
        self.close()

    def __repr__(self) -> str:
        state = 'closed' if self.closed else 'open'
        services = ', '.join(service.full_name for service in self.rpcs)
        return f'<pw_rpc2.Client ({state}): {services}>'
