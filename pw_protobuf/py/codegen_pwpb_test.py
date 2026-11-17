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
"""Tests for pw_protobuf C++ header code generation."""

import unittest

from google.protobuf import descriptor_pb2

from pw_protobuf import codegen_pwpb


class CodegenPwpbTest(unittest.TestCase):
    """Tests generated .pwpb.h header contents."""

    def test_exports_internal_codegen_header(self) -> None:
        """Verifies .pwpb.h headers export pw_protobuf/internal/codegen.h."""
        field_proto = descriptor_pb2.FieldDescriptorProto
        proto_file = descriptor_pb2.FileDescriptorProto(
            name='pkg/test.proto',
            package='pkg',
            syntax='proto3',
            message_type=[
                descriptor_pb2.DescriptorProto(
                    name='TestMessage',
                    field=[
                        field_proto(
                            name='value',
                            number=1,
                            label=field_proto.LABEL_OPTIONAL,
                            type=field_proto.TYPE_UINT32,
                        ),
                    ],
                ),
            ],
        )
        options = codegen_pwpb.GeneratorOptions(
            oneof_callbacks=True,
            exclude_legacy_snake_case_field_name_enums=True,
            suppress_legacy_namespace=False,
        )
        output_files = codegen_pwpb.process_proto_file(proto_file, [], options)
        if output_files is None:
            self.fail('Expected process_proto_file to return output files')
        files = list(output_files)
        self.assertEqual(len(files), 1)
        self.assertEqual(files[0].name(), 'pkg/test.pwpb.h')
        self.assertIn(
            '#include "pw_protobuf/internal/codegen.h"  // IWYU pragma: export',
            files[0].content().splitlines(),
        )


if __name__ == '__main__':
    unittest.main()
