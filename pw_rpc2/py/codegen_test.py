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
            'pw_rpc2_internal',
            'PwInternal',
            'PwInternal_',
            'PwInternal_Impl',
            'PwInternal_ClientCopyMethods',
            'PwInternal_Derived',
            'PwInternal_Anything',
            'kPwInternal_Methods',
            'FooPwInternalBar',
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

    def test_similar_and_base_class_names_are_allowed(self) -> None:
        """Unprefixed internal and base-class member names are allowed."""
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
                            _method('internal'),
                            _method('kServiceId'),
                            _method('ClientCopyMethods'),
                            _method('derived'),
                            _method('Impl'),
                            _method('ImplT'),
                            _method('Size'),
                            _method('kPwRpcMethods'),
                            _method('CallUnary'),
                            _method('ReserveUnary'),
                            _method('ServiceClient'),
                            _method('GeneratedServiceClient'),
                            _method('client'),
                            _method('is_open'),
                            _method('FindMethod'),
                            _method('methods'),
                            _method('service_id'),
                        ],
                    )
                ]
            )
        )
        self.assertIn(
            '  FooFuture(PwInternal_Size max_message_size) const {', header
        )
        self.assertIn(
            '  is_open(PwInternal_Size max_message_size) const {', header
        )
        self.assertIn(
            '  internal(PwInternal_Size max_message_size) const {', header
        )
        self.assertIn(
            '  derived(PwInternal_Size max_message_size) const {', header
        )
        self.assertIn(
            '  Impl(PwInternal_Size max_message_size) const {', header
        )
        self.assertIn(
            '  Size(PwInternal_Size max_message_size) const {', header
        )

    def test_copy_and_reserve_are_allowed(self) -> None:
        """`Copy` and `Reserve` are valid method names of every type."""
        for streaming in (False, True):
            proto_file = _file(
                services=[
                    _service(
                        'Svc',
                        [
                            _method('Copy', server_streaming=streaming),
                            _method('Reserve', client_streaming=streaming),
                        ],
                    )
                ]
            )
            for process in (_raw, _pwpb):
                with self.subTest(process=process, streaming=streaming):
                    header, _ = process(proto_file)
                    self.assertIn('struct Copy final {', header)
                    self.assertIn('struct Reserve final {', header)
                    self.assertIn(
                        '  using Copy = ::pw::rpc2::internal::', header
                    )

    def test_keyword_method_name(self) -> None:
        header, _ = _raw(_file(services=[_service('Svc', [_method('delete')])]))
        self.assertIn('struct delete_ final {', header)
        self.assertIn(
            'struct MethodInfo<::pkg::pw_rpc2::raw::Svc::delete_> {', header
        )
        self.assertIn(
            '  delete_(PwInternal_Size max_message_size) const {', header
        )
        # The ID is the hash of the proto name, not the C++ name.
        delete_id = f'{hash_65599("delete"):#010x}'
        self.assertIn(
            '  using delete_ = ::pw::rpc2::internal::UnaryCopyMethod<\n'
            '      ::pkg::pw_rpc2::raw::Svc::Client,\n'
            f'      {delete_id},\n'
            '      ::pw::ConstBuf>;',
            header,
        )
        self.assertIn(
            'ReserveUnary<\n'
            '        ::pw::ConstBuf>(\n'
            f'        {delete_id}, '
            '::pw::rpc2::internal::MessageSize(max_message_size));',
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


_EXPECTED_COPY_METHODS = """\
class Client;

namespace pw_rpc2_internal {{

// Provides `client.Method::Copy(buf)` for unary and server streaming methods.
class PwInternal_ClientCopyMethods
    : public ::pw::rpc2::internal::UnaryCopyMethod<
          {client},
          {unary_id},
          {resp}>,
      public ::pw::rpc2::internal::ServerStreamCopyMethod<
          {client},
          {stream_id},
          {resp}> {{
 public:
  // These alias the classes that provide `Copy()`, not the method
  // tags of the same names (e.g. `Client::Method` is not `Method`).
  using Unary = ::pw::rpc2::internal::UnaryCopyMethod<
      {client},
      {unary_id},
      {resp}>;
  using ServerStream = ::pw::rpc2::internal::ServerStreamCopyMethod<
      {client},
      {stream_id},
      {resp}>;

 protected:
  constexpr PwInternal_ClientCopyMethods() = default;
  constexpr PwInternal_ClientCopyMethods(
      const PwInternal_ClientCopyMethods&) = default;
  constexpr PwInternal_ClientCopyMethods& operator=(
      const PwInternal_ClientCopyMethods&) = default;
}};

}}  // namespace pw_rpc2_internal

class Client final : public ::pw::rpc2::internal::GeneratedServiceClient,
                     public pw_rpc2_internal::PwInternal_ClientCopyMethods {{
"""

_EXPECTED_RESERVE_METHOD = """\
  template <typename PwInternal_Size,
            typename = ::pw::rpc2::internal::EnableIfMessageSize<
                PwInternal_Size>>
  [[nodiscard]] ::pw::rpc2::{future}<
      {resp}>
  {name}(PwInternal_Size max_message_size) const {{
    return ::pw::rpc2::internal::GeneratedServiceClient::{reserve}<
        {resp}>(
        {method_id}, ::pw::rpc2::internal::MessageSize(max_message_size));
  }}
"""

_EXPECTED_RAW_METHOD_INFO = """\
template <>
struct MethodInfo<::pkg::pw_rpc2::raw::Kinds::{name}> {{
  static constexpr ::std::uint32_t kServiceId = {service_id};
  static constexpr ::std::uint32_t kMethodId = {method_id};
  static constexpr ::pw::rpc2::MethodType kType =
      ::pw::rpc2::MethodType::{kind};
  using Request = ::pw::ConstBuf;
  using Response = ::pw::ConstBuf;
}};"""

_EXPECTED_PWPB_CLIENT_METHODS = """\
  [[nodiscard]] ::pw::rpc2::UnaryFuture<
      ::pkg::pwpb::Request::Message,
      ::pkg::pwpb::Response::Message>
  Unary(
      const ::pkg::pwpb::Request::Message& request) const {{
    return ::pw::rpc2::internal::GeneratedServiceClient::CallUnary<
        ::pkg::pwpb::Request::Message,
        ::pkg::pwpb::Response::Message>(
        {unary_id}, request);
  }}

  [[nodiscard]] ::pw::rpc2::UnaryFuture<
      ::pkg::pwpb::Request::Message,
      ::pkg::pwpb::Response::Message>
  Unary(
      ::pkg::pwpb::Request::Message&& request) const {{
    return ::pw::rpc2::internal::GeneratedServiceClient::CallUnary<
        ::pkg::pwpb::Request::Message,
        ::pkg::pwpb::Response::Message>(
        {unary_id}, ::std::move(request));
  }}
""".format(
    unary_id=f'{hash_65599("Unary"):#010x}',
)

_EXPECTED_PWPB_SERVER_STREAM_METHODS = """\
  [[nodiscard]] ::pw::rpc2::ServerStreamFuture<
      ::pkg::pwpb::Request::Message,
      ::pkg::pwpb::Response::Message>
  ServerStream(
      const ::pkg::pwpb::Request::Message& request) const {{
    return ::pw::rpc2::internal::GeneratedServiceClient::CallServerStream<
        ::pkg::pwpb::Request::Message,
        ::pkg::pwpb::Response::Message>(
        {stream_id}, request);
  }}

  [[nodiscard]] ::pw::rpc2::ServerStreamFuture<
      ::pkg::pwpb::Request::Message,
      ::pkg::pwpb::Response::Message>
  ServerStream(
      ::pkg::pwpb::Request::Message&& request) const {{
    return ::pw::rpc2::internal::GeneratedServiceClient::CallServerStream<
        ::pkg::pwpb::Request::Message,
        ::pkg::pwpb::Response::Message>(
        {stream_id}, ::std::move(request));
  }}
""".format(
    stream_id=f'{hash_65599("ServerStream"):#010x}',
)

_EXPECTED_PWPB_CLIENT_STREAM_METHODS = """\
  [[nodiscard]] ::pw::rpc2::ClientStreamFuture<
      ::pkg::pwpb::Request::Message,
      ::pkg::pwpb::Response::Message>
  ClientStream() const {{
    return ::pw::rpc2::internal::GeneratedServiceClient::CallClientStream<
        ::pkg::pwpb::Request::Message,
        ::pkg::pwpb::Response::Message>({client_stream_id});
  }}

  [[nodiscard]] ::pw::rpc2::BidiStreamFuture<
      ::pkg::pwpb::Request::Message,
      ::pkg::pwpb::Response::Message>
  Bidi() const {{
    return ::pw::rpc2::internal::GeneratedServiceClient::CallBidiStream<
        ::pkg::pwpb::Request::Message,
        ::pkg::pwpb::Response::Message>({bidi_id});
  }}
""".format(
    client_stream_id=f'{hash_65599("ClientStream"):#010x}',
    bidi_id=f'{hash_65599("Bidi"):#010x}',
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

    def test_ids_are_inlined(self) -> None:
        header, _ = _raw(_file(services=[_service('Svc', [_method('Get')])]))
        service_id = f'{hash_65599("pkg.Svc"):#010x}'
        self.assertIn(
            f'::pw::rpc2::Service({service_id}, kPwInternal_Methods) {{}}',
            header,
        )
        # kServiceId is only declared in MethodInfo, not in Client or Service.
        self.assertEqual(header.count('kServiceId'), 1)
        self.assertNotIn('kMethodId_', header)

    def test_client_constructors(self) -> None:
        header, _ = _raw(_file(services=[_service('Svc', [_method('Get')])]))
        service_id = f'{hash_65599("pkg.Svc"):#010x}'
        self.assertEqual(len(re.findall(r'\bClient\(', header)), 2)
        self.assertIn('  constexpr Client() = default;\n', header)
        self.assertIn(
            '  explicit Client(const ::pw::rpc2::Client& client)\n'
            '      : ::pw::rpc2::internal::GeneratedServiceClient('
            f'client, {service_id}) {{}}',
            header,
        )

    def test_service_contract_errors(self) -> None:
        header, _ = _raw(_file(services=[_service('Svc', [_method('Get')])]))
        self.assertIn(
            '        "Service implementation must define either a member '
            'function "\n'
            '        "named \'Get\' or a future type "\n'
            '        "named \'GetFuture\'"',
            header,
        )
        self.assertIn(
            '        "Service implementation must not define both a member '
            'function "\n'
            '        "named \'Get\' and a future type "\n'
            '        "named \'GetFuture\'"',
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
            'PwInternal_Impl& PwInternal_Derived()',
            'kPwInternal_Methods =',
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
                # Aside from MethodInfo, only PwInternal_ClientCopyMethods
                # has aliases.
                self.assertEqual(
                    re.findall(
                        r'^\s*using (?!Request|Response|\w+ = '
                        r'::pw::rpc2::internal::\w+CopyMethod<)',
                        header,
                        re.M,
                    ),
                    [],
                )
                self.assertNotIn('using ::', stubs)

    def test_tags_are_empty_and_final(self) -> None:
        for process in (_raw, _pwpb):
            header, _ = process(_file(services=[_all_kinds()]))
            with self.subTest(process=process):
                for name in ('Unary', 'ServerStream', 'ClientStream', 'Bidi'):
                    self.assertIn(
                        f'struct {name} final {{\n  {name}() = delete;\n}};',
                        header,
                    )

    def test_copy_methods(self) -> None:
        """Tests generated PwInternal_ClientCopyMethods."""
        for process, flavor in ((_raw, 'raw'), (_pwpb, 'pwpb')):
            header, _ = process(_file(services=[_all_kinds()]))
            resp = (
                '::pw::ConstBuf'
                if flavor == 'raw'
                else '::pkg::pwpb::Response::Message'
            )
            with self.subTest(flavor=flavor):
                self.assertIn(
                    _EXPECTED_COPY_METHODS.format(
                        client=f'::pkg::pw_rpc2::{flavor}::Kinds::Client',
                        unary_id=f'{hash_65599("Unary"):#010x}',
                        stream_id=f'{hash_65599("ServerStream"):#010x}',
                        resp=resp,
                    ),
                    header,
                )
                self.assertNotIn('using ClientStream', header)
                self.assertNotIn('using Bidi', header)

    def test_reserve(self) -> None:
        """Tests generated client reservation methods."""
        for process, flavor in ((_raw, 'raw'), (_pwpb, 'pwpb')):
            header, _ = process(_file(services=[_all_kinds()]))
            resp = (
                '::pw::ConstBuf'
                if flavor == 'raw'
                else '::pkg::pwpb::Response::Message'
            )
            for name, future, reserve in (
                ('Unary', 'UnaryReserveFuture', 'ReserveUnary'),
                (
                    'ServerStream',
                    'ServerStreamReserveFuture',
                    'ReserveServerStream',
                ),
            ):
                with self.subTest(flavor=flavor, name=name):
                    self.assertIn(
                        _EXPECTED_RESERVE_METHOD.format(
                            name=name,
                            future=future,
                            reserve=reserve,
                            resp=resp,
                            method_id=f'{hash_65599(name):#010x}',
                        ),
                        header,
                    )
            for name in ('ClientStream', 'Bidi'):
                with self.subTest(flavor=flavor, name=name):
                    self.assertNotIn(f'{name}(PwInternal_Size', header)

    def test_no_copy_methods_without_single_request_methods(self) -> None:
        streaming = _service(
            'Svc',
            [_method('Bidi', client_streaming=True, server_streaming=True)],
        )
        for process in (_raw, _pwpb):
            header, _ = process(_file(services=[streaming]))
            with self.subTest(process=process):
                self.assertNotIn('class Client;', header)
                self.assertNotIn('ClientCopyMethods', header)
                self.assertNotIn('Reserve', header)
                self.assertIn(
                    'class Client final : public '
                    '::pw::rpc2::internal::GeneratedServiceClient {',
                    header,
                )

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
                        f'class {service}Service\n'
                        f'    : public ::pkg::pw_rpc2::{flavor}::'
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
    """Tests code generated with raw messages."""

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
                    _EXPECTED_RAW_METHOD_INFO.format(
                        name=name,
                        service_id=service_id,
                        method_id=f'{hash_65599(name):#010x}',
                        kind=kind,
                    ),
                    self.header,
                )

    def test_client(self) -> None:
        for signature in (
            '  Unary(PwInternal_Size max_message_size) const {',
            '  ServerStream(PwInternal_Size max_message_size) const {',
            '  [[nodiscard]] ::pw::rpc2::RawClientStreamFuture\n'
            '  ClientStream() const {',
            '  [[nodiscard]] ::pw::rpc2::RawBidiStreamFuture\n'
            '  Bidi() const {',
        ):
            with self.subTest(signature=signature):
                self.assertIn(signature, self.header)
        # Raw requests are sent with `client.Method::Copy(buf)`.
        client = self.header[self.header.index('class Client final') :]
        client = client[: client.index('\n};')]
        self.assertNotIn('ConstBuf request', client)
        self.assertNotIn('CallUnary', client)
        self.assertNotIn('CallServerStream', client)

    def test_service(self) -> None:
        """Tests raw Service<Impl> and stub method signatures."""
        for name, first, second in (
            (
                'Unary',
                '::pw::ConstBuf request',
                '::pw::rpc2::RawUnaryWriter writer',
            ),
            (
                'ServerStream',
                '::pw::ConstBuf request',
                '::pw::rpc2::RawWriter writer',
            ),
            (
                'ClientStream',
                '::pw::rpc2::RawReader reader',
                '::pw::rpc2::RawUnaryWriter writer',
            ),
            (
                'Bidi',
                '::pw::rpc2::RawReader reader',
                '::pw::rpc2::RawWriter writer',
            ),
        ):
            with self.subTest(name=name):
                self.assertIn(
                    f'{name}(\n      {first},\n      {second})', self.header
                )
                self.assertIn(
                    f'  //   SomeFuture {name}(\n'
                    f'  //       {first},\n'
                    f'  //       {second});',
                    self.stubs,
                )
        self.assertIn(
            '      : ::pw::rpc2::internal::MethodInvokerFor<\n'
            '            &T::Bidi,\n'
            '            ::pkg::pw_rpc2::raw::Kinds::Bidi> {};',
            self.header,
        )
        self.assertIn(
            '      : ::pw::rpc2::internal::FutureMethodInvokerFor<\n'
            '            typename T::BidiFuture,\n'
            '            ::pkg::pw_rpc2::raw::Kinds::Bidi> {};',
            self.header,
        )

    def test_stubs(self) -> None:
        self.assertIn(
            'class KindsService\n'
            '    : public ::pkg::pw_rpc2::raw::Kinds::'
            'Service<KindsService> {',
            self.stubs,
        )
        self.assertIn(
            '    BidiFuture(\n'
            '        KindsService& service,\n'
            '        ::pw::rpc2::RawReader reader,\n'
            '        ::pw::rpc2::RawWriter writer);',
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
        """Tests RPC methods that share a name with a message."""
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
        self.assertIn('struct Ping final {', header)
        self.assertIn(
            '  [[nodiscard]] ::pw::rpc2::UnaryReserveFuture<\n'
            '      ::pkg::pwpb::Pong::Message>\n'
            '  Ping(PwInternal_Size max_message_size) const {',
            header,
        )
        self.assertIn(
            '  using Ping = ::pw::rpc2::internal::UnaryCopyMethod<\n'
            '      ::pkg::pw_rpc2::pwpb::Svc::Client,\n'
            f'      {hash_65599("Ping"):#010x},\n'
            '      ::pkg::pwpb::Pong::Message>;',
            header,
        )
        self.assertIn(
            'struct MethodInfo<::pkg::pw_rpc2::pwpb::Svc::Ping> {',
            header,
        )
        self.assertIn('using Request = ::pkg::pwpb::Ping::Message;', header)
        self.assertIn('using Response = ::pkg::pwpb::Pong::Message;', header)
        self.assertIn(
            '  [[nodiscard]] ::pw::rpc2::UnaryFuture<\n'
            '      ::pkg::pwpb::Ping::Message,\n'
            '      ::pkg::pwpb::Pong::Message>\n'
            '  Pong(\n'
            '      const ::pkg::pwpb::Ping::Message& request) const {',
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
        """Tests pwpb Client method signatures."""
        header, _ = _pwpb(_file(services=[_all_kinds()]))
        self.assertIn(_EXPECTED_PWPB_CLIENT_METHODS, header)
        self.assertIn(_EXPECTED_PWPB_SERVER_STREAM_METHODS, header)
        self.assertIn(_EXPECTED_PWPB_CLIENT_STREAM_METHODS, header)

    def test_service_and_stubs(self) -> None:
        """Generated Service<Impl> and stub classes use pwpb message types."""
        header, stubs = _pwpb(_file(services=[_all_kinds()]))
        req = '::pkg::pwpb::Request::Message'
        resp = '::pkg::pwpb::Response::Message'
        for name, first, second in (
            (
                'Unary',
                f'{req} request',
                f'::pw::rpc2::UnaryWriter<{resp}> writer',
            ),
            (
                'ServerStream',
                f'{req} request',
                f'::pw::rpc2::Writer<{resp}> writer',
            ),
            (
                'ClientStream',
                f'::pw::rpc2::Reader<{req}> reader',
                f'::pw::rpc2::UnaryWriter<{resp}> writer',
            ),
            (
                'Bidi',
                f'::pw::rpc2::Reader<{req}> reader',
                f'::pw::rpc2::Writer<{resp}> writer',
            ),
        ):
            with self.subTest(name=name):
                self.assertIn(
                    f'{name}(\n      {first},\n      {second})', header
                )
                self.assertIn(
                    f'  //   SomeFuture {name}(\n'
                    f'  //       {first},\n'
                    f'  //       {second});',
                    stubs,
                )
        self.assertIn(
            '      : ::pw::rpc2::internal::MethodInvokerFor<\n'
            '            &T::ClientStream,\n'
            '            ::pkg::pw_rpc2::pwpb::Kinds::ClientStream> {};',
            header,
        )
        self.assertIn(
            '      : ::pw::rpc2::internal::FutureMethodInvokerFor<\n'
            '            typename T::ClientStreamFuture,\n'
            '            ::pkg::pw_rpc2::pwpb::Kinds::ClientStream> {};',
            header,
        )
        self.assertIn(
            'class KindsService\n'
            '    : public ::pkg::pw_rpc2::pwpb::Kinds::'
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
