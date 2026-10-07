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
"""Tests for the pw_rpc2 protoc plugin entrypoint."""

from typing import Sequence
import unittest

from google.protobuf import descriptor_pb2
from google.protobuf.compiler import plugin_pb2

from pw_rpc2 import codegen_pwpb, codegen_raw, plugin


def _file(
    name: str,
    package: str,
    messages: Sequence[str] = (),
    methods: Sequence[tuple[str, str, str]] = (),
    dependencies: Sequence[str] = (),
) -> descriptor_pb2.FileDescriptorProto:
    """Creates a file with the given messages and a service `Svc`."""
    proto_file = descriptor_pb2.FileDescriptorProto(
        name=name, package=package, dependency=dependencies
    )
    for message in messages:
        proto_file.message_type.add(name=message)
    if methods:
        service = proto_file.service.add(name='Svc')
        for method, request, response in methods:
            service.method.add(
                name=method, input_type=request, output_type=response
            )
    return proto_file


def _request(
    files: Sequence[descriptor_pb2.FileDescriptorProto],
    files_to_generate: Sequence[str],
) -> plugin_pb2.CodeGeneratorRequest:
    return plugin_pb2.CodeGeneratorRequest(
        proto_file=files, file_to_generate=files_to_generate
    )


_COMMON = _file('common.proto', 'common', messages=['Shared'])
_SERVICE = _file(
    'svc.proto',
    'svc',
    methods=[('Get', '.common.Shared', '.common.Shared')],
    dependencies=['common.proto'],
)


class ProcessRequestTest(unittest.TestCase):
    """Tests plugin.process_request."""

    def test_generates_only_requested_files(self) -> None:
        response = plugin.process_request(
            codegen_raw.process_proto_file,
            _request([_COMMON, _SERVICE], ['svc.proto']),
        )
        self.assertFalse(response.HasField('error'))
        self.assertEqual(
            [file.name for file in response.file],
            ['svc.raw.rpc2.h', 'svc.raw_stubs.rpc2.h'],
        )
        self.assertIn(
            'namespace svc::pw_rpc2::raw::Svc {', response.file[0].content
        )

    def test_generates_every_requested_file(self) -> None:
        response = plugin.process_request(
            codegen_raw.process_proto_file,
            _request([_COMMON, _SERVICE], ['common.proto', 'svc.proto']),
        )
        self.assertEqual(
            [file.name for file in response.file],
            [
                'common.raw.rpc2.h',
                'common.raw_stubs.rpc2.h',
                'svc.raw.rpc2.h',
                'svc.raw_stubs.rpc2.h',
            ],
        )

    def test_passes_imports_to_codegen(self) -> None:
        response = plugin.process_request(
            codegen_pwpb.process_proto_file,
            _request([_COMMON, _SERVICE], ['svc.proto']),
        )
        self.assertFalse(response.HasField('error'))
        self.assertIn(
            'using Request = ::common::pwpb::Shared::Message;',
            response.file[0].content,
        )

    def test_supports_proto3_optional(self) -> None:
        response = plugin.process_request(
            codegen_raw.process_proto_file, _request([_SERVICE], ['svc.proto'])
        )
        self.assertEqual(
            response.supported_features,
            plugin_pb2.CodeGeneratorResponse.FEATURE_PROTO3_OPTIONAL,
        )

    def test_reports_invalid_file(self) -> None:
        invalid = _file(
            'bad.proto',
            'bad',
            messages=['Msg'],
            methods=[('Client', '.bad.Msg', '.bad.Msg')],
        )
        for process in (
            codegen_raw.process_proto_file,
            codegen_pwpb.process_proto_file,
        ):
            with self.subTest(process=process):
                response = plugin.process_request(
                    process,
                    _request(
                        [_COMMON, _SERVICE, invalid], ['svc.proto', 'bad.proto']
                    ),
                )
                self.assertTrue(response.error.startswith('bad.proto: '))
                self.assertIn("'bad.Svc.Client'", response.error)
                # Nothing is generated if any file is invalid.
                self.assertEqual(len(response.file), 0)

    def test_reports_unknown_message(self) -> None:
        missing = _file(
            'missing.proto', 'pkg', methods=[('Get', '.pkg.Nope', '.pkg.Nope')]
        )
        response = plugin.process_request(
            codegen_pwpb.process_proto_file,
            _request([missing], ['missing.proto']),
        )
        self.assertTrue(response.error.startswith('missing.proto: '))
        self.assertIn("'.pkg.Nope'", response.error)


if __name__ == '__main__':
    unittest.main()
