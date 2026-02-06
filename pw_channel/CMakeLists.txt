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
include($ENV{PW_ROOT}/pw_async2/backend.cmake)

pw_add_library(pw_channel INTERFACE
  HEADERS
    public/pw_channel/channel.h
    public/pw_channel/internal/channel_specializations.h
  PUBLIC_DEPS
    pw_assert
    pw_async2
    pw_bytes
    pw_channel.properties
    pw_memory._sibling_cast
    pw_multibuf
    pw_multibuf.allocator
    pw_multibuf.v1.allocator_async
    pw_result
    pw_span
    pw_status
  PUBLIC_INCLUDES
    public
)

pw_add_library(pw_channel.packet_channel INTERFACE
  HEADERS
    public/pw_channel/packet_channel.h
  PUBLIC_DEPS
    pw_async2
    pw_channel.properties
    pw_result
    pw_third_party.fuchsia.stdcompat
  PUBLIC_INCLUDES
    public
)

pw_add_library(pw_channel.properties INTERFACE
  HEADERS
    public/pw_channel/properties.h
  PUBLIC_INCLUDES
    public
)

pw_add_test(pw_channel.channel_test
  SOURCES
    channel_test.cc
  PRIVATE_DEPS
    pw_channel
    pw_allocator.testing
    pw_compilation_testing._pigweed_only_negative_compilation
    pw_preprocessor
    pw_multibuf.simple_allocator
)

pw_add_test(pw_channel.packet_channel_test
  SOURCES
    packet_channel_test.cc
  PRIVATE_DEPS
    pw_channel.packet_channel
    pw_allocator.allocator
    pw_async2
    pw_async2.testing
    pw_containers
    pw_unit_test
    pw_compilation_testing._pigweed_only_negative_compilation
    pw_third_party.fuchsia.stdcompat
)

pw_add_library(pw_channel.forwarding_channel STATIC
  HEADERS
    public/pw_channel/forwarding_channel.h
  SOURCES
    forwarding_channel.cc
  PUBLIC_DEPS
    pw_channel
    pw_multibuf.allocator
    pw_multibuf.v1.allocator_async
    pw_sync.mutex
  PUBLIC_INCLUDES
    public
)

pw_add_test(pw_channel.forwarding_channel_test
  SOURCES
    forwarding_channel_test.cc
  PRIVATE_DEPS
    pw_allocator.testing
    pw_async2.testing
    pw_channel.forwarding_channel
    pw_multibuf.simple_allocator
    pw_multibuf.v1.header_chunk_region_tracker
)

pw_add_library(pw_channel.loopback_channel STATIC
  HEADERS
    public/pw_channel/loopback_channel.h
  SOURCES
    loopback_channel.cc
  PUBLIC_DEPS
    pw_channel
    pw_multibuf.allocator
    pw_multibuf.v1.allocator_async
  PUBLIC_INCLUDES
    public
)

pw_add_test(pw_channel.loopback_channel_test
  SOURCES
    loopback_channel_test.cc
  PRIVATE_DEPS
    pw_channel.loopback_channel
    pw_multibuf.v1.allocator_async
    pw_multibuf.testing
    pw_async2.testing
)

pw_add_library(pw_channel.epoll_channel STATIC
  HEADERS
    public/pw_channel/epoll_channel.h
  SOURCES
    epoll_channel.cc
  PUBLIC_DEPS
    pw_async2.epoll_dispatcher
    pw_channel
    pw_multibuf.allocator
    pw_multibuf.v1.allocator_async
    pw_sync.mutex
  PUBLIC_INCLUDES
    public
  PRIVATE_DEPS
    pw_assert
    pw_log
)

pw_add_test(pw_channel.epoll_channel_test
  SOURCES
    epoll_channel_test.cc
  PRIVATE_DEPS
    pw_async2.epoll_dispatcher
    pw_async2.testing
    pw_channel.epoll_channel
    pw_multibuf.v1.allocator_async
    pw_multibuf.testing
    pw_async2.testing
    pw_thread.sleep
    pw_thread.thread
)

pw_add_library(pw_channel.stream_channel STATIC
  HEADERS
    public/pw_channel/stream_channel.h
  SOURCES
    stream_channel.cc
  PUBLIC_DEPS
    pw_channel
    pw_log
    pw_stream
    pw_sync.interrupt_spin_lock
    pw_sync.thread_notification
    pw_thread.thread
  PUBLIC_INCLUDES
    public
)

pw_add_test(pw_channel.stream_channel_test
  SOURCES
    stream_channel_test.cc
  PRIVATE_DEPS
    pw_async2
    pw_async2.testing
    pw_channel.stream_channel
    pw_multibuf.testing
    pw_multibuf.v1.allocator_async
    pw_stream
    pw_stream.mpsc_stream
    pw_thread.thread
    pw_thread.test_thread_context
)

pw_add_library(pw_channel.test_packet_channel INTERFACE
  HEADERS
    public/pw_channel/test_packet_channel.h
  PUBLIC_DEPS
    pw_allocator.testing
    pw_assert
    pw_async2
    pw_channel.packet_channel
    pw_containers.dynamic_deque
    pw_containers.dynamic_vector
    pw_span
  PUBLIC_INCLUDES
    public
)

pw_add_test(pw_channel.test_packet_channel_test
  SOURCES
    test_packet_channel_test.cc
  PRIVATE_DEPS
    pw_allocator.testing
    pw_async2
    pw_async2.testing
    pw_channel.test_packet_channel
    pw_result
)
