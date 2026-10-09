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
    write_reserve_method,
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
        first = (
            Param(req, 'request')
            if method.has_single_request
            else Param(f'::pw::rpc2::Reader<{req}>', 'reader')
        )
        writer = (
            'UnaryWriter'
            if method.type in (MethodType.UNARY, MethodType.CLIENT_STREAMING)
            else 'Writer'
        )
        return ServerParams(
            first, Param(f'::pw::rpc2::{writer}<{resp}>', 'writer')
        )

    def write_client_method(self, output: OutputFile, method: Method) -> None:
        name = method.cpp_name
        req, resp = self._types(method)
        method_id = hex_id(method.id)

        def write_future(future: str) -> None:
            output.write_line(f'[[nodiscard]] ::pw::rpc2::{future}<')
            with output.indent(4):
                output.write_line(f'{req},')
                output.write_line(f'{resp}>')

        if method.has_single_request:
            if method.type is MethodType.UNARY:
                future = 'UnaryFuture'
                call = 'CallUnary'
            else:
                future = 'ServerStreamFuture'
                call = 'CallServerStream'

            # The request is copied or moved into the future, which serializes
            # it when polled.
            for param, arg in (
                (f'const {req}& request', 'request'),
                (f'{req}&& request', '::std::move(request)'),
            ):
                write_future(future)
                output.write_line(f'{name}(')
                output.write_line(f'    {param}) const {{')
                with output.indent():
                    output.write_line(f'return {CLIENT_BASE}::{call}<')
                    with output.indent(4):
                        output.write_line(f'{req},')
                        output.write_line(f'{resp}>(')
                        output.write_line(f'{method_id}, {arg});')
                output.write_line('}')
                output.write_line()

            write_reserve_method(output, self, method)
            return

        if method.type is MethodType.CLIENT_STREAMING:
            future = 'ClientStreamFuture'
            call = 'CallClientStream'
        else:
            future = 'BidiStreamFuture'
            call = 'CallBidiStream'

        write_future(future)
        output.write_line(f'{name}() const {{')
        with output.indent():
            output.write_line(f'return {CLIENT_BASE}::{call}<')
            with output.indent(4):
                output.write_line(f'{req},')
                output.write_line(f'{resp}>({method_id});')
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
