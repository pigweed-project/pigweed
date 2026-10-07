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
"""Generates C++ code with pw_protobuf message types for pw_rpc2 services."""

import dataclasses
import os
from typing import Iterable, Sequence

from google.protobuf import descriptor_pb2

from pw_protobuf.output_file import OutputFile
from pw_protobuf.symbol_name_mapping import fix_cc_identifier

from pw_rpc2.codegen_common import (
    CLIENT_BASE,
    CodeGenerator,
    Method,
    MethodType,
    Param,
    ServerParams,
    Service,
    cpp_namespace,
    generate_headers,
    hex_id,
)


@dataclasses.dataclass(frozen=True)
class _PwpbMessage:
    namespace: str  # The pwpb namespace of the message, without leading ::.
    proto_file: str  # The .proto file that defines the message.

    @property
    def struct(self) -> str:
        return f'::{self.namespace}::Message'

    @property
    def header(self) -> str:
        return f'"{os.path.splitext(self.proto_file)[0]}.pwpb.h"'


def _pwpb_messages(
    files: Iterable[descriptor_pb2.FileDescriptorProto],
) -> dict[str, _PwpbMessage]:
    """Maps fully qualified proto message names to their pwpb C++ names.

    The pwpb codegen places a message `pkg.Msg.Nested` in the namespace
    `pkg::pwpb::Msg::Nested`, or `Msg::Nested` if the file has no package.
    """
    messages: dict[str, _PwpbMessage] = {}

    def add(
        proto_file: str,
        prefix: str,
        namespace: str,
        message_types: Iterable[descriptor_pb2.DescriptorProto],
    ) -> None:
        for message in message_types:
            name = f'{prefix}.{message.name}'
            cpp_name = fix_cc_identifier(message.name)
            nested = f'{namespace}::{cpp_name}' if namespace else cpp_name
            messages[name] = _PwpbMessage(nested, proto_file)
            add(proto_file, name, nested, message.nested_type)

    for file in files:
        package = f'.{file.package}' if file.package else ''
        namespace = cpp_namespace(file.package, 'pwpb') if file.package else ''
        add(file.name, package, namespace, file.message_type)

    return messages


class PwpbCodeGenerator(CodeGenerator):
    """Generates code for services with pw_protobuf message structs."""

    def __init__(
        self,
        proto_file: descriptor_pb2.FileDescriptorProto,
        all_files: Sequence[descriptor_pb2.FileDescriptorProto],
    ) -> None:
        super().__init__('pwpb')
        self._messages = _pwpb_messages([proto_file, *all_files])

    def _message(self, proto_name: str) -> _PwpbMessage:
        try:
            return self._messages[proto_name]
        except KeyError:
            raise ValueError(
                f"Message type '{proto_name}' was not found in the .proto "
                'file or its imports.'
            ) from None

    def _used_messages(self, services: Sequence[Service]) -> list[_PwpbMessage]:
        used = {
            self._message(proto_name)
            for service in services
            for method in service.methods
            for proto_name in (method.request_type, method.response_type)
        }
        return sorted(used, key=lambda message: message.namespace)

    def includes(self, services: Sequence[Service]) -> Iterable[str]:
        yield '"pw_rpc2/internal/pwpb_serialize.h"'
        for message in self._used_messages(services):
            yield message.header

    def request_type(self, method: Method) -> str:
        return self._message(method.request_type).struct

    def response_type(self, method: Method) -> str:
        return self._message(method.response_type).struct

    def _types(self, method: Method) -> tuple[str, str]:
        return self.request_type(method), self.response_type(method)

    def server_params(self, method: Method) -> ServerParams:
        req, resp = self._types(method)
        if method.type is MethodType.UNARY:
            return ServerParams(
                Param(req, 'request'),
                Param(f'::pw::rpc2::UnaryWriter<{resp}>', 'writer'),
            )
        if method.type is MethodType.SERVER_STREAMING:
            return ServerParams(
                Param(req, 'request'),
                Param(f'::pw::rpc2::Writer<{resp}>', 'writer'),
            )
        if method.type is MethodType.CLIENT_STREAMING:
            return ServerParams(
                Param(f'::pw::rpc2::Reader<{req}>', 'reader'),
                Param(f'::pw::rpc2::UnaryWriter<{resp}>', 'writer'),
            )
        return ServerParams(
            Param(f'::pw::rpc2::Reader<{req}>', 'reader'),
            Param(f'::pw::rpc2::Writer<{resp}>', 'writer'),
        )

    def write_client_method(self, output: OutputFile, method: Method) -> None:
        name = method.cpp_name
        req, resp = self._types(method)
        types = f'{req}, {resp}'
        method_id = hex_id(method.id)

        if method.type is MethodType.UNARY:
            future = 'UnaryFuture'
            params = f'const {req}& request'
            call = f'CallUnary<{types}>({method_id}, request)'
        elif method.type is MethodType.SERVER_STREAMING:
            future = 'ServerStreamFuture'
            params = f'const {req}& request'
            call = f'CallServerStream<{types}>({method_id}, request)'
        elif method.type is MethodType.CLIENT_STREAMING:
            future = 'ClientStreamFuture'
            params = ''
            call = f'CallClientStream<{types}>({method_id})'
        else:
            future = 'BidiStreamFuture'
            params = ''
            call = f'CallBidiStream<{types}>({method_id})'

        output.write_line(f'[[nodiscard]] ::pw::rpc2::{future}<{types}>')
        output.write_line(f'{name}({params}) const {{')
        with output.indent():
            output.write_line(f'return {CLIENT_BASE}::{call};')
        output.write_line('}')


def process_proto_file(
    proto_file: descriptor_pb2.FileDescriptorProto,
    all_files: Sequence[descriptor_pb2.FileDescriptorProto] = (),
) -> list[OutputFile]:
    """Generates the pwpb pw_rpc2 headers for a .proto file.

    Args:
      proto_file: The file to generate code for.
      all_files: The files it imports, directly or indirectly, which define
          the messages it uses. protoc passes these to the plugin.
    """
    return generate_headers(
        proto_file, PwpbCodeGenerator(proto_file, all_files)
    )
