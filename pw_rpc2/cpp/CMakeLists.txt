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

pw_add_enum_library(pw_rpc2.cpp._protocol_status
  ENUM_HEADERS
    public/pw_rpc2/internal/protocol_status.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_status
)

pw_add_library(pw_rpc2.cpp._packet STATIC
  HEADERS
    public/pw_rpc2/internal/packet.h
    public/pw_rpc2/internal/packet_type.h
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
    pw_rpc2.cpp._protocol_status
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
    pw_rpc2.cpp._packet
    pw_rpc2.cpp._protocol_status
    pw_status
  GROUPS
    modules
    pw_rpc2
)

pw_add_library(pw_rpc2.cpp._serialize INTERFACE
  HEADERS
    public/pw_rpc2/internal/serialize.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_buf
    pw_bytes
    pw_result
    pw_status
)

pw_add_test(pw_rpc2.cpp.serialize_test
  SOURCES
    serialize_test.cc
  PRIVATE_DEPS
    pw_buf
    pw_bytes
    pw_result
    pw_rpc2.cpp._serialize
    pw_status
  GROUPS
    modules
    pw_rpc2
)

pw_add_library(pw_rpc2.cpp._pwpb_serialize STATIC
  HEADERS
    public/pw_rpc2/internal/pwpb_serialize.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_bytes
    pw_protobuf
    pw_result
    pw_rpc2.cpp._serialize
    pw_span
    pw_status
  PRIVATE_DEPS
    pw_stream
  SOURCES
    pwpb_serialize.cc
)

pw_add_test(pw_rpc2.cpp.pwpb_serialize_test
  SOURCES
    pwpb_serialize_test.cc
  PRIVATE_DEPS
    pw_buf
    pw_compilation_testing._pigweed_only_negative_compilation
    pw_protobuf
    pw_rpc2.cpp._pwpb_serialize
    pw_span
    pw_status
  GROUPS
    modules
    pw_rpc2
)

pw_add_library(pw_rpc2.cpp._method INTERFACE
  HEADERS
    public/pw_rpc2/internal/method_info.h
    public/pw_rpc2/method_type.h
  PUBLIC_INCLUDES
    public
)

pw_add_library(pw_rpc2.cpp._handshake STATIC
  HEADERS
    public/pw_rpc2/internal/handshake.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_assert.check
    pw_async2
    pw_buf
    pw_bytes
    pw_result
    pw_rpc2.cpp._packet
    pw_status
    pw_transport.cpp
  PRIVATE_DEPS
    pw_preprocessor
  SOURCES
    handshake.cc
)

pw_add_library(pw_rpc2.cpp._connection STATIC
  HEADERS
    public/pw_rpc2/internal/call.h
    public/pw_rpc2/internal/call_handle.h
    public/pw_rpc2/internal/client_connection_task.h
    public/pw_rpc2/internal/connection_task.h
    public/pw_rpc2/internal/control_future.h
    public/pw_rpc2/internal/future_base.h
    public/pw_rpc2/reader.h
    public/pw_rpc2/write_reservation.h
    public/pw_rpc2/writer.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_allocator
    pw_assert.assert
    pw_assert.check
    pw_async2
    pw_async2.channel
    pw_async2.future_or_value
    pw_async2.value_future
    pw_buf
    pw_bytes
    pw_containers.algorithm
    pw_containers.dynamic_deque
    pw_containers.intrusive_forward_list
    pw_intrusive_ptr
    pw_log
    pw_polyfill
    pw_result
    pw_rpc2.cpp._handshake
    pw_rpc2.cpp._method
    pw_rpc2.cpp._packet
    pw_rpc2.cpp._protocol_status
    pw_rpc2.cpp._serialize
    pw_status
    pw_sync.interrupt_spin_lock
    pw_sync.lock_annotations
    pw_transport.cpp
  SOURCES
    call.cc
    client_connection_task.cc
    connection_task.cc
    write_reservation.cc
)

set(_pw_rpc2_coro_deps)
if("cxx_std_20" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
  set(_pw_rpc2_coro_deps pw_async2.coro)
endif()

pw_add_library(pw_rpc2.cpp._server STATIC
  HEADERS
    public/pw_rpc2/internal/method.h
    public/pw_rpc2/internal/method_future.h
    public/pw_rpc2/internal/method_invoker.h
    public/pw_rpc2/internal/method_traits.h
    public/pw_rpc2/internal/server_call.h
    public/pw_rpc2/internal/server_connection_task.h
    public/pw_rpc2/internal/server_task.h
    public/pw_rpc2/server.h
    public/pw_rpc2/service.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_async2.box
    ${_pw_rpc2_coro_deps}
    pw_bytes.alignment
    pw_containers.forward_list
    pw_function
    pw_rpc2.cpp._connection
    pw_span
    pw_third_party.fuchsia.stdcompat
  PRIVATE_DEPS
    pw_preprocessor
  SOURCES
    server_call.cc
    server_connection_task.cc
    server_task.cc
    service.cc
)

pw_add_library(pw_rpc2.cpp INTERFACE
  PUBLIC_DEPS
    pw_rpc2.cpp._server
)

pw_add_library(pw_rpc2.cpp._test_utils STATIC
  HEADERS
    public/pw_rpc2/internal/packet_testing.h
    public/pw_rpc2/internal/test_utils.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_allocator
    pw_assert.check
    pw_async2
    pw_async2.value_future
    pw_buf
    pw_bytes
    pw_containers.inline_deque
    pw_containers.vector
    pw_log
    pw_result
    pw_rpc2.cpp._connection
    pw_rpc2.cpp._handshake
    pw_rpc2.cpp._method
    pw_rpc2.cpp._packet
    pw_rpc2.cpp._protocol_status
    pw_rpc2.cpp._serialize
    pw_status
    pw_sync.interrupt_spin_lock
    pw_transport.cpp
    pw_unit_test
  SOURCES
    test_utils.cc
)

pw_add_test(pw_rpc2.cpp.handshake_test
  SOURCES
    handshake_test.cc
  PRIVATE_DEPS
    pw_allocator.testing
    pw_async2
    pw_async2.testing
    pw_bytes
    pw_rpc2.cpp._handshake
    pw_rpc2.cpp._packet
    pw_rpc2.cpp._test_utils
    pw_status
    pw_transport.cpp
  GROUPS
    modules
    pw_rpc2
)

pw_add_test(pw_rpc2.cpp.call_test
  SOURCES
    call_test.cc
  PRIVATE_DEPS
    pw_allocator.testing
    pw_async2.testing
    pw_rpc2.cpp._connection
    pw_rpc2.cpp._test_utils
    pw_transport.cpp
  GROUPS
    modules
    pw_rpc2
)

pw_add_test(pw_rpc2.cpp.rpc_reader_writer_test
  SOURCES
    rpc_reader_writer_test.cc
  PRIVATE_DEPS
    pw_allocator.testing
    pw_async2.testing
    pw_bytes
    pw_rpc2.cpp._connection
    pw_rpc2.cpp._packet
    pw_rpc2.cpp._test_utils
    pw_transport.cpp
  GROUPS
    modules
    pw_rpc2
)

pw_add_test(pw_rpc2.cpp.server_dispatch_test
  SOURCES
    server_dispatch_test.cc
  PRIVATE_DEPS
    pw_allocator.testing
    pw_async2
    pw_async2.testing
    pw_bytes
    pw_rpc2.cpp._handshake
    pw_rpc2.cpp._packet
    pw_rpc2.cpp._server
    pw_rpc2.cpp._test_utils
    pw_transport.cpp
  GROUPS
    modules
    pw_rpc2
)

pw_add_test(pw_rpc2.cpp.connection_task_test
  SOURCES
    connection_task_test.cc
  PRIVATE_DEPS
    pw_allocator.testing
    pw_async2
    pw_async2.testing
    pw_bytes
    pw_rpc2.cpp._handshake
    pw_rpc2.cpp._packet
    pw_rpc2.cpp._server
    pw_rpc2.cpp._test_utils
    pw_transport.cpp
  GROUPS
    modules
    pw_rpc2
)

pw_add_test(pw_rpc2.cpp.server_test
  SOURCES
    server_test.cc
  PRIVATE_DEPS
    pw_allocator.testing
    pw_async2
    pw_async2.testing
    pw_async2.value_future
    pw_rpc2.cpp._handshake
    pw_rpc2.cpp._packet
    pw_rpc2.cpp._server
    pw_rpc2.cpp._test_utils
    pw_thread.test_thread_context
    pw_thread.thread
    pw_transport.cpp
  GROUPS
    modules
    pw_rpc2
)

pw_add_test(pw_rpc2.cpp.method_future_test
  SOURCES
    method_future_test.cc
  PRIVATE_DEPS
    pw_allocator.testing
    pw_async2
    ${_pw_rpc2_coro_deps}
    pw_async2.testing
    pw_rpc2.cpp._server
  GROUPS
    modules
    pw_rpc2
)

pw_add_test(pw_rpc2.cpp.internal_server_call_test
  SOURCES
    internal_server_call_test.cc
  PRIVATE_DEPS
    pw_allocator.testing
    pw_assert.check
    pw_async2
    ${_pw_rpc2_coro_deps}
    pw_async2.testing
    pw_rpc2.cpp._server
    pw_rpc2.cpp._test_utils
    pw_transport.cpp
  GROUPS
    modules
    pw_rpc2
)

pw_add_test(pw_rpc2.cpp.method_test
  SOURCES
    method_test.cc
  PRIVATE_DEPS
    pw_allocator.testing
    pw_assert.check
    pw_async2
    ${_pw_rpc2_coro_deps}
    pw_async2.testing
    pw_buf
    pw_bytes
    pw_rpc2.cpp._method
    pw_rpc2.cpp._server
    pw_rpc2.cpp._test_utils
    pw_transport.cpp
  GROUPS
    modules
    pw_rpc2
)

pw_add_test(pw_rpc2.cpp.method_traits_test
  SOURCES
    method_traits_test.cc
  PRIVATE_DEPS
    pw_async2.poll
    pw_buf
    pw_bytes
    pw_result
    pw_rpc2.cpp._connection
    pw_rpc2.cpp._method
    pw_rpc2.cpp._server
    pw_status
  GROUPS
    modules
    pw_rpc2
)

pw_add_test(pw_rpc2.cpp.server_call_test
  SOURCES
    server_call_test.cc
  PRIVATE_DEPS
    pw_allocator.testing
    pw_async2
    pw_async2.testing
    pw_rpc2.cpp._packet
    pw_rpc2.cpp._server
    pw_rpc2.cpp._test_utils
    pw_transport.cpp
  GROUPS
    modules
    pw_rpc2
)


