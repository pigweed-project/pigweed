# Copyright 2020 The Pigweed Authors
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

include($ENV{PW_ROOT}/pw_unit_test/backend.cmake)

pw_add_module_config(pw_unit_test_CONFIG)

pw_add_library(pw_unit_test.config INTERFACE
  HEADERS
    public/pw_unit_test/config.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    ${pw_unit_test_CONFIG}
    pw_polyfill
)

add_library(pw_unit_test INTERFACE)
target_include_directories(pw_unit_test
  INTERFACE
    public
)

# Temporarily redirect deprecated googletest pointer to new pointer.
target_link_libraries(pw_unit_test
  INTERFACE
    "${pw_unit_test_BACKEND}"
)

pw_add_library(pw_unit_test.ffi_test_runner INTERFACE
  HEADERS
    public/pw_unit_test/ffi_test_runner.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_preprocessor
    pw_unit_test.event_handler
)

pw_add_library(pw_unit_test.register_ffi_runner STATIC
  SOURCES
    rust/register_ffi_runner.cc
  PUBLIC_DEPS
    pw_unit_test.ffi_test_runner
)

pw_add_library(pw_unit_test.light STATIC
  HEADERS
    light_public_overrides/pw_unit_test/framework_backend.h
  SOURCES
    ffi_test_runner.cc
    framework_light.cc
  PUBLIC_DEPS
    pw_bytes.alignment
    pw_polyfill
    pw_preprocessor
    pw_result
    pw_status
    pw_string.builder
    pw_unit_test.config
    pw_unit_test.event_handler
    pw_unit_test.ffi_test_runner
  PRIVATE_DEPS
    pw_assert
  PUBLIC_INCLUDES
    light_public_overrides
    public
    public_overrides
  PRIVATE_INCLUDES
    .
)

pw_add_library(pw_unit_test.googletest INTERFACE
  HEADERS
    googletest_public_overrides/pw_unit_test/framework_backend.h
  PUBLIC_INCLUDES
    googletest_public_overrides
  PUBLIC_DEPS
    pw_third_party.googletest
    pw_result
    pw_status
)

pw_add_library(pw_unit_test.googletest_handler_adapter STATIC
  SOURCES
    googletest_handler_adapter.cc
  PUBLIC_DEPS
    pw_unit_test.event_handler
    pw_third_party.googletest
  PUBLIC_INCLUDES
    public
)

pw_add_library(pw_unit_test.fuzztest INTERFACE
  PUBLIC_DEPS
    pw_fuzzer.fuzztest
    pw_unit_test.googletest
)

pw_add_test(pw_unit_test.framework_test
    SOURCES
  framework_test.cc
    PRIVATE_DEPS
  pw_assert
  pw_status
    GROUPS
  pw_unit_test
)

if(${pw_unit_test_BACKEND} STREQUAL "pw_unit_test.light")
  pw_add_test(pw_unit_test.framework_light_test
    SOURCES
      framework_light_test.cc
    PRIVATE_DEPS
      pw_string
    GROUPS
      pw_unit_test
  )
endif()

pw_add_library(pw_unit_test.static_library_support STATIC
  HEADERS
    public/pw_unit_test/static_library_support.h
  PUBLIC_INCLUDES
    public
  SOURCES
    static_library_support.cc
  PUBLIC_DEPS
    pw_unit_test.light
)

pw_add_library(pw_unit_test.event_handler INTERFACE
  HEADERS
    public/pw_unit_test/event_handler.h
  PUBLIC_INCLUDES
    public
)

pw_add_library(pw_unit_test.status_macros INTERFACE
  HEADERS
    public/pw_unit_test/status_macros.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_status
    pw_third_party.fuchsia.stdcompat
)

pw_add_library(pw_unit_test.googletest_style_event_handler STATIC
  HEADERS
    public/pw_unit_test/googletest_style_event_handler.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_unit_test.event_handler
    pw_preprocessor
  SOURCES
    googletest_style_event_handler.cc
)

if(${pw_unit_test_BACKEND} STREQUAL "pw_unit_test.googletest")
  pw_add_library(pw_unit_test.googletest_test_matchers INTERFACE
    HEADERS
      public/pw_unit_test/googletest_test_matchers.h
    PUBLIC_INCLUDES
      public
    PUBLIC_DEPS
      pw_result
      pw_status
      pw_third_party.googletest
  )
  pw_add_test(pw_unit_test.googletest_test_matchers_test
    SOURCES
      googletest_test_matchers_test.cc
    PRIVATE_DEPS
      pw_unit_test.googletest_test_matchers
    GROUPS
      pw_unit_test
  )
endif()

pw_add_library(pw_unit_test.simple_printing_main OBJECT
  SOURCES
    simple_printing_main.cc
    simple_printing_event_handler.cc
  PRIVATE_DEPS
    pw_unit_test
    pw_unit_test.googletest_style_event_handler
    pw_preprocessor
    pw_string
    pw_sys_io
)

pw_add_library(pw_unit_test.logging_event_handler STATIC
  HEADERS
    public/pw_unit_test/logging_event_handler.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_log
    pw_unit_test.googletest_style_event_handler
  SOURCES
    logging_event_handler.cc
)

pw_add_library(pw_unit_test.logging_main STATIC
  SOURCES
    logging_main.cc
  PRIVATE_DEPS
    pw_unit_test.logging_event_handler
    pw_unit_test
)

pw_add_library(pw_unit_test.constexpr INTERFACE
  HEADERS
    public/pw_unit_test/constexpr.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_preprocessor
    pw_third_party.fuchsia.stdcompat
)

pw_add_test(pw_unit_test.constexpr_test
  SOURCES
    constexpr_test.cc
  PRIVATE_DEPS
    pw_compilation_testing._pigweed_only_negative_compilation
    pw_unit_test.constexpr
  GROUPS
    pw_unit_test
)

pw_add_library(pw_unit_test.multi_event_handler INTERFACE
  HEADERS
    public/pw_unit_test/multi_event_handler.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_unit_test.event_handler
)

pw_add_test(pw_unit_test.multi_event_handler_test
  SOURCES
    multi_event_handler_test.cc
  PRIVATE_DEPS
    pw_unit_test.multi_event_handler
  GROUPS
    pw_unit_test
)

pw_add_library(pw_unit_test.test_record_event_handler INTERFACE
  HEADERS
    public/pw_unit_test/internal/test_record_trie.h
    public/pw_unit_test/test_record_event_handler.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    pw_assert
    pw_json.builder
    pw_unit_test.event_handler
)

pw_add_test(pw_unit_test.test_record_event_handler_test
  SOURCES
    test_record_event_handler_test.cc
  PRIVATE_DEPS
    pw_unit_test.test_record_event_handler
  GROUPS
    pw_unit_test
)

pw_proto_library(pw_unit_test.unit_test_proto
  SOURCES
    pw_unit_test_proto/unit_test.proto
)

if("${pw_unit_test_BACKEND}" STREQUAL "pw_unit_test.light")
  pw_add_library(pw_unit_test.rpc_service STATIC
    PUBLIC_DEPS
      pw_unit_test.config
      pw_unit_test.event_handler
      pw_unit_test
      pw_unit_test.unit_test_proto.pwpb
      pw_unit_test.unit_test_proto.raw_rpc
      pw_containers.vector
      pw_string.util
      pw_sync.lock_annotations
      pw_sync.mutex
      pw_sync.thread_notification
      pw_thread.thread_core
    PRIVATE_DEPS
      pw_assert
      pw_log
    PUBLIC_INCLUDES
      public
      rpc_light_public
    HEADERS
      public/pw_unit_test/unit_test_service.h
      rpc_light_public/pw_unit_test/internal/rpc_event_handler.h
    SOURCES
      unit_test_service.cc
      rpc_light_event_handler.cc
  )
else()
  pw_add_library(pw_unit_test.rpc_service STATIC
    PUBLIC_DEPS
      pw_unit_test.config
      pw_unit_test.event_handler
      pw_unit_test
      pw_unit_test.unit_test_proto.pwpb
      pw_unit_test.unit_test_proto.raw_rpc
      pw_containers.vector
    PRIVATE_DEPS
      pw_log
    PUBLIC_INCLUDES
      public
      rpc_gtest_public
    HEADERS
      public/pw_unit_test/unit_test_service.h
      rpc_light_public/pw_unit_test/internal/rpc_event_handler.h
      rpc_gtest_public/pw_unit_test/internal/rpc_event_handler.h
    SOURCES
      unit_test_service.cc
      rpc_gtest_event_handler.cc
  )
endif()
