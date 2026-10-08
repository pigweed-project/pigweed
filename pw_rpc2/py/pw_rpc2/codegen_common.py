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
"""C++ code generation shared by the pw_rpc2 raw and pwpb protoc plugins.

Each plugin generates two headers for every .proto file:

- ``foo.<flavor>.rpc2.h``: the method descriptors, ``Client`` and
  ``Service<Impl>`` for every service in the file.
- ``foo.<flavor>_stubs.rpc2.h``: a stub service implementation to copy from.
  It is generated, but build rules do not put it on the include path.

All generated code refers to every name by its fully qualified name, since the
generated code is placed in namespaces derived from arbitrary proto packages.
"""

from __future__ import annotations

import abc
import dataclasses
import enum
import os
from typing import Iterable, Sequence

from google.protobuf import descriptor_pb2

from pw_protobuf.output_file import OutputFile
from pw_protobuf.symbol_name_mapping import fix_cc_identifier

_HASH_CONSTANT = 65599


# This is the same hash as pw_rpc.ids.calculate (and pw_tokenizer's hash,
# without the maximum length). It is duplicated to avoid a dependency on
# pw_rpc.
def hash_65599(string: str) -> int:
    """Computes the 32-bit 65599 hash of a string."""
    hash_value = len(string)
    coefficient = _HASH_CONSTANT

    for char in string:
        hash_value = (hash_value + coefficient * ord(char)) % 2**32
        coefficient = (coefficient * _HASH_CONSTANT) % 2**32

    return hash_value


class RpcIdCollisionError(ValueError):
    """Raised when two services or two methods hash to the same ID."""


class ReservedNameError(ValueError):
    """Raised when a method name collides with a generated identifier."""


class MethodType(enum.Enum):
    """The four RPC method types, with their C++ enumerator names."""

    UNARY = 'kUnary'
    SERVER_STREAMING = 'kServerStreaming'
    CLIENT_STREAMING = 'kClientStreaming'
    BIDIRECTIONAL_STREAMING = 'kBidirectionalStreaming'

    @classmethod
    def from_descriptor(
        cls, method: descriptor_pb2.MethodDescriptorProto
    ) -> MethodType:
        if method.client_streaming and method.server_streaming:
            return cls.BIDIRECTIONAL_STREAMING
        if method.client_streaming:
            return cls.CLIENT_STREAMING
        if method.server_streaming:
            return cls.SERVER_STREAMING
        return cls.UNARY

    def cpp(self) -> str:
        """The fully qualified C++ enumerator for this method type."""
        return f'::pw::rpc2::MethodType::{self.value}'

    def description(self) -> str:
        return {
            MethodType.UNARY: 'Unary',
            MethodType.SERVER_STREAMING: 'Server streaming',
            MethodType.CLIENT_STREAMING: 'Client streaming',
            MethodType.BIDIRECTIONAL_STREAMING: 'Bidirectional streaming',
        }[self]


@dataclasses.dataclass(frozen=True)
class Method:
    """An RPC method, as needed for code generation."""

    name: str
    type: MethodType
    request_type: str  # Fully qualified proto name, e.g. ".pkg.Msg".
    response_type: str

    @property
    def cpp_name(self) -> str:
        return fix_cc_identifier(self.name)

    @property
    def id(self) -> int:
        return hash_65599(self.name)


@dataclasses.dataclass(frozen=True)
class Service:
    """An RPC service, as needed for code generation."""

    name: str
    full_name: str  # Fully qualified proto name, without a leading dot.
    package: str
    methods: tuple[Method, ...]

    @property
    def cpp_name(self) -> str:
        return fix_cc_identifier(self.name)

    @property
    def id(self) -> int:
        return hash_65599(self.full_name)


def services_in(
    proto_file: descriptor_pb2.FileDescriptorProto,
) -> list[Service]:
    """Lists the services declared in a .proto file."""
    package = proto_file.package
    return [
        Service(
            name=service.name,
            full_name=f'{package}.{service.name}' if package else service.name,
            package=package,
            methods=tuple(
                Method(
                    name=method.name,
                    type=MethodType.from_descriptor(method),
                    request_type=method.input_type,
                    response_type=method.output_type,
                )
                for method in service.method
            ),
        )
        for service in proto_file.service
    ]


def cpp_namespace(package: str, *subnamespaces: str) -> str:
    """Returns the C++ namespace (without leading ::) for a proto package."""
    parts = [fix_cc_identifier(part) for part in package.split('.') if part]
    return '::'.join(parts + list(subnamespaces))


def hex_id(value: int) -> str:
    return f'0x{value:08x}'


def _check_ids(services: Sequence[Service]) -> None:
    """Checks that service IDs and each service's method IDs are unique."""
    seen_services: dict[int, Service] = {}
    for service in services:
        if service.id in seen_services:
            raise RpcIdCollisionError(
                f"Services '{seen_services[service.id].full_name}' and "
                f"'{service.full_name}' have the same service ID "
                f'({hex_id(service.id)}). Rename one of them.'
            )
        seen_services[service.id] = service

        seen_methods: dict[int, Method] = {}
        for method in service.methods:
            if method.id in seen_methods:
                raise RpcIdCollisionError(
                    f"Methods '{seen_methods[method.id].name}' and "
                    f"'{method.name}' in service '{service.full_name}' have "
                    f'the same method ID ({hex_id(method.id)}). Rename one of '
                    'them.'
                )
            seen_methods[method.id] = method


# Identifiers that generated code declares in a service's namespace, in its
# Client and Service<Impl> classes, or in their base classes, which a method of
# the same name would conflict with or hide.
_RESERVED_NAMES = frozenset(
    [
        # Service namespace
        'Client',
        'Service',
        # Client, pw::rpc2::internal::GeneratedServiceClient, and
        # pw::rpc2::ServiceClient
        'CallBidiStream',
        'CallClientStream',
        'CallServerStream',
        'CallServerStreamRaw',
        'CallUnary',
        'CallUnaryRaw',
        'GeneratedServiceClient',
        'ServiceClient',
        'client',
        'is_open',
        'kServiceId',
        # Service<Impl> and pw::rpc2::Service
        'FindMethod',
        'Impl',
        'ImplT',
        'derived',
        'kPwRpcMethods',
        'methods',
        'service_id',
    ]
)


def _generated_names(service: Service) -> dict[str, str]:
    """Maps each generated identifier to a description of what it is."""
    names = {name: 'a generated identifier' for name in _RESERVED_NAMES}
    names[f'{service.cpp_name}Service'] = 'the generated stub class'
    for method in service.methods:
        name = method.cpp_name
        names[f'{name}Future'] = f"the future type for method '{name}'"
        for prefix in ('HasMethod_', 'HasFuture_', 'Invoker_'):
            names[f'{prefix}{name}'] = f"generated for method '{name}'"
    return names


def _check_method_names(service: Service) -> None:
    """Checks that no method name collides with a generated identifier."""
    generated = _generated_names(service)
    seen: set[str] = set()

    for method in service.methods:
        name = method.cpp_name
        if name in seen:
            raise ReservedNameError(
                f"Method '{service.full_name}.{method.name}' has the same C++ "
                f"name '{name}' as another method."
            )
        seen.add(name)

        if name in generated:
            raise ReservedNameError(
                f"'{service.full_name}.{method.name}' is not a valid pw_rpc2 "
                f"method name: '{name}' is {generated[name]}. Rename the "
                'method.'
            )


def check_services(services: Sequence[Service]) -> None:
    """Validates a file's services. Raises ValueError if they are invalid."""
    _check_ids(services)
    for service in services:
        _check_method_names(service)


@dataclasses.dataclass(frozen=True)
class Param:
    type: str
    name: str


@dataclasses.dataclass(frozen=True)
class ServerParams:
    """The (request or reader, writer) parameters of a server method."""

    first: Param
    second: Param

    def declaration(self) -> str:
        return (
            f'{self.first.type} {self.first.name}, '
            f'{self.second.type} {self.second.name}'
        )

    def forward(self) -> str:
        return (
            f'::std::move({self.first.name}), ::std::move({self.second.name})'
        )


class CodeGenerator(abc.ABC):
    """Generates the C++ code for one flavor (raw or pwpb) of pw_rpc2."""

    def __init__(self, flavor: str) -> None:
        self.flavor = flavor

    @abc.abstractmethod
    def includes(self, services: Sequence[Service]) -> Iterable[str]:
        """Headers the main header needs in addition to the common ones."""

    @abc.abstractmethod
    def request_type(self, method: Method) -> str:
        """The fully qualified C++ request type of a method."""

    @abc.abstractmethod
    def response_type(self, method: Method) -> str:
        """The fully qualified C++ response type of a method."""

    @abc.abstractmethod
    def server_params(self, method: Method) -> ServerParams:
        """The parameters of a method's server implementation."""

    @abc.abstractmethod
    def write_client_method(self, output: OutputFile, method: Method) -> None:
        """Writes the Client member function for a method."""

    def service_namespace(self, service: Service) -> str:
        return cpp_namespace(
            service.package, 'pw_rpc2', self.flavor, service.cpp_name
        )


CLIENT_BASE = '::pw::rpc2::internal::GeneratedServiceClient'

_COMMON_INCLUDES = (
    '<array>',
    '<cstdint>',
    '<type_traits>',
    '<utility>',
    '"pw_rpc2/client.h"',
    '"pw_rpc2/internal/generated_service_client.h"',
    '"pw_rpc2/internal/method.h"',
    '"pw_rpc2/internal/method_info.h"',
    '"pw_rpc2/internal/method_invoker.h"',
    '"pw_rpc2/method_type.h"',
    '"pw_rpc2/reader.h"',
    '"pw_rpc2/service.h"',
    '"pw_rpc2/service_client.h"',
    '"pw_rpc2/writer.h"',
)

_STUBS_INCLUDES = (
    '"pw_async2/context.h"',
    '"pw_async2/poll.h"',
)


def _write_preamble(
    output: OutputFile, proto_file: str, includes: Iterable[str]
) -> None:
    output.write_line(
        f'// Generated by the pw_rpc2 codegen from {proto_file}. DO NOT EDIT.'
    )
    output.write_line('// clang-format off')
    output.write_line('#pragma once')

    unique = set(includes)
    system = sorted(i for i in unique if i.startswith('<'))
    local = sorted(i for i in unique if not i.startswith('<'))
    for group in (system, local):
        if group:
            output.write_line()
            for include in group:
                output.write_line(f'#include {include}')


def _write_namespace_open(output: OutputFile, namespace: str) -> None:
    output.write_line()
    output.write_line(f'namespace {namespace} {{')
    output.write_line()


def _write_namespace_close(output: OutputFile, namespace: str) -> None:
    output.write_line()
    output.write_line(f'}}  // namespace {namespace}')


def _write_method_tags(output: OutputFile, service: Service) -> None:
    output.write_line(f'// Method tags for {service.full_name}.')
    for method in service.methods:
        output.write_line(f'struct {method.cpp_name} final {{')
        with output.indent():
            output.write_line(f'{method.cpp_name}() = delete;')
        output.write_line('};')


def _write_method_info(
    output: OutputFile, gen: CodeGenerator, service: Service
) -> None:
    namespace = gen.service_namespace(service)
    for i, method in enumerate(service.methods):
        if i:
            output.write_line()
        output.write_line('template <>')
        output.write_line(
            f'struct MethodInfo<::{namespace}::{method.cpp_name}> {{'
        )
        with output.indent():
            output.write_line(
                'static constexpr ::std::uint32_t kServiceId = '
                f'{hex_id(service.id)};'
            )
            output.write_line(
                'static constexpr ::std::uint32_t kMethodId = '
                f'{hex_id(method.id)};'
            )
            output.write_line(
                'static constexpr ::pw::rpc2::MethodType kType = '
                f'{method.type.cpp()};'
            )
            output.write_line(f'using Request = {gen.request_type(method)};')
            output.write_line(f'using Response = {gen.response_type(method)};')
        output.write_line('};')


def _write_client(
    output: OutputFile, gen: CodeGenerator, service: Service
) -> None:
    output.write_line(f'class Client final : public {CLIENT_BASE} {{')
    output.write_line(' public:')
    with output.indent():
        output.write_line('constexpr Client() = default;')
        output.write_line()
        output.write_line('explicit Client(const ::pw::rpc2::Client& client)')
        output.write_line(f'    : {CLIENT_BASE}(client, kServiceId) {{}}')
        for method in service.methods:
            output.write_line()
            gen.write_client_method(output, method)
    output.write_line()
    output.write_line(' private:')
    with output.indent():
        output.write_line(
            'static constexpr ::std::uint32_t kServiceId = '
            f'{hex_id(service.id)};'
        )
    output.write_line('};')


def _write_invoker_selection(
    output: OutputFile, method: Method, method_info: str
) -> None:
    """Writes traits that select how Impl implements a method."""
    name = method.cpp_name
    future = f'{name}Future'

    output.write_line('template <typename T, typename = void>')
    output.write_line(f'struct HasMethod_{name} : ::std::false_type {{}};')
    output.write_line()
    output.write_line('template <typename T>')
    output.write_line(
        f'struct HasMethod_{name}<T, ::std::void_t<decltype(&T::{name})>>'
    )
    output.write_line(
        f'    : ::pw::rpc2::internal::IsMethodPointer<decltype(&T::{name})>'
        ' {};'
    )
    output.write_line()
    output.write_line('template <typename T, typename = void>')
    output.write_line(f'struct HasFuture_{name} : ::std::false_type {{}};')
    output.write_line()
    output.write_line('template <typename T>')
    output.write_line(
        f'struct HasFuture_{name}<T, ::std::void_t<typename T::{future}>>'
    )
    output.write_line('    : ::std::true_type {};')
    output.write_line()
    output.write_line('template <typename T, typename = void>')
    output.write_line(f'struct Invoker_{name} {{')
    with output.indent():
        output.write_line(
            'static_assert(::pw::rpc2::internal::kAlwaysFalse<T>,'
        )
        output.write_line(
            '              "Service implementation must define either a '
            f"member function named '{name}' or a future type named "
            f"'{future}'\");"
        )
    output.write_line('};')
    output.write_line()
    output.write_line('template <typename T>')
    output.write_line(f'struct Invoker_{name}<T, ::std::enable_if_t<')
    output.write_line(
        f'    HasMethod_{name}<T>::value && HasFuture_{name}<T>::value>> {{'
    )
    with output.indent():
        output.write_line(
            'static_assert(::pw::rpc2::internal::kAlwaysFalse<T>,'
        )
        output.write_line(
            '              "Service implementation must not define both a '
            f"member function named '{name}' and a future type named "
            f"'{future}'\");"
        )
    output.write_line('};')
    output.write_line()
    output.write_line('template <typename T>')
    output.write_line(f'struct Invoker_{name}<T, ::std::enable_if_t<')
    output.write_line(
        f'    HasMethod_{name}<T>::value && !HasFuture_{name}<T>::value>>'
    )
    output.write_line(
        f'    : ::pw::rpc2::internal::MethodInvokerFor<&T::{name}, '
        f'{method_info}> {{}};'
    )
    output.write_line()
    output.write_line('template <typename T>')
    output.write_line(f'struct Invoker_{name}<T, ::std::enable_if_t<')
    output.write_line(
        f'    !HasMethod_{name}<T>::value && HasFuture_{name}<T>::value>>'
    )
    output.write_line(
        '    : ::pw::rpc2::internal::FutureMethodInvokerFor<'
        f'typename T::{future}, {method_info}> {{}};'
    )


def _write_service(
    output: OutputFile, gen: CodeGenerator, service: Service
) -> None:
    namespace = gen.service_namespace(service)
    output.write_line(f'// Base class for {service.full_name} implementations.')
    output.write_line('//')
    output.write_line(
        '// For each method `Foo`, `Impl` must define either a member '
        'function `Foo`'
    )
    output.write_line(
        '// or a future type `FooFuture`, which is constructed from the '
        "method's"
    )
    output.write_line(
        '// arguments, optionally preceded by a reference to the service.'
    )
    output.write_line('template <typename Impl>')
    output.write_line('class Service : public ::pw::rpc2::Service {')
    if service.methods:
        output.write_line(' public:')
    with output.indent():
        if service.methods:
            output.write_line(
                '// Default implementations for `<Method>Future` types; '
                'hidden if `Impl`'
            )
            output.write_line('// defines `<Method>()` as a member function.')
        for i, method in enumerate(service.methods):
            if i:
                output.write_line()
            name = method.cpp_name
            params = gen.server_params(method)
            output.write_line('template <typename ImplT = Impl>')
            output.write_line(f'typename ImplT::{name}Future {name}(')
            output.write_line(f'    {params.declaration()}) {{')
            with output.indent():
                output.write_line(
                    'return ::pw::rpc2::internal::CreateFuture<'
                    f'typename ImplT::{name}Future>('
                )
                output.write_line(f'    derived(), {params.forward()});')
            output.write_line('}')

    if service.methods:
        output.write_line()
    output.write_line(' protected:')
    with output.indent():
        output.write_line(
            'constexpr Service() '
            ': ::pw::rpc2::Service(kServiceId, kPwRpcMethods) {}'
        )

    output.write_line()
    output.write_line(' private:')
    with output.indent():
        output.write_line(
            'static constexpr ::std::uint32_t kServiceId = '
            f'{hex_id(service.id)};'
        )
        for method in service.methods:
            output.write_line()
            _write_invoker_selection(
                output, method, f'::{namespace}::{method.cpp_name}'
            )
        output.write_line()
        output.write_line(
            'Impl& derived() { return static_cast<Impl&>(*this); }'
        )
        output.write_line()
        output.write_line(
            'static constexpr ::std::array<::pw::rpc2::internal::Method, '
            f'{len(service.methods)}> kPwRpcMethods = {{'
        )
        with output.indent():
            for method in service.methods:
                name = method.cpp_name
                output.write_line(
                    f'Invoker_{name}<Impl>::template CreateMethod<Impl>(),'
                )
        output.write_line('};')
    output.write_line('};')


def _write_stub(
    output: OutputFile, gen: CodeGenerator, service: Service
) -> None:
    impl = f'{service.cpp_name}Service'
    base = f'::{gen.service_namespace(service)}::Service<{impl}>'

    output.write_line(
        f'// Stub implementation of {service.full_name}. Copy this class to '
        'start an'
    )
    output.write_line('// implementation of the service.')
    output.write_line(f'class {impl} : public {base} {{')
    output.write_line(' public:')
    with output.indent():
        for i, method in enumerate(service.methods):
            name = method.cpp_name
            future = f'{name}Future'
            params = gen.server_params(method)
            if i:
                output.write_line()
            output.write_line(
                f'// {method.type.description()} RPC. Or implement as:'
            )
            output.write_line(
                f'//   SomeFuture {name}({params.declaration()});'
            )
            output.write_line(f'class {future} {{')
            output.write_line(' public:')
            with output.indent():
                output.write_line('using value_type = void;')
                output.write_line()
                output.write_line(f'{future}() = default;')
                output.write_line(
                    f'{future}({impl}& service, {params.declaration()});'
                )
                output.write_line()
                output.write_line('bool is_pendable() const;')
                output.write_line('bool is_complete() const;')
                output.write_line(
                    '::pw::async2::Poll<> Pend(::pw::async2::Context& cx);'
                )
            output.write_line('};')
    output.write_line('};')


def header_name(proto_file: str, flavor: str) -> str:
    return f'{os.path.splitext(proto_file)[0]}.{flavor}.rpc2.h'


def stubs_header_name(proto_file: str, flavor: str) -> str:
    return f'{os.path.splitext(proto_file)[0]}.{flavor}_stubs.rpc2.h'


def generate_headers(
    proto_file: descriptor_pb2.FileDescriptorProto, gen: CodeGenerator
) -> list[OutputFile]:
    """Generates the main and stubs headers for a .proto file."""
    services = services_in(proto_file)
    check_services(services)

    main_name = header_name(proto_file.name, gen.flavor)
    header = OutputFile(main_name)
    _write_preamble(
        header,
        proto_file.name,
        [*_COMMON_INCLUDES, *gen.includes(services)] if services else [],
    )

    for service in services:
        namespace = gen.service_namespace(service)
        _write_namespace_open(header, namespace)
        if service.methods:
            _write_method_tags(header, service)
            header.write_line()
        _write_client(header, gen, service)
        header.write_line()
        _write_service(header, gen, service)
        _write_namespace_close(header, namespace)

    services_with_methods = [s for s in services if s.methods]
    if services_with_methods:
        _write_namespace_open(header, 'pw::rpc2::internal')
        for i, service in enumerate(services_with_methods):
            if i:
                header.write_line()
            _write_method_info(header, gen, service)
        _write_namespace_close(header, 'pw::rpc2::internal')

    stubs = OutputFile(stubs_header_name(proto_file.name, gen.flavor))
    _write_preamble(
        stubs,
        proto_file.name,
        [f'"{main_name}"', *(_STUBS_INCLUDES if services else [])],
    )
    for service in services:
        namespace = gen.service_namespace(service)
        _write_namespace_open(stubs, namespace)
        _write_stub(stubs, gen, service)
        _write_namespace_close(stubs, namespace)

    return [header, stubs]
