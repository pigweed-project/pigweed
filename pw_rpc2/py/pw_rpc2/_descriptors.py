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
"""Describes pw_rpc2 services and methods from protobuf descriptors.

Everything here is derived from ``google.protobuf`` descriptors alone. It does
not matter whether those descriptors come from generated ``_pb2`` modules or
were built at runtime in a :py:class:`~google.protobuf.descriptor_pool.
DescriptorPool` (for example, from serialized ``FileDescriptorProto``\\s).
"""

from __future__ import annotations

import dataclasses
import enum
import inspect
from types import ModuleType
from typing import Any, Iterable, Iterator, Mapping, Union

from google.protobuf import descriptor_pb2, message_factory
from google.protobuf.descriptor import (
    Descriptor,
    FieldDescriptor,
    FileDescriptor,
    MethodDescriptor,
    ServiceDescriptor,
)
from google.protobuf.message import Message

_HASH_CONSTANT = 65599

# A generated or dynamically created protobuf message class. This alias is
# needed because Method's `type` field shadows the builtin in its class body.
MessageClass = type[Message]


def rpc_id(name: str) -> int:
    """Calculates the 32-bit ID of a service or method from its name.

    Services are identified by their fully qualified name (e.g.
    ``examples.Echo``) and methods by their bare name (e.g. ``Echo``). This
    must match the hash used by pw_rpc2's code generators.
    """
    hash_value = len(name)
    coefficient = _HASH_CONSTANT

    for char in name:
        hash_value = (hash_value + coefficient * ord(char)) % 2**32
        coefficient = (coefficient * _HASH_CONSTANT) % 2**32

    return hash_value


class MethodType(enum.Enum):
    """The streaming mode of an RPC method."""

    UNARY = (False, False)
    SERVER_STREAMING = (False, True)
    CLIENT_STREAMING = (True, False)
    BIDIRECTIONAL_STREAMING = (True, True)

    @property
    def client_streaming(self) -> bool:
        """True if the client sends a stream of requests."""
        return self.value[0]

    @property
    def server_streaming(self) -> bool:
        """True if the server sends a stream of responses."""
        return self.value[1]

    def __str__(self) -> str:
        return self.name.lower().replace('_', ' ')


@dataclasses.dataclass(frozen=True, eq=False)
class Method:
    """An RPC method: its ID, streaming mode, and message types."""

    descriptor: MethodDescriptor = dataclasses.field(repr=False)
    service: Service = dataclasses.field(repr=False)
    _id: int
    type: MethodType
    request_type: MessageClass = dataclasses.field(repr=False)
    response_type: MessageClass = dataclasses.field(repr=False)

    @classmethod
    def from_descriptor(
        cls, descriptor: MethodDescriptor, service: Service
    ) -> Method:
        return cls(
            descriptor,
            service,
            rpc_id(descriptor.name),
            MethodType(
                (descriptor.client_streaming, descriptor.server_streaming)
            ),
            message_factory.GetMessageClass(descriptor.input_type),
            message_factory.GetMessageClass(descriptor.output_type),
        )

    @property
    def name(self) -> str:
        return self.descriptor.name

    @property
    def full_name(self) -> str:
        return self.descriptor.full_name

    def request(
        self,
        message: Message | None = None,
        fields: Mapping[str, Any] | None = None,
    ) -> Message:
        """Builds a request from a message or from its field values.

        Messages whose type has the same full name as the request type are
        accepted even if they are instances of another class, which happens
        when the same proto is loaded into more than one descriptor pool.

        Raises:
          TypeError: Both a message and fields were given, or the message is
            the wrong type.
          ValueError: A field name is not in the request type.
        """
        if message is None:
            return self.request_type(**(fields or {}))

        if fields:
            raise TypeError(
                f'Pass the {self.name} request either as a message or as '
                'keyword arguments, not both'
            )

        expected = self.request_type.DESCRIPTOR.full_name
        actual = (
            message.DESCRIPTOR.full_name if isinstance(message, Message) else ''
        )
        if actual != expected:
            raise TypeError(
                f'{self.name} requests must be {expected} messages, not '
                f'{actual or type(message).__name__}'
            )

        return message

    def request_signature(self) -> inspect.Signature:
        """A signature that takes a request message or its fields as keywords.

        This is for help text and IDEs; the annotations are strings.
        """
        request = self.descriptor.input_type
        parameters = [
            inspect.Parameter(
                'request',
                inspect.Parameter.POSITIONAL_ONLY,
                default=None,
                annotation=f'{request.full_name} | None',
            ),
            *(
                inspect.Parameter(
                    field.name,
                    inspect.Parameter.KEYWORD_ONLY,
                    default=_parameter_default(field),
                    annotation=_annotation(field),
                )
                for field in request.fields
            ),
        ]
        return inspect.Signature(parameters)

    def describe(self) -> str:
        """Describes the method and its message fields, for help text."""
        sections = [f'{str(self.type).capitalize()} RPC {self}.']

        request = self.descriptor.input_type
        if request.fields:
            sections.append(
                f'Request fields ({request.full_name}):\n'
                + _describe_fields(request, show_defaults=True)
            )

        response = self.descriptor.output_type
        if response.fields:
            sections.append(
                f'Response fields ({response.full_name}):\n'
                + _describe_fields(response, show_defaults=False)
            )

        return '\n\n'.join(sections)

    def __str__(self) -> str:
        request = self._message_name(self.request_type)
        response = self._message_name(self.response_type)
        request_stream = 'stream ' if self.type.client_streaming else ''
        response_stream = 'stream ' if self.type.server_streaming else ''
        return (
            f'{self.full_name}({request_stream}{request}) returns '
            f'({response_stream}{response})'
        )

    def _message_name(self, message_type: MessageClass) -> str:
        descriptor = message_type.DESCRIPTOR
        if descriptor.file.package == self.service.package:
            return descriptor.name
        return descriptor.full_name


@dataclasses.dataclass(frozen=True, eq=False)
class Service:
    """An RPC service: its ID and its methods, keyed by name."""

    descriptor: ServiceDescriptor = dataclasses.field(repr=False)
    _id: int
    methods: Mapping[str, Method] = dataclasses.field(repr=False)

    @classmethod
    def from_descriptor(cls, descriptor: ServiceDescriptor) -> Service:
        # pylint: disable=protected-access
        methods: dict[str, Method] = {}
        service = cls(descriptor, rpc_id(descriptor.full_name), methods)

        by_id: dict[int, Method] = {}
        for method_descriptor in descriptor.methods:
            method = Method.from_descriptor(method_descriptor, service)
            if (other := by_id.setdefault(method._id, method)) is not method:
                raise ValueError(
                    f'Methods {other.name} and {method.name} in '
                    f'{descriptor.full_name} have the same ID '
                    f'({method._id:#010x}); rename one of them'
                )
            methods[method.name] = method

        return service

    @property
    def name(self) -> str:
        return self.descriptor.name

    @property
    def full_name(self) -> str:
        return self.descriptor.full_name

    @property
    def package(self) -> str:
        return self.descriptor.file.package

    def __str__(self) -> str:
        return self.full_name


ProtoSource = Union[ModuleType, FileDescriptor, ServiceDescriptor]
"""Anything that describes RPC services.

This is a generated ``_pb2`` module, a :py:class:`FileDescriptor` (for example,
from a :py:class:`~google.protobuf.descriptor_pool.DescriptorPool`), or a
:py:class:`ServiceDescriptor`.
"""


def services(protos: Iterable[ProtoSource]) -> Iterator[Service]:
    """Yields every service described by the given protos.

    Raises:
      TypeError: An item is not a :py:data:`ProtoSource`.
    """
    for proto in protos:
        if isinstance(proto, ServiceDescriptor):
            yield Service.from_descriptor(proto)
            continue

        # Generated _pb2 modules store their FileDescriptor in DESCRIPTOR.
        file = getattr(proto, 'DESCRIPTOR', proto)
        if not isinstance(file, FileDescriptor):
            raise TypeError(
                'Expected a protobuf module, FileDescriptor, or '
                f'ServiceDescriptor; got {proto!r}'
            )

        for service in file.services_by_name.values():
            yield Service.from_descriptor(service)


_FIELD_TYPES = {
    FieldDescriptor.CPPTYPE_BOOL: 'bool',
    FieldDescriptor.CPPTYPE_DOUBLE: 'float',
    FieldDescriptor.CPPTYPE_FLOAT: 'float',
    FieldDescriptor.CPPTYPE_INT32: 'int',
    FieldDescriptor.CPPTYPE_INT64: 'int',
    FieldDescriptor.CPPTYPE_UINT32: 'int',
    FieldDescriptor.CPPTYPE_UINT64: 'int',
}


def _annotation(field: FieldDescriptor) -> str:
    """Returns a readable type annotation for a field, for help text only."""
    if field.type == FieldDescriptor.TYPE_BYTES:
        name = 'bytes'
    elif field.type == FieldDescriptor.TYPE_STRING:
        name = 'str'
    elif field.enum_type is not None:
        name = field.enum_type.full_name
    elif field.message_type is not None:
        name = field.message_type.full_name
    else:
        name = _FIELD_TYPES.get(field.cpp_type, 'Any')

    return f'Iterable[{name}]' if field.is_repeated else name


def _parameter_default(field: FieldDescriptor) -> Any:
    if field.is_required:
        return inspect.Parameter.empty
    if field.is_repeated or field.has_presence:
        return None
    return field.default_value


def _synthetic_oneof_indices(message: Descriptor) -> set[int]:
    if not message.oneofs:
        return set()
    proto = descriptor_pb2.DescriptorProto()
    message.CopyToProto(proto)
    return {field.oneof_index for field in proto.field if field.proto3_optional}


def _describe_field(
    field: FieldDescriptor, *, show_defaults: bool, in_oneof: bool = False
) -> str:
    if in_oneof:
        prefix = ''
    elif field.is_required:
        prefix = 'required '
    elif field.has_presence:
        prefix = 'optional '
    else:
        prefix = ''

    line = f'{prefix}{field.name}: {_annotation(field)}'
    if show_defaults:
        if field.is_repeated:
            line += ' = []'
        elif not field.is_required and (
            not field.has_presence or field.has_default_value
        ):
            line += f' = {field.default_value!r}'
    return line


def _describe_fields(message: Descriptor, *, show_defaults: bool) -> str:
    synthetic_oneofs = _synthetic_oneof_indices(message)
    seen_oneofs: set[int] = set()
    lines = []

    for field in message.fields:
        oneof = field.containing_oneof
        if oneof is not None and oneof.index not in synthetic_oneofs:
            if oneof.index not in seen_oneofs:
                seen_oneofs.add(oneof.index)
                lines.append(f'  oneof {oneof.name}:')
                for member in oneof.fields:
                    lines.append(
                        '    '
                        + _describe_field(
                            member,
                            show_defaults=show_defaults,
                            in_oneof=True,
                        )
                    )
            continue

        lines.append('  ' + _describe_field(field, show_defaults=show_defaults))

    return '\n'.join(lines)
