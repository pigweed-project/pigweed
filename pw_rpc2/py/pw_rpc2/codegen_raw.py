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
"""Generates raw (pw::ConstBuf message) C++ code for pw_rpc2 services."""

from typing import Iterable, Sequence

from google.protobuf import descriptor_pb2

from pw_protobuf.output_file import OutputFile

from pw_rpc2.codegen_common import (
    CLIENT_BASE,
    CodeGenerator,
    Method,
    MethodType,
    Param,
    ServerParams,
    Service,
    generate_headers,
    hex_id,
    write_reserve_method,
)

_CONST_BUF = '::pw::ConstBuf'

_SERVER_PARAMS = {
    MethodType.UNARY: ServerParams(
        Param(_CONST_BUF, 'request'),
        Param('::pw::rpc2::RawUnaryWriter', 'writer'),
    ),
    MethodType.SERVER_STREAMING: ServerParams(
        Param(_CONST_BUF, 'request'),
        Param('::pw::rpc2::RawWriter', 'writer'),
    ),
    MethodType.CLIENT_STREAMING: ServerParams(
        Param('::pw::rpc2::RawReader', 'reader'),
        Param('::pw::rpc2::RawUnaryWriter', 'writer'),
    ),
    MethodType.BIDIRECTIONAL_STREAMING: ServerParams(
        Param('::pw::rpc2::RawReader', 'reader'),
        Param('::pw::rpc2::RawWriter', 'writer'),
    ),
}


class RawCodeGenerator(CodeGenerator):
    """Generates code for services with raw pw::ConstBuf messages."""

    def __init__(self) -> None:
        super().__init__('raw')

    def includes(self, services: Sequence[Service]) -> Iterable[str]:
        del services
        return ('<cstddef>', '"pw_buf/buf.h"')

    def request_type(self, method: Method) -> str:
        return _CONST_BUF

    def response_type(self, method: Method) -> str:
        return _CONST_BUF

    def server_params(self, method: Method) -> ServerParams:
        return _SERVER_PARAMS[method.type]

    def write_client_method(self, output: OutputFile, method: Method) -> None:
        # Unary and server-streaming requests are written in place with
        # `client.Method(max_message_size)` or copied from a `ConstBuf` with
        # `client.Method::Copy()`, which `ClientCopyMethods` provides.
        if method.has_single_request:
            write_reserve_method(output, self, method)
            return

        name = method.cpp_name
        method_id = hex_id(method.id)
        types = f'{_CONST_BUF}, {_CONST_BUF}'

        if method.type is MethodType.CLIENT_STREAMING:
            future = 'RawClientStreamFuture'
            call = 'CallClientStream'
        else:
            future = 'RawBidiStreamFuture'
            call = 'CallBidiStream'

        output.write_line(f'[[nodiscard]] ::pw::rpc2::{future}')
        output.write_line(f'{name}() const {{')
        with output.indent():
            output.write_line(f'return {CLIENT_BASE}::{call}<')
            output.write_line(f'    {types}>({method_id});')
        output.write_line('}')


def process_proto_file(
    proto_file: descriptor_pb2.FileDescriptorProto,
    all_files: Sequence[descriptor_pb2.FileDescriptorProto] = (),
) -> list[OutputFile]:
    """Generates the raw pw_rpc2 headers for a .proto file.

    ``all_files`` is unused, since raw code does not refer to message types. It
    is accepted so that both plugins share an interface.
    """
    del all_files  # Unused
    return generate_headers(proto_file, RawCodeGenerator())
