# Copyright 2023 The Pigweed Authors
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

pw_add_module_config(pw_log_rpc_CONFIG)

pw_add_library(pw_log_rpc.config INTERFACE
  HEADERS
    public/pw_log_rpc/internal/config.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    ${pw_log_rpc_CONFIG}
)

pw_add_library(pw_log_rpc.log_config INTERFACE
  HEADERS
    public/pw_log_rpc/internal/log_config.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_log_rpc.config
)

pw_add_library(pw_log_rpc.log_service STATIC
  HEADERS
    public/pw_log_rpc/log_service.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_log_rpc.rpc_log_drain
    pw_log.protos.raw_rpc
  SOURCES
    log_service.cc
  PRIVATE_DEPS
    pw_log
    pw_log.protos.pwpb
    pw_log_rpc.log_config
)

pw_add_library(pw_log_rpc.log_filter_service STATIC
  HEADERS
    public/pw_log_rpc/log_filter_service.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_log.protos.raw_rpc
    pw_log_rpc.log_filter
    pw_protobuf.bytes_utils
  SOURCES
    log_filter_service.cc
  PRIVATE_DEPS
    pw_log
    pw_log.protos.pwpb
    pw_protobuf
)

pw_add_library(pw_log_rpc.log_filter STATIC
  HEADERS
    public/pw_log_rpc/log_filter.h
    public/pw_log_rpc/log_filter_map.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_assert
    pw_bytes
    pw_containers.vector
    pw_log.protos.pwpb
    pw_log_rpc.config
    pw_protobuf
    pw_span
    pw_status
  SOURCES
    log_filter.cc
  PRIVATE_DEPS
    pw_log
    pw_log.protos.pwpb
)

pw_add_library(pw_log_rpc.rpc_log_drain STATIC
  HEADERS
    public/pw_log_rpc/rpc_log_drain.h
    public/pw_log_rpc/rpc_log_drain_map.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_assert
    pw_chrono.system_clock
    pw_function
    pw_log.protos.pwpb
    pw_log.protos.raw_rpc
    pw_log_rpc.config
    pw_log_rpc.log_filter
    pw_multisink
    pw_protobuf
    pw_result
    pw_span
    pw_status
    pw_sync.lock_annotations
    pw_sync.mutex
  SOURCES
    rpc_log_drain.cc
)

pw_add_library(pw_log_rpc.rpc_log_drain_thread INTERFACE
  HEADERS
    public/pw_log_rpc/rpc_log_drain_thread.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_chrono.system_clock
    pw_log_rpc.log_service
    pw_log_rpc.rpc_log_drain
    pw_multisink
    pw_result
    pw_rpc.raw.server_api
    pw_status
    pw_sync.timed_thread_notification
    pw_thread.thread
    pw_thread.thread_core
)

pw_add_library(pw_log_rpc.test_utils STATIC
  HEADERS
    pw_log_rpc_private/test_utils.h
  PUBLIC_INCLUDES
    .
  PUBLIC_DEPS
    pw_bytes
    pw_containers.vector
    pw_log_tokenized.metadata
    pw_protobuf
    pw_unit_test
  SOURCES
    test_utils.cc
  PRIVATE_DEPS
    pw_log
    pw_log.protos.pwpb
    pw_protobuf.bytes_utils
)

if(NOT "${pw_chrono.system_clock_BACKEND}" STREQUAL "")
  pw_add_test(pw_log_rpc.log_service_test
    SOURCES
      log_service_test.cc
    PRIVATE_DEPS
      pw_containers.vector
      pw_log
      pw_log.proto_utils
      pw_log.protos.pwpb
      pw_log_rpc.log_filter
      pw_log_rpc.log_service
      pw_log_rpc.test_utils
      pw_log_tokenized.metadata
      pw_protobuf
      pw_protobuf.bytes_utils
      pw_result
      pw_rpc.raw.test_method_context
      pw_status
    GROUPS
      modules
      pw_log_rpc
  )
endif()

pw_add_test(pw_log_rpc.log_filter_service_test
  SOURCES
    log_filter_service_test.cc
  PRIVATE_DEPS
    pw_log.protos.pwpb
    pw_log_rpc.log_filter
    pw_log_rpc.log_filter_service
    pw_protobuf
    pw_protobuf.bytes_utils
    pw_result
    pw_rpc.raw.test_method_context
    pw_status
  GROUPS
    modules
    pw_log_rpc
)

pw_add_test(pw_log_rpc.log_filter_test
  SOURCES
    log_filter_test.cc
  PRIVATE_DEPS
    pw_log
    pw_log.proto_utils
    pw_log.protos.pwpb
    pw_log_rpc.log_filter
    pw_log_tokenized.metadata
    pw_result
    pw_status
  GROUPS
    modules
    pw_log_rpc
)

if((NOT "${pw_chrono.system_clock_BACKEND}" STREQUAL "") AND
   (NOT "${pw_sync.timed_thread_notification_BACKEND}" STREQUAL "") AND
   (NOT "${pw_thread.test_thread_context_BACKEND}" STREQUAL "") AND
   (NOT "${pw_thread.thread_BACKEND}" STREQUAL ""))
  pw_add_test(pw_log_rpc.rpc_log_drain_test
    SOURCES
      rpc_log_drain_test.cc
    PRIVATE_DEPS
      pw_bytes
      pw_chrono.system_clock
      pw_log.proto_utils
      pw_log.protos.pwpb
      pw_log_rpc.log_filter
      pw_log_rpc.log_service
      pw_log_rpc.rpc_log_drain
      pw_log_rpc.test_utils
      pw_log_tokenized.metadata
      pw_multisink
      pw_protobuf
      pw_rpc.common
      pw_rpc.raw.fake_channel_output
      pw_rpc.raw.server_api
      pw_rpc.raw.test_method_context
      pw_status
      pw_sync.mutex
      pw_sync.thread_notification
      pw_sync.timed_thread_notification
      pw_thread.test_thread_context
      pw_thread.thread
      pw_thread.thread_core
    GROUPS
      modules
      pw_log_rpc
  )
endif()
