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
"""Shared protoc plugin entrypoint for the pw_rpc2 code generators."""

import sys
from typing import Callable, Sequence

from google.protobuf import descriptor_pb2
from google.protobuf.compiler import plugin_pb2

from pw_protobuf.output_file import OutputFile

ProcessProtoFile = Callable[
    [
        descriptor_pb2.FileDescriptorProto,
        Sequence[descriptor_pb2.FileDescriptorProto],
    ],
    list[OutputFile],
]


def process_request(
    process_proto_file: ProcessProtoFile,
    request: plugin_pb2.CodeGeneratorRequest,
) -> plugin_pb2.CodeGeneratorResponse:
    """Generates code for the files protoc requested.

    If a file is invalid, the response contains only an error message, which
    protoc reports.
    """
    response = plugin_pb2.CodeGeneratorResponse()

    # No message code is generated, so proto3 optional fields are supported
    # trivially.
    response.supported_features = (
        plugin_pb2.CodeGeneratorResponse.FEATURE_PROTO3_OPTIONAL
    )

    files_to_generate = set(request.file_to_generate)

    for proto_file in request.proto_file:
        if proto_file.name not in files_to_generate:
            continue

        try:
            output_files = process_proto_file(proto_file, request.proto_file)
        except ValueError as err:
            return plugin_pb2.CodeGeneratorResponse(
                error=f'{proto_file.name}: {err}'
            )

        for output_file in output_files:
            response.file.add(
                name=output_file.name(), content=output_file.content()
            )

    return response


def main(process_proto_file: ProcessProtoFile) -> int:
    """Runs a protoc plugin, which reads a CodeGeneratorRequest from stdin and
    writes a CodeGeneratorResponse to stdout."""
    request = plugin_pb2.CodeGeneratorRequest.FromString(
        sys.stdin.buffer.read()
    )
    response = process_request(process_proto_file, request)
    sys.stdout.buffer.write(response.SerializeToString())
    return 0
