# Copyright 2020 The Pigweed Authors
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
"""Types representing the basic pw_rpc concepts: channel, service, method."""

from __future__ import annotations

import abc
from dataclasses import dataclass
import enum
from inspect import Parameter
from typing import (
    Any,
    Callable,
    Collection,
    Generic,
    Iterable,
    Iterator,
    TypeVar,
)

from google.protobuf import descriptor_pb2, message_factory
from google.protobuf.descriptor import (
    FieldDescriptor,
    MethodDescriptor,
    ServiceDescriptor,
)
from google.protobuf.message import Message
from pw_protobuf_compiler import python_protos

from pw_rpc import ids


@dataclass(frozen=True)
class Channel:
    id: int
    output: Callable[[bytes], Any]

    def __repr__(self) -> str:
        return f'Channel({self.id})'


class ChannelManipulator(abc.ABC):
    """A a pipe interface that may manipulate packets before they're sent.

    ``ChannelManipulator``s allow application-specific packet handling to be
    injected into the packet processing pipeline for an ingress or egress
    channel-like pathway. This is particularly useful for integration testing
    resilience to things like packet loss on a usually-reliable transport. RPC
    server integrations (e.g. ``HdlcRpcLocalServerAndClient``) may provide an
    opportunity to inject a ``ChannelManipulator`` for this use case.

    A ``ChannelManipulator`` should not modify send_packet, as the consumer of a
    ``ChannelManipulator`` will use ``send_packet`` to insert the provided
    ``ChannelManipulator`` into a packet processing path.

    For example:

    .. code-block:: python

      class PacketLogger(ChannelManipulator):
          def process_and_send(self, packet: bytes) -> None:
              _LOG.debug('Received packet with payload: %s', str(packet))
              self.send_packet(packet)


      packet_logger = PacketLogger()

      # Configure actual send command.
      packet_logger.send_packet = socket.sendall

      # Route the output channel through the PacketLogger channel manipulator.
      channels = tuple(Channel(_DEFAULT_CHANNEL, packet_logger))

      # Create a RPC client.
      reader = SocketReader(socket)
      with reader:
          client = HdlcRpcClient(reader, protos, channels, stdout)
          with client:
            # Do something with client
    """

    def __init__(self) -> None:
        self.send_packet: Callable[[bytes], Any] = lambda _: None

    @abc.abstractmethod
    def process_and_send(self, packet: bytes) -> None:
        """Processes an incoming packet before optionally sending it.

        Implementations of this method may send the processed packet, multiple
        packets, or no packets at all via the registered `send_packet()`
        handler.
        """

    def __call__(self, data: bytes) -> None:
        self.process_and_send(data)


@dataclass(frozen=True, eq=False)
class Service:
    """Describes an RPC service."""

    _descriptor: ServiceDescriptor
    id: int
    methods: Methods

    @property
    def name(self):
        return self._descriptor.name

    @property
    def full_name(self):
        return self._descriptor.full_name

    @property
    def package(self):
        return self._descriptor.file.package

    @classmethod
    def from_descriptor(cls, descriptor: ServiceDescriptor) -> Service:
        service = cls(
            descriptor,
            ids.calculate(descriptor.full_name),
            None,  # type: ignore[arg-type]
        )
        object.__setattr__(
            service,
            'methods',
            Methods(
                Method.from_descriptor(method_descriptor, service)
                for method_descriptor in descriptor.methods
            ),
        )

        return service

    def __repr__(self) -> str:
        return f'Service({self.full_name!r})'

    def __str__(self) -> str:
        return self.full_name


def _streaming_attributes(method) -> tuple[bool, bool]:
    # TODO(hepler): Investigate adding server_streaming and client_streaming
    #     attributes to the generated protobuf code. As a workaround,
    #     deserialize the FileDescriptorProto to get that information.
    service = method.containing_service

    file_pb = descriptor_pb2.FileDescriptorProto()
    file_pb.MergeFromString(service.file.serialized_pb)

    method_pb = file_pb.service[service.index].method[
        method.index
    ]  # pylint: disable=no-member
    return method_pb.server_streaming, method_pb.client_streaming


_PROTO_FIELD_TYPES = {
    FieldDescriptor.TYPE_BOOL: bool,
    FieldDescriptor.TYPE_BYTES: bytes,
    FieldDescriptor.TYPE_DOUBLE: float,
    FieldDescriptor.TYPE_ENUM: int,
    FieldDescriptor.TYPE_FIXED32: int,
    FieldDescriptor.TYPE_FIXED64: int,
    FieldDescriptor.TYPE_FLOAT: float,
    FieldDescriptor.TYPE_INT32: int,
    FieldDescriptor.TYPE_INT64: int,
    FieldDescriptor.TYPE_SFIXED32: int,
    FieldDescriptor.TYPE_SFIXED64: int,
    FieldDescriptor.TYPE_SINT32: int,
    FieldDescriptor.TYPE_SINT64: int,
    FieldDescriptor.TYPE_STRING: str,
    FieldDescriptor.TYPE_UINT32: int,
    FieldDescriptor.TYPE_UINT64: int,
    # These types are not annotated:
    # FieldDescriptor.TYPE_GROUP = 10
    # FieldDescriptor.TYPE_MESSAGE = 11
}


def _is_repeated(field: Any) -> bool:
    # FieldDescriptor.is_repeated is supported in protobuf 5.26+ (Python
    # protobuf 5.x, 6.x, and 7.34+).
    if hasattr(field, 'is_repeated'):
        return field.is_repeated

    # FieldDescriptor.label was supported up through Python protobuf 6.33.x
    # for older versions (< 5.26 / 4.x / 3.x), but was removed in 7.34+
    # (protobuf 34.0+).
    return getattr(field, 'label', None) == getattr(
        FieldDescriptor, 'LABEL_REPEATED', 3
    )


def _field_type_annotation(field: FieldDescriptor):
    """Creates a field type annotation to use in the help message only."""
    if field.type == FieldDescriptor.TYPE_MESSAGE:
        assert field.message_type is not None
        annotation = message_factory.GetMessageClass(field.message_type)
    else:
        annotation = _PROTO_FIELD_TYPES.get(field.type, Parameter.empty)

    if _is_repeated(field):
        return Iterable[annotation]  # type: ignore[valid-type]

    return annotation


def field_help(proto_message, *, annotations: bool = False) -> Iterator[str]:
    """Yields argument strings for proto fields for use in a help message."""
    for field in proto_message.DESCRIPTOR.fields:
        if field.type == FieldDescriptor.TYPE_ENUM:
            value = field.enum_type.values_by_number[field.default_value].name
            type_name = field.enum_type.full_name
            value = f'{type_name.rsplit(".", 1)[0]}.{value}'
        else:
            type_name = _PROTO_FIELD_TYPES[field.type].__name__
            value = repr(field.default_value)

        if annotations:
            yield f'{field.name}: {type_name} = {value}'
        else:
            yield f'{field.name}={value}'


@dataclass(frozen=True, eq=False)
class Method:
    """Describes a method in a service."""

    _descriptor: MethodDescriptor
    service: Service
    id: int
    server_streaming: bool
    client_streaming: bool
    request_type: Any
    response_type: Any

    @classmethod
    def from_descriptor(
        cls, descriptor: MethodDescriptor, service: Service
    ) -> Method:
        return Method(
            descriptor,
            service,
            ids.calculate(descriptor.name),
            *_streaming_attributes(descriptor),
            message_factory.GetMessageClass(descriptor.input_type),
            message_factory.GetMessageClass(descriptor.output_type),
        )

    class Type(enum.Enum):
        UNARY = 0
        SERVER_STREAMING = 1
        CLIENT_STREAMING = 2
        BIDIRECTIONAL_STREAMING = 3

        def sentence_name(self) -> str:
            return self.name.lower().replace(
                '_', ' '
            )  # pylint: disable=no-member

    @property
    def name(self) -> str:
        return self._descriptor.name

    @property
    def full_name(self) -> str:
        return self._descriptor.full_name

    @property
    def package(self) -> str:
        return self._descriptor.containing_service.file.package

    @property
    def type(self) -> Method.Type:
        if self.server_streaming and self.client_streaming:
            return self.Type.BIDIRECTIONAL_STREAMING

        if self.server_streaming:
            return self.Type.SERVER_STREAMING

        if self.client_streaming:
            return self.Type.CLIENT_STREAMING

        return self.Type.UNARY

    def get_request(
        self, proto: Message | None, proto_kwargs: dict[str, Any] | None
    ) -> Message:
        """Returns a request_type protobuf message.

        The client implementation may use this to support providing a request
        as either a message object or as keyword arguments for the message's
        fields (but not both).
        """
        if proto_kwargs is None:
            proto_kwargs = {}

        if proto and proto_kwargs:
            proto_str = repr(proto).strip() or "''"
            raise TypeError(
                'Requests must be provided either as a message object or a '
                'series of keyword args, but both were provided '
                f"({proto_str} and {proto_kwargs!r})"
            )

        if proto is None:
            return self.request_type(**proto_kwargs)

        if not isinstance(proto, self.request_type):
            try:
                bad_type = proto.DESCRIPTOR.full_name
            except AttributeError:
                bad_type = type(proto).__name__

            raise TypeError(
                f'Expected a message of type '
                f'{self.request_type.DESCRIPTOR.full_name}, '
                f'got {bad_type}'
            )

        return proto

    def request_parameters(self) -> Iterator[Parameter]:
        """Yields inspect.Parameters corresponding to the request's fields.

        This can be used to make function signatures match the request proto.
        """
        for field in self.request_type.DESCRIPTOR.fields:
            yield Parameter(
                field.name,
                Parameter.KEYWORD_ONLY,
                annotation=_field_type_annotation(field),
                default=field.default_value,
            )

    def __repr__(self) -> str:
        req = self._method_parameter(self.request_type, self.client_streaming)
        res = self._method_parameter(self.response_type, self.server_streaming)
        return f'<{self.full_name}({req}) returns ({res})>'

    def _method_parameter(self, proto, streaming: bool) -> str:
        """Returns a description of the method's request or response type."""
        stream = 'stream ' if streaming else ''

        if proto.DESCRIPTOR.file.package == self.service.package:
            return stream + proto.DESCRIPTOR.name

        return stream + proto.DESCRIPTOR.full_name

    def __str__(self) -> str:
        return self.full_name


T = TypeVar('T')


def _name(item: Service | Method) -> str:
    return item.full_name if isinstance(item, Service) else item.name


class _AccessByName(Generic[T]):
    """Wrapper for accessing types by name within a proto package structure."""

    def __init__(self, name: str, item: T):
        setattr(self, name, item)


class ServiceAccessor(Collection[T]):
    """Navigates RPC services by name or ID."""

    def __init__(self, members, as_attrs: str = ''):
        """Creates accessor from an {item: value} dict or [values] iterable."""
        # If the members arg was passed as a [values] iterable, convert it to
        # an equivalent dictionary.
        if not isinstance(members, dict):
            members = {m: m for m in members}

        by_name: dict[str, Any] = {_name(k): v for k, v in members.items()}
        self._by_id = {k.id: v for k, v in members.items()}
        # Note: a dictionary is used rather than `setattr` in order to
        # (1) Hint to the type checker that there will be extra fields
        # (2) Ensure that built-in attributes such as `_by_id`` are not
        #     overwritten.
        self._attrs: dict[str, Any] = {}

        if as_attrs == 'members':
            for name, member in by_name.items():
                self._attrs[name] = member
        elif as_attrs == 'packages':
            for package in python_protos.as_packages(
                (m.package, _AccessByName(m.name, members[m])) for m in members
            ).packages:
                self._attrs[str(package)] = package
        elif as_attrs:
            raise ValueError(f'Unexpected value {as_attrs!r} for as_attrs')

    def __getattr__(self, name: str) -> Any:
        return self._attrs[name]

    def __getitem__(self, name_or_id: str | int) -> Any:
        """Accesses a service/method by the string name or ID."""
        try:
            return self._by_id[_id(name_or_id)]
        except KeyError:
            pass

        name = f' ("{name_or_id}")' if isinstance(name_or_id, str) else ''
        raise KeyError(f'Unknown ID {_id(name_or_id)}{name}')

    def __iter__(self) -> Iterator[T]:
        return iter(self._by_id.values())

    def __len__(self) -> int:
        return len(self._by_id)

    def __contains__(self, name_or_id) -> bool:
        return _id(name_or_id) in self._by_id

    def __repr__(self) -> str:
        members = ', '.join(repr(m) for m in self._by_id.values())
        return f'{self.__class__.__name__}({members})'


def _id(handle: str | int) -> int:
    return ids.calculate(handle) if isinstance(handle, str) else handle


class Methods(ServiceAccessor[Method]):
    """A collection of Method descriptors in a Service."""

    def __init__(self, method: Iterable[Method]):
        super().__init__(method)


class Services(ServiceAccessor[Service]):
    """A collection of Service descriptors."""

    def __init__(self, services: Iterable[Service]):
        super().__init__(services)


def get_method(service_accessor: ServiceAccessor, name: str):
    """Returns a method matching the given full name in a ServiceAccessor.

    Args:
      name: name as package.Service/Method or package.Service.Method.

    Raises:
      ValueError: the method name is not properly formatted
      KeyError: the method is not present
    """
    if '/' in name:
        service_name, method_name = name.split('/')
    else:
        service_name, method_name = name.rsplit('.', 1)

    service = service_accessor[service_name]
    if isinstance(service, Service):
        service = service.methods

    return service[method_name]


@dataclass(frozen=True)
class RpcIds:
    """Integer IDs that uniquely identify a remote procedure call."""

    channel_id: int
    service_id: int
    method_id: int
    call_id: int


@dataclass(frozen=True)
class PendingRpc:
    """Tracks an active RPC call."""

    channel: Channel
    service: Service
    method: Method
    call_id: int

    @property
    def channel_id(self) -> int:
        return self.channel.id

    @property
    def service_id(self) -> int:
        return self.service.id

    @property
    def method_id(self) -> int:
        return self.method.id

    def __eq__(self, other: Any) -> bool:
        if isinstance(other, PendingRpc):
            return self.ids() == other.ids()

        return NotImplemented

    def __hash__(self) -> int:
        return hash(self.ids())

    def ids(self) -> RpcIds:
        return RpcIds(
            self.channel.id, self.service.id, self.method.id, self.call_id
        )

    def __str__(self) -> str:
        return (
            f'PendingRpc(channel={self.channel.id}, method={self.method}, '
            f'call_id={self.call_id})'
        )

    def matches_channel_service_method(self, other: PendingRpc) -> bool:
        return (
            self.channel.id == other.channel.id
            and self.service.id == other.service.id
            and self.method.id == other.method.id
        )
