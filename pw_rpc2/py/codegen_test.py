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
"""Tests for the pw_rpc2 raw and pwpb code generators."""

import re
from typing import Sequence
import unittest

from google.protobuf import descriptor_pb2

from pw_rpc2 import codegen_common, codegen_pwpb, codegen_raw
from pw_rpc2.codegen_common import (
    ReservedNameError,
    RpcIdCollisionError,
    hash_65599,
)


# Suffixes that collide under the 65599 hash after any common prefix, found by
# a random search. The hash is linear, so equal-length strings with the same
# prefix collide if and only if their suffixes do.
_COLLIDING_SUFFIXES = ('URcmZfaflO', 'apsUHJCajT')


def _method(
    name: str,
    request: str = '.pkg.Request',
    response: str = '.pkg.Response',
    client_streaming: bool = False,
    server_streaming: bool = False,
) -> descriptor_pb2.MethodDescriptorProto:
    return descriptor_pb2.MethodDescriptorProto(
        name=name,
        input_type=request,
        output_type=response,
        client_streaming=client_streaming,
        server_streaming=server_streaming,
    )


def _message(
    name: str, nested: Sequence[descriptor_pb2.DescriptorProto] = ()
) -> descriptor_pb2.DescriptorProto:
    return descriptor_pb2.DescriptorProto(name=name, nested_type=nested)


def _service(
    name: str, methods: Sequence[descriptor_pb2.MethodDescriptorProto]
) -> descriptor_pb2.ServiceDescriptorProto:
    return descriptor_pb2.ServiceDescriptorProto(name=name, method=methods)


def _file(
    name: str = 'pkg/test.proto',
    package: str = 'pkg',
    messages: Sequence[descriptor_pb2.DescriptorProto] = (
        _message('Request'),
        _message('Response'),
    ),
    services: Sequence[descriptor_pb2.ServiceDescriptorProto] = (),
    dependencies: Sequence[str] = (),
) -> descriptor_pb2.FileDescriptorProto:
    return descriptor_pb2.FileDescriptorProto(
        name=name,
        package=package,
        message_type=messages,
        service=services,
        dependency=dependencies,
    )


def _all_kinds(service: str = 'Kinds') -> descriptor_pb2.ServiceDescriptorProto:
    return _service(
        service,
        [
            _method('Unary'),
            _method('ServerStream', server_streaming=True),
            _method('ClientStream', client_streaming=True),
            _method('Bidi', client_streaming=True, server_streaming=True),
        ],
    )


def _raw(proto_file: descriptor_pb2.FileDescriptorProto) -> tuple[str, str]:
    """Returns the contents of the raw main and stubs headers."""
    header, stubs = codegen_raw.process_proto_file(proto_file)
    return header.content(), stubs.content()


def _pwpb(
    proto_file: descriptor_pb2.FileDescriptorProto,
    imports: Sequence[descriptor_pb2.FileDescriptorProto] = (),
) -> tuple[str, str]:
    """Returns the contents of the pwpb main and stubs headers."""
    header, stubs = codegen_pwpb.process_proto_file(proto_file, imports)
    return header.content(), stubs.content()


def _includes(header: str) -> list[str]:
    return re.findall(r'^#include (.*)$', header, re.MULTILINE)


# Matches a reference to the pw or std namespace that is not fully qualified.
_UNQUALIFIED = re.compile(r'(?<!namespace )(?<![:\w])(pw|std)::')


class HashTest(unittest.TestCase):
    """Tests the service and method ID hash."""

    def test_matches_pw_rpc_ids(self) -> None:
        # Values from pw_rpc.ids.calculate, which pw_rpc2 must agree with.
        self.assertEqual(hash_65599(''), 0)
        self.assertEqual(hash_65599('a'), 0x006117E0)
        self.assertEqual(hash_65599('Echo'), 0x8B470EE9)
        self.assertEqual(hash_65599('pw.rpc2.test.Echo'), 0x88543BAA)

    def test_colliding_suffixes_collide(self) -> None:
        first, second = _COLLIDING_SUFFIXES
        self.assertNotEqual(first, second)
        self.assertEqual(hash_65599(first), hash_65599(second))
        self.assertEqual(
            hash_65599(f'Method{first}'), hash_65599(f'Method{second}')
        )


class IdCollisionTest(unittest.TestCase):
    """Tests that colliding service and method IDs are rejected."""

    def test_method_ids_collide(self) -> None:
        first, second = _COLLIDING_SUFFIXES
        proto_file = _file(
            services=[
                _service(
                    'Svc',
                    [_method(f'Method{first}'), _method(f'Method{second}')],
                )
            ]
        )
        with self.assertRaisesRegex(RpcIdCollisionError, 'same method ID'):
            codegen_raw.process_proto_file(proto_file)
        with self.assertRaisesRegex(RpcIdCollisionError, 'same method ID'):
            codegen_pwpb.process_proto_file(proto_file)

    def test_service_ids_collide(self) -> None:
        first, second = _COLLIDING_SUFFIXES
        proto_file = _file(
            package='foo.bar',
            messages=[],
            services=[
                _service(f'Service{first}', []),
                _service(f'Service{second}', []),
            ],
        )
        with self.assertRaisesRegex(RpcIdCollisionError, 'same service ID'):
            codegen_raw.process_proto_file(proto_file)

    def test_same_method_in_different_services_is_allowed(self) -> None:
        proto_file = _file(
            services=[
                _service('First', [_method('Get')]),
                _service('Second', [_method('Get')]),
            ]
        )
        header, _ = _raw(proto_file)
        self.assertIn('namespace pkg::pw_rpc2::raw::First {', header)
        self.assertIn('namespace pkg::pw_rpc2::raw::Second {', header)

    def test_collision_errors_are_value_errors(self) -> None:
        self.assertTrue(issubclass(RpcIdCollisionError, ValueError))
        self.assertTrue(issubclass(ReservedNameError, ValueError))


class ReservedNameTest(unittest.TestCase):
    """Tests that method names that collide with generated code are rejected."""

    def _assert_rejected(
        self,
        methods: Sequence[descriptor_pb2.MethodDescriptorProto],
        service: str = 'Svc',
    ) -> None:
        proto_file = _file(services=[_service(service, methods)])
        with self.assertRaises(ReservedNameError):
            codegen_raw.process_proto_file(proto_file)
        with self.assertRaises(ReservedNameError):
            codegen_pwpb.process_proto_file(proto_file)

    def test_generated_identifiers(self) -> None:
        for name in (
            'Client',
            'Service',
            'kServiceId',
            'CallUnary',
            'CallBidiStream',
            'GeneratedServiceClient',
            'ServiceClient',
            'client',
            'is_open',
            'derived',
            'kPwRpcMethods',
            'Impl',
        ):
            with self.subTest(name=name):
                self._assert_rejected([_method(name)])

    def test_future_type_of_another_method(self) -> None:
        self._assert_rejected([_method('Foo'), _method('FooFuture')])
        self._assert_rejected([_method('FooFuture'), _method('Foo')])

    def test_per_method_identifiers(self) -> None:
        for prefix in ('HasMethod_', 'HasFuture_', 'Invoker_'):
            with self.subTest(prefix=prefix):
                self._assert_rejected([_method('Foo'), _method(f'{prefix}Foo')])

    def test_stub_class(self) -> None:
        self._assert_rejected([_method('EchoService')], service='Echo')

    def test_same_cpp_name(self) -> None:
        # `delete` is a C++ keyword, so it becomes `delete_`.
        self._assert_rejected([_method('delete'), _method('delete_')])

    def test_error_names_the_method(self) -> None:
        proto_file = _file(services=[_service('Svc', [_method('Client')])])
        with self.assertRaisesRegex(ReservedNameError, r"'pkg\.Svc\.Client'"):
            codegen_raw.process_proto_file(proto_file)

    def test_similar_names_are_allowed(self) -> None:
        header, _ = _raw(
            _file(
                services=[
                    _service(
                        'Svc',
                        [
                            _method('Clients'),
                            _method('Reader'),
                            _method('Writer'),
                            _method('FooFuture'),
                        ],
                    )
                ]
            )
        )
        self.assertIn('RawUnaryReserveFuture FooFuture(', header)

    def test_keyword_method_name(self) -> None:
        header, _ = _raw(_file(services=[_service('Svc', [_method('delete')])]))
        self.assertIn(
            'struct delete_ final {\n  delete_() = delete;\n};', header
        )
        self.assertIn(
            'struct MethodInfo<::pkg::pw_rpc2::raw::Svc::delete_> {', header
        )
        self.assertIn('RawUnaryReserveFuture delete_(', header)
        # The ID is the hash of the proto name, not the C++ name.
        delete_id = f'{hash_65599("delete"):#010x}'
        self.assertIn(
            f'CallUnaryRaw({delete_id}, max_payload_size)',
            header,
        )
        self.assertIn(f'kMethodId = {delete_id};', header)


class HeaderNameTest(unittest.TestCase):
    """Tests the names of the generated files."""

    def test_raw(self) -> None:
        names = [
            output.name()
            for output in codegen_raw.process_proto_file(
                _file(name='dir/foo.proto')
            )
        ]
        self.assertEqual(
            names, ['dir/foo.raw.rpc2.h', 'dir/foo.raw_stubs.rpc2.h']
        )

    def test_pwpb(self) -> None:
        names = [
            output.name()
            for output in codegen_pwpb.process_proto_file(
                _file(name='dir/foo.proto')
            )
        ]
        self.assertEqual(
            names, ['dir/foo.pwpb.rpc2.h', 'dir/foo.pwpb_stubs.rpc2.h']
        )


class CommonOutputTest(unittest.TestCase):
    """Tests output that is the same for both flavors."""

    def test_file_without_services(self) -> None:
        for process in (_raw, _pwpb):
            with self.subTest(process=process):
                header, stubs = process(_file(name='dir/foo.proto'))
                self.assertEqual(_includes(header), [])
                self.assertIn('#pragma once', header)
                self.assertNotIn('namespace', header)
                self.assertEqual(len(_includes(stubs)), 1)
                self.assertNotIn('namespace', stubs)

    def test_fully_qualified(self) -> None:
        proto_file = _file(services=[_all_kinds()])
        for process in (_raw, _pwpb):
            for content in process(proto_file):
                with self.subTest(process=process):
                    self.assertIsNone(
                        _UNQUALIFIED.search(content),
                        'Generated code must use fully qualified names',
                    )

    def test_ids_are_private(self) -> None:
        header, _ = _raw(_file(services=[_service('Svc', [_method('Get')])]))
        service_id = f'{hash_65599("pkg.Svc"):#010x}'
        constants = (
            ' private:\n'
            f'  static constexpr ::std::uint32_t kServiceId = {service_id};\n'
        )
        # Once in the Client and once in the Service.
        self.assertEqual(header.count(constants), 2)
        self.assertNotIn('kMethodId_', header)

    def test_client_constructors(self) -> None:
        header, _ = _raw(_file(services=[_service('Svc', [_method('Get')])]))
        self.assertEqual(len(re.findall(r'\bClient\(', header)), 2)
        self.assertIn('  constexpr Client() = default;\n', header)
        self.assertIn(
            '  explicit Client(const ::pw::rpc2::Client& client)\n'
            '      : ::pw::rpc2::internal::GeneratedServiceClient('
            'client, kServiceId) {}',
            header,
        )

    def test_service_contract_errors(self) -> None:
        header, _ = _raw(_file(services=[_service('Svc', [_method('Get')])]))
        self.assertIn(
            '"Service implementation must define either a member function '
            "named 'Get' or a future type named 'GetFuture'\"",
            header,
        )
        self.assertIn(
            '"Service implementation must not define both a member function '
            "named 'Get' and a future type named 'GetFuture'\"",
            header,
        )

    def test_derived_and_traits_are_private(self) -> None:
        header, _ = _raw(_file(services=[_service('Svc', [_method('Get')])]))
        service = header[header.index('class Service ') :]
        private = service.index(' private:')
        for name in (
            'HasMethod_Get',
            'HasFuture_Get',
            'Invoker_Get',
            'Impl& derived()',
        ):
            with self.subTest(name=name):
                self.assertGreater(service.index(name), private)

    def test_constructor_is_constexpr_and_protected(self) -> None:
        for methods in ([_method('Get')], []):
            proto_file = _file(services=[_service('Svc', methods)])
            for process in (_raw, _pwpb):
                header, stubs = process(proto_file)
                with self.subTest(process=process, methods=len(methods)):
                    svc = header[header.index('class Service ') :]
                    svc = svc[: svc.index('\n};')]
                    ctors = re.findall(
                        r'^ *(?:constexpr )?Service\(', svc, re.M
                    )
                    self.assertEqual(ctors, ['  constexpr Service('])
                    ctor = svc.index(ctors[0])
                    self.assertGreater(ctor, svc.index(' protected:'))
                    self.assertLess(ctor, svc.index(' private:'))
                    self.assertEqual(' public:' in svc, bool(methods))
                    for text in (header, stubs):
                        self.assertNotIn('Dispatcher', text)
                        self.assertNotIn('Allocator', text)

    def test_no_aliases(self) -> None:
        proto_file = _file(services=[_all_kinds()])
        for process in (_raw, _pwpb):
            header, stubs = process(proto_file)
            with self.subTest(process=process):
                self.assertNotIn('using Reader', header)
                self.assertNotIn('using Raw', header)
                self.assertNotIn('Coro', header)
                self.assertNotIn('Deserialize', header)
                self.assertEqual(
                    re.findall(r'^\s*using (?!Request|Response)', header, re.M),
                    [],
                )
                self.assertNotIn('using ::', stubs)

    def test_package_with_keyword(self) -> None:
        header, _ = _raw(
            _file(
                package='pw.delete.v1',
                services=[_service('Svc', [_method('Get')])],
            )
        )
        self.assertIn('namespace pw::delete_::v1::pw_rpc2::raw::Svc {', header)
        # The service ID is the hash of the proto name.
        self.assertIn(f'{hash_65599("pw.delete.v1.Svc"):#010x}', header)

    def test_multiple_services(self) -> None:
        for process, flavor in ((_raw, 'raw'), (_pwpb, 'pwpb')):
            header, stubs = process(
                _file(services=[_all_kinds('First'), _all_kinds('Second')])
            )
            with self.subTest(flavor=flavor):
                for service in ('First', 'Second'):
                    self.assertIn(
                        f'namespace pkg::pw_rpc2::{flavor}::{service} {{',
                        header,
                    )
                    self.assertIn(
                        f'namespace pkg::pw_rpc2::{flavor}::{service} {{', stubs
                    )
                    self.assertIn(
                        f'class {service}Service : public '
                        f'::pkg::pw_rpc2::{flavor}::'
                        f'{service}::Service<{service}Service> {{',
                        stubs,
                    )

    def test_stubs_include_main_header(self) -> None:
        for process, flavor in ((_raw, 'raw'), (_pwpb, 'pwpb')):
            _, stubs = process(
                _file(name='dir/foo.proto', services=[_all_kinds()])
            )
            with self.subTest(flavor=flavor):
                self.assertIn(f'"dir/foo.{flavor}.rpc2.h"', _includes(stubs))
                self.assertNotIn('class Client final', stubs)


class RawCodegenTest(unittest.TestCase):
    """Tests code generated with raw payloads."""

    def setUp(self) -> None:
        self.header, self.stubs = _raw(_file(services=[_all_kinds()]))

    def test_includes(self) -> None:
        includes = _includes(self.header)
        self.assertEqual(
            includes,
            sorted(i for i in includes if i.startswith('<'))
            + sorted(i for i in includes if i.startswith('"')),
        )
        self.assertIn('"pw_buf/buf.h"', includes)
        self.assertIn('"pw_rpc2/internal/generated_service_client.h"', includes)
        self.assertIn('"pw_rpc2/service_client.h"', includes)
        self.assertIn('<cstddef>', includes)
        self.assertFalse(any('pwpb' in include for include in includes))

    def test_method_descriptors(self) -> None:
        service_id = f'{hash_65599("pkg.Kinds"):#010x}'
        for name, kind in (
            ('Unary', 'kUnary'),
            ('ServerStream', 'kServerStreaming'),
            ('ClientStream', 'kClientStreaming'),
            ('Bidi', 'kBidirectionalStreaming'),
        ):
            with self.subTest(name=name):
                self.assertIn(
                    f'struct {name} final {{\n  {name}() = delete;\n}};',
                    self.header,
                )
                self.assertIn(
                    'template <>\n'
                    f'struct MethodInfo<::pkg::pw_rpc2::raw::Kinds::{name}> '
                    '{\n'
                    '  static constexpr ::std::uint32_t kServiceId = '
                    f'{service_id};\n'
                    '  static constexpr ::std::uint32_t kMethodId = '
                    f'{hash_65599(name):#010x};\n'
                    '  static constexpr ::pw::rpc2::MethodType kType = '
                    f'::pw::rpc2::MethodType::{kind};\n'
                    '  using Request = ::pw::ConstBuf;\n'
                    '  using Response = ::pw::ConstBuf;\n'
                    '};',
                    self.header,
                )

    def test_client(self) -> None:
        for signature in (
            '[[nodiscard]] ::pw::rpc2::RawUnaryReserveFuture Unary('
            '::std::size_t max_payload_size) const {',
            '[[nodiscard]] ::pw::rpc2::RawServerStreamReserveFuture '
            'ServerStream(::std::size_t max_payload_size) const {',
            '[[nodiscard]] ::pw::rpc2::RawClientStreamFuture '
            'ClientStream() const {',
            '[[nodiscard]] ::pw::rpc2::RawBidiStreamFuture Bidi() const {',
        ):
            with self.subTest(signature=signature):
                self.assertIn(signature, self.header)
        # There is no overload that copies a request.
        client = self.header.split('class Service', maxsplit=1)[0]
        self.assertNotIn('::pw::ConstBuf request', client)

    def test_service(self) -> None:
        for name, params in (
            (
                'Unary',
                '::pw::ConstBuf request, ::pw::rpc2::RawUnaryWriter writer',
            ),
            (
                'ServerStream',
                '::pw::ConstBuf request, ::pw::rpc2::RawWriter writer',
            ),
            (
                'ClientStream',
                '::pw::rpc2::RawReader reader, '
                '::pw::rpc2::RawUnaryWriter writer',
            ),
            (
                'Bidi',
                '::pw::rpc2::RawReader reader, ::pw::rpc2::RawWriter writer',
            ),
        ):
            with self.subTest(name=name):
                self.assertIn(f'{name}(\n      {params})', self.header)
                self.assertIn(f'{name}({params})', self.stubs)
        self.assertIn(
            '::pw::rpc2::internal::MethodInvokerFor<&T::Bidi, '
            '::pkg::pw_rpc2::raw::Kinds::Bidi>',
            self.header,
        )
        self.assertIn(
            '::pw::rpc2::internal::FutureMethodInvokerFor<typename '
            'T::BidiFuture, ::pkg::pw_rpc2::raw::Kinds::Bidi>',
            self.header,
        )

    def test_stubs(self) -> None:
        self.assertIn(
            'class KindsService : public ::pkg::pw_rpc2::raw::Kinds::'
            'Service<KindsService> {',
            self.stubs,
        )
        self.assertIn(
            'BidiFuture(KindsService& service, ::pw::rpc2::RawReader reader, '
            '::pw::rpc2::RawWriter writer);',
            self.stubs,
        )

    def test_package_less(self) -> None:
        header, stubs = _raw(
            _file(
                package='',
                messages=[],
                services=[_service('Svc', [_method('Get', '.Req', '.Resp')])],
            )
        )
        self.assertIn('namespace pw_rpc2::raw::Svc {', header)
        self.assertIn('namespace pw_rpc2::raw::Svc {', stubs)
        self.assertIn('::pw_rpc2::raw::Svc::Service<SvcService>', stubs)
        self.assertIn(f'kServiceId = {hash_65599("Svc"):#010x};', header)


class PwpbCodegenTest(unittest.TestCase):
    """Tests code generated with pw_protobuf message structs."""

    def test_method_named_after_message(self) -> None:
        header, _ = _pwpb(
            _file(
                package='pkg',
                messages=[_message('Ping'), _message('Pong')],
                services=[
                    _service(
                        'Svc',
                        [
                            _method('Ping', '.pkg.Ping', '.pkg.Pong'),
                            _method('Pong', '.pkg.Ping', '.pkg.Pong'),
                        ],
                    )
                ],
            )
        )
        self.assertIn(
            'struct Ping final {\n  Ping() = delete;\n};',
            header,
        )
        self.assertIn(
            'struct MethodInfo<::pkg::pw_rpc2::pwpb::Svc::Ping> {',
            header,
        )
        self.assertIn('using Request = ::pkg::pwpb::Ping::Message;', header)
        self.assertIn('using Response = ::pkg::pwpb::Pong::Message;', header)
        self.assertIn(
            '  [[nodiscard]] ::pw::rpc2::UnaryFuture<'
            '::pkg::pwpb::Ping::Message, ::pkg::pwpb::Pong::Message>\n'
            '  Pong(const ::pkg::pwpb::Ping::Message& request) const {',
            header,
        )

    def test_keyword_and_nested_messages(self) -> None:
        """Keyword and nested message names map to their C++ structs."""
        header, _ = _pwpb(
            _file(
                package='pkg.v1',
                messages=[
                    _message('delete'),
                    _message('Outer', [_message('Inner', [_message('Deep')])]),
                ],
                services=[
                    _service(
                        'Svc',
                        [
                            _method(
                                'Delete', '.pkg.v1.delete', '.pkg.v1.delete'
                            ),
                            _method(
                                'Nested',
                                '.pkg.v1.Outer.Inner',
                                '.pkg.v1.Outer.Inner.Deep',
                            ),
                        ],
                    )
                ],
            )
        )
        self.assertIn(
            'using Request = ::pkg::v1::pwpb::delete_::Message;', header
        )
        self.assertIn(
            'using Request = ::pkg::v1::pwpb::Outer::Inner::Message;', header
        )
        self.assertIn(
            'using Response = ::pkg::v1::pwpb::Outer::Inner::Deep::Message;',
            header,
        )

    def test_imported_messages(self) -> None:
        """Messages from imported files, with or without packages, resolve."""
        common = _file(
            name='common/common.proto',
            package='common',
            messages=[_message('Shared', [_message('Inner')])],
        )
        no_package = _file(
            name='np/messages.proto', package='', messages=[_message('Loose')]
        )
        header, _ = _pwpb(
            _file(
                name='svc/svc.proto',
                package='svc',
                messages=[_message('Local')],
                services=[
                    _service(
                        'Svc',
                        [
                            _method(
                                'A', '.common.Shared', '.common.Shared.Inner'
                            ),
                            _method('B', '.Loose', '.svc.Local'),
                        ],
                    )
                ],
                dependencies=[common.name, no_package.name],
            ),
            [common, no_package],
        )
        self.assertIn(
            'using Request = ::common::pwpb::Shared::Message;', header
        )
        self.assertIn(
            'using Response = ::common::pwpb::Shared::Inner::Message;', header
        )
        self.assertIn('using Request = ::Loose::Message;', header)
        self.assertIn('using Response = ::svc::pwpb::Local::Message;', header)

        includes = _includes(header)
        for include in (
            '"common/common.pwpb.h"',
            '"np/messages.pwpb.h"',
            '"svc/svc.pwpb.h"',
            '"pw_rpc2/internal/pwpb_serialize.h"',
        ):
            with self.subTest(include=include):
                self.assertIn(include, includes)
        self.assertNotIn('<cstddef>', includes)
        self.assertNotIn('"pw_buf/buf.h"', includes)
        self.assertEqual(len(includes), len(set(includes)))

    def test_unknown_message(self) -> None:
        proto_file = _file(
            services=[_service('Svc', [_method('Get', '.other.Missing')])]
        )
        with self.assertRaisesRegex(ValueError, r"'\.other\.Missing'"):
            codegen_pwpb.process_proto_file(proto_file)

    def test_package_less(self) -> None:
        header, stubs = _pwpb(
            _file(
                name='np.proto',
                package='',
                messages=[
                    _message('Req', [_message('Nested')]),
                    _message('Resp'),
                ],
                services=[
                    _service('Svc', [_method('Get', '.Req.Nested', '.Resp')])
                ],
            )
        )
        self.assertIn('namespace pw_rpc2::pwpb::Svc {', header)
        self.assertIn('using Request = ::Req::Nested::Message;', header)
        self.assertIn('using Response = ::Resp::Message;', header)
        self.assertIn('"np.pwpb.h"', _includes(header))
        self.assertIn('::pw_rpc2::pwpb::Svc::Service<SvcService>', stubs)
        self.assertIn(f'kServiceId = {hash_65599("Svc"):#010x};', header)

    def test_client(self) -> None:
        header, _ = _pwpb(_file(services=[_all_kinds()]))
        types = '::pkg::pwpb::Request::Message, ::pkg::pwpb::Response::Message'
        request = 'const ::pkg::pwpb::Request::Message& request'
        for signature in (
            f'  [[nodiscard]] ::pw::rpc2::UnaryFuture<{types}>\n'
            f'  Unary({request}) const {{',
            f'  [[nodiscard]] ::pw::rpc2::ServerStreamFuture<{types}>\n'
            f'  ServerStream({request}) const {{',
            f'  [[nodiscard]] ::pw::rpc2::ClientStreamFuture<{types}>\n'
            '  ClientStream() const {',
            f'  [[nodiscard]] ::pw::rpc2::BidiStreamFuture<{types}>\n'
            '  Bidi() const {',
        ):
            with self.subTest(signature=signature):
                self.assertIn(signature, header)

    def test_service_and_stubs(self) -> None:
        """Generated Service<Impl> and stub classes use pwpb message types."""
        header, stubs = _pwpb(_file(services=[_all_kinds()]))
        req = '::pkg::pwpb::Request::Message'
        resp = '::pkg::pwpb::Response::Message'
        for name, params in (
            ('Unary', f'{req} request, ::pw::rpc2::UnaryWriter<{resp}> writer'),
            (
                'ServerStream',
                f'{req} request, ::pw::rpc2::Writer<{resp}> writer',
            ),
            (
                'ClientStream',
                f'::pw::rpc2::Reader<{req}> reader, '
                f'::pw::rpc2::UnaryWriter<{resp}> writer',
            ),
            (
                'Bidi',
                f'::pw::rpc2::Reader<{req}> reader, '
                f'::pw::rpc2::Writer<{resp}> writer',
            ),
        ):
            with self.subTest(name=name):
                self.assertIn(f'{name}(\n      {params})', header)
                self.assertIn(f'{name}({params})', stubs)
        self.assertIn(
            '::pw::rpc2::internal::MethodInvokerFor<&T::ClientStream, '
            '::pkg::pw_rpc2::pwpb::Kinds::ClientStream>',
            header,
        )
        self.assertIn(
            '::pw::rpc2::internal::FutureMethodInvokerFor<typename '
            'T::ClientStreamFuture, '
            '::pkg::pw_rpc2::pwpb::Kinds::ClientStream>',
            header,
        )
        self.assertIn(
            'class KindsService : public ::pkg::pw_rpc2::pwpb::Kinds::'
            'Service<KindsService> {',
            stubs,
        )

    def test_raw_and_pwpb_ids_match(self) -> None:
        proto_file = _file(services=[_all_kinds()])
        raw, _ = _raw(proto_file)
        pwpb, _ = _pwpb(proto_file)
        ids = re.compile(r'k\w*Id\w* = 0x[0-9a-f]{8};')
        self.assertEqual(ids.findall(raw), ids.findall(pwpb))
        self.assertTrue(ids.findall(raw))


class CppNamespaceTest(unittest.TestCase):
    def test_cpp_namespace(self) -> None:
        self.assertEqual(
            codegen_common.cpp_namespace('a.b', 'raw'), 'a::b::raw'
        )
        self.assertEqual(codegen_common.cpp_namespace('', 'raw', 'S'), 'raw::S')
        self.assertEqual(codegen_common.cpp_namespace('a.delete'), 'a::delete_')


if __name__ == '__main__':
    unittest.main()
