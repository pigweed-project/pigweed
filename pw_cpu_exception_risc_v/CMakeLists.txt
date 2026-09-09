# Copyright 2024 The Pigweed Authors
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

pw_add_module_config(pw_cpu_exception_risc_v_CONFIG)

pw_add_library(pw_cpu_exception_risc_v.config INTERFACE
  HEADERS
    pw_cpu_exception_risc_v_private/config.h
  PUBLIC_INCLUDES
    .
  PUBLIC_DEPS
    ${pw_cpu_exception_risc_v_CONFIG}
)

pw_add_library(pw_cpu_exception_risc_v.cpu_state INTERFACE
  HEADERS
    public/pw_cpu_exception_risc_v/cpu_state.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_preprocessor
    pw_preprocessor.arch
)

pw_add_library(pw_cpu_exception_risc_v.support STATIC
  PRIVATE_DEPS
    pw_cpu_exception_risc_v.config
    pw_cpu_exception_risc_v.util
    pw_log
    pw_preprocessor.arch
    pw_span
    pw_string
  SOURCES
    support.cc
)

pw_proto_library(pw_cpu_exception_risc_v.cpu_state_protos
  SOURCES
    pw_cpu_exception_risc_v_protos/cpu_state.proto
)

pw_add_library(pw_cpu_exception_risc_v.proto_dump STATIC
  HEADERS
    public/pw_cpu_exception_risc_v/proto_dump.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_cpu_exception_risc_v.cpu_state
    pw_protobuf
    pw_status
    pw_stream
  PRIVATE_DEPS
    pw_cpu_exception_risc_v.config
    pw_cpu_exception_risc_v.cpu_state_protos.pwpb
  SOURCES
    proto_dump.cc
)

pw_add_library(pw_cpu_exception_risc_v.snapshot STATIC
  HEADERS
    public/pw_cpu_exception_risc_v/snapshot.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_cpu_exception_risc_v.cpu_state
    pw_cpu_exception_risc_v.cpu_state_protos.pwpb
    pw_protobuf
    pw_status
    pw_thread.protos.pwpb
    pw_thread.snapshot
  PRIVATE_DEPS
    pw_cpu_exception_risc_v.config
    pw_cpu_exception_risc_v.proto_dump
    pw_cpu_exception_risc_v.util
    pw_log
    pw_span
  SOURCES
    snapshot.cc
)
