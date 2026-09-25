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

include($ENV{PW_ROOT}/pw_build/pigweed.cmake)
include($ENV{PW_ROOT}/pw_enum/pw_cc_enum_library.cmake)

pw_add_enum_library(pw_rpc2.cpp.protocol_status
  ENUM_HEADERS
    public/pw_rpc2/internal/protocol_status.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_status
)

pw_add_library(pw_rpc2.cpp.packet STATIC
  HEADERS
    public/pw_rpc2/internal/packet.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_assert.assert
    pw_assert.check
    pw_buf
    pw_bytes
    pw_enum
    pw_preprocessor
    pw_result
    pw_rpc2.cpp.protocol_status
    pw_status
  SOURCES
    packet.cc
)

pw_add_test(pw_rpc2.cpp.packet_test
  SOURCES
    packet_test.cc
  PRIVATE_DEPS
    pw_allocator.testing
    pw_assert.check
    pw_bytes
    pw_enum
    pw_rpc2.cpp.packet
    pw_rpc2.cpp.protocol_status
    pw_status
  GROUPS
    modules
    pw_rpc2
)

pw_add_library(pw_rpc2.cpp.serialize INTERFACE
  HEADERS
    public/pw_rpc2/serialize.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_allocator
    pw_assert.check
    pw_buf
    pw_bytes
    pw_result
    pw_status
)

pw_add_library(pw_rpc2.cpp.pwpb_serialize INTERFACE
  HEADERS
    public/pw_rpc2/pwpb_serialize.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_buf
    pw_bytes
    pw_containers.vector
    pw_protobuf
    pw_result
    pw_rpc2.cpp.serialize
    pw_span
    pw_status
    pw_stream
    pw_string.string
    pw_varint
)

pw_add_test(pw_rpc2.cpp.pwpb_serialize_test
  SOURCES
    pwpb_serialize_test.cc
  PRIVATE_DEPS
    pw_buf
    pw_protobuf
    pw_rpc2.cpp.pwpb_serialize
    pw_span
    pw_status
  GROUPS
    modules
    pw_rpc2
)
