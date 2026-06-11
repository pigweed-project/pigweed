# Copyright 2021 The Pigweed Authors
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

include($ENV{PW_ROOT}/pw_build/pigweed.cmake)
include($ENV{PW_ROOT}/pw_protobuf_compiler/proto.cmake)

pw_proto_library(pw_protobuf_compiler.pwpb_test_protos
  SOURCES
    pwpb_test_protos/pwpb_test.proto
  INPUTS
    pwpb_test_protos/pwpb_test.pwpb_options
  PREFIX
    "pw_protobuf_compiler/"
  DEPS
    pw_protobuf.field_options_proto
)

pw_add_test(pw_protobuf_compiler.pwpb_test
  SOURCES
    pwpb_test.cc
  PRIVATE_DEPS
    pw_protobuf_compiler.pwpb_test_protos.pwpb
  GROUPS
    pw_protobuf_compiler
)

pw_proto_library(pw_protobuf_compiler.pwpb_test_no_prefix_protos
  SOURCES
    pwpb_no_prefix_test_protos/pwpb_test_no_prefix.proto
  STRIP_PREFIX
    "pwpb_no_prefix_test_protos/"
  DEPS
    pw_protobuf.field_options_proto
)

pw_add_test(pw_protobuf_compiler.pwpb_no_prefix_test
  SOURCES
    pwpb_test_no_prefix.cc
  PRIVATE_DEPS
    pw_protobuf_compiler.pwpb_test_no_prefix_protos.pwpb
  GROUPS
    pw_protobuf_compiler
)

if(NOT "${dir_pw_third_party_nanopb}" STREQUAL "")
  pw_proto_library(pw_protobuf_compiler.nanopb_test_protos
    SOURCES
      nanopb_test_protos/nanopb_test.proto
    PREFIX
      "pw_protobuf_compiler/"
    DEPS
      pw_third_party.nanopb.proto
  )

  pw_add_test(pw_protobuf_compiler.nanopb_test
    SOURCES
      nanopb_test.cc
    PRIVATE_DEPS
      pw_protobuf_compiler.nanopb_test_protos.nanopb
    GROUPS
      pw_protobuf_compiler
  )
endif()

