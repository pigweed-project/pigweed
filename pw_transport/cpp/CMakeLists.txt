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

pw_add_library(pw_transport.cpp.socket STATIC
  HEADERS
    public/pw_transport/socket.h
    public/pw_transport/transport.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_allocator
    pw_assert.assert
    pw_async2
    pw_async2.value_future
    pw_buf
    pw_intrusive_ptr
    pw_memory
    pw_result
    pw_status
    pw_sync.lock_annotations
  SOURCES
    socket.cc
)

pw_add_library(pw_transport.cpp.forwarding_socket STATIC
  HEADERS
    public/pw_transport/forwarding_socket.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_allocator
    pw_result
    pw_transport.cpp.socket
  PRIVATE_DEPS
    pw_assert.assert
    pw_assert.check
    pw_async2
    pw_buf
    pw_containers.queue
    pw_intrusive_ptr
    pw_status
    pw_sync.interrupt_spin_lock
    pw_sync.lock_annotations
  SOURCES
    forwarding_socket.cc
)

pw_add_test(pw_transport.cpp.forwarding_socket_test
  SOURCES
    forwarding_socket_test.cc
  PRIVATE_DEPS
    pw_allocator.null_allocator
    pw_allocator.testing
    pw_async2
    pw_async2.testing
    pw_buf
    pw_result
    pw_status
    pw_transport.cpp.forwarding_socket
    pw_transport.cpp.socket
    pw_unit_test.status_macros
  GROUPS
    modules
    pw_transport
)

pw_add_test(pw_transport.cpp.transport_test
  SOURCES
    transport_test.cc
  PRIVATE_DEPS
    pw_transport.cpp.socket
    pw_allocator.testing
    pw_async2
    pw_async2.testing
    pw_bytes
    pw_sync.interrupt_spin_lock
  GROUPS
    modules
    pw_transport
)
