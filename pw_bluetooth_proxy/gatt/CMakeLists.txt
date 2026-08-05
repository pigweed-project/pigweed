# Copyright 2025 The Pigweed Authors
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

###############################################################################
##          Everything below here is intended to be emboss only              ##
##          and will be skipped if emboss isn't enabled.                     ##
###############################################################################
if("${dir_pw_third_party_emboss}" STREQUAL "")
  # Skip emboss defs if it's not configured
  return()
endif()

# LINT.IfChange
set(PW_BLUETOOTH_PROXY_GATT_PUBLIC_DEPS
    pw_allocator
    pw_assert.check
    pw_containers.dynamic_map
    pw_containers.dynamic_vector
    pw_log
)

set(PW_BLUETOOTH_PROXY_GATT_PRIVATE_DEPS
    pw_bluetooth.emboss_att
    pw_bluetooth.emboss_util
    pw_span.cast
)

set(PW_BLUETOOTH_PROXY_GATT_SRCS
    gatt.cc
)

# LINT.ThenChange(Android.bp, gatt.bzl, BUILD.gn)

# LINT.IfChange
pw_add_library(pw_bluetooth_proxy.gatt STATIC
  HEADERS
    public/pw_bluetooth_proxy/gatt/gatt.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    ${PW_BLUETOOTH_PROXY_GATT_PUBLIC_DEPS}
    pw_bluetooth_proxy
  PRIVATE_DEPS
    ${PW_BLUETOOTH_PROXY_GATT_PRIVATE_DEPS}
  SOURCES
    ${PW_BLUETOOTH_PROXY_GATT_SRCS}
)

# Exit early if missing test requirements.
if(("${pw_thread.thread_BACKEND}" STREQUAL "") OR
   ("${pw_thread.test_thread_context_BACKEND}" STREQUAL ""))
  return()
endif()

pw_add_library(pw_bluetooth_proxy.gatt._sync STATIC
  HEADERS
    public/pw_bluetooth_proxy/gatt/gatt.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    ${PW_BLUETOOTH_PROXY_GATT_PUBLIC_DEPS}
    pw_bluetooth_proxy._sync
  PRIVATE_DEPS
    ${PW_BLUETOOTH_PROXY_GATT_PRIVATE_DEPS}
  SOURCES
    ${PW_BLUETOOTH_PROXY_GATT_SRCS}
)
target_compile_definitions(pw_bluetooth_proxy.gatt._sync PUBLIC
  PW_BLUETOOTH_PROXY_ASYNC=0
)

pw_add_library(pw_bluetooth_proxy.gatt._sync_recovery STATIC
  HEADERS
    public/pw_bluetooth_proxy/gatt/gatt.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    ${PW_BLUETOOTH_PROXY_GATT_PUBLIC_DEPS}
    pw_bluetooth_proxy._sync_recovery
  PRIVATE_DEPS
    ${PW_BLUETOOTH_PROXY_GATT_PRIVATE_DEPS}
  SOURCES
    ${PW_BLUETOOTH_PROXY_GATT_SRCS}
)
target_compile_definitions(pw_bluetooth_proxy.gatt._sync_recovery PUBLIC
  PW_BLUETOOTH_PROXY_ASYNC=0
)

pw_add_library(pw_bluetooth_proxy.gatt._async STATIC
  HEADERS
    public/pw_bluetooth_proxy/gatt/gatt.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    ${PW_BLUETOOTH_PROXY_GATT_PUBLIC_DEPS}
    pw_bluetooth_proxy._async
  PRIVATE_DEPS
    ${PW_BLUETOOTH_PROXY_GATT_PRIVATE_DEPS}
  SOURCES
    ${PW_BLUETOOTH_PROXY_GATT_SRCS}
)
target_compile_definitions(pw_bluetooth_proxy.gatt._async PUBLIC
  PW_BLUETOOTH_PROXY_ASYNC=1
)

pw_add_library(pw_bluetooth_proxy.gatt._async_recovery STATIC
  HEADERS
    public/pw_bluetooth_proxy/gatt/gatt.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    ${PW_BLUETOOTH_PROXY_GATT_PUBLIC_DEPS}
    pw_bluetooth_proxy._async_recovery
  PRIVATE_DEPS
    ${PW_BLUETOOTH_PROXY_GATT_PRIVATE_DEPS}
  SOURCES
    ${PW_BLUETOOTH_PROXY_GATT_SRCS}
)
target_compile_definitions(pw_bluetooth_proxy.gatt._async_recovery PUBLIC
  PW_BLUETOOTH_PROXY_ASYNC=1
)

set(PW_BLUETOOTH_PROXY_GATT_TEST_DEPS
  pw_allocator.libc_allocator
  pw_allocator.synchronized_allocator
  pw_allocator.testing
  pw_chrono.system_clock
  pw_containers.vector
  pw_multibuf.simple_allocator
  pw_span.cast
  pw_sync.no_lock
  pw_sync.timed_thread_notification
  pw_thread.test_thread_context
  pw_thread.thread
)

pw_add_test(pw_bluetooth_proxy.gatt.gatt_sync_test
  PRIVATE_DEPS
    ${PW_BLUETOOTH_PROXY_GATT_TEST_DEPS}
    pw_bluetooth_proxy.gatt._sync
  SOURCES
    gatt_test.cc
  GROUPS
    modules
)

pw_add_test(pw_bluetooth_proxy.gatt.gatt_sync_recovery_test
  PRIVATE_DEPS
    ${PW_BLUETOOTH_PROXY_GATT_TEST_DEPS}
    pw_bluetooth_proxy.gatt._sync_recovery
  SOURCES
    gatt_test.cc
  GROUPS
    modules
)

pw_add_test(pw_bluetooth_proxy.gatt.gatt_async_test
  PRIVATE_DEPS
    ${PW_BLUETOOTH_PROXY_GATT_TEST_DEPS}
    pw_bluetooth_proxy.gatt._async
  SOURCES
    gatt_test.cc
  GROUPS
    modules
)

pw_add_test(pw_bluetooth_proxy.gatt.gatt_async_recovery_test
  PRIVATE_DEPS
    ${PW_BLUETOOTH_PROXY_GATT_TEST_DEPS}
    pw_bluetooth_proxy.gatt._async_recovery
  SOURCES
    gatt_test.cc
  GROUPS
    modules
)

pw_add_test(pw_bluetooth_proxy.gatt.gatt_integration_test
  PRIVATE_DEPS
    pw_allocator.testing
    pw_bluetooth_proxy.gatt._async
    pw_bluetooth_proxy._test_utils_async
    pw_containers.vector
    pw_multibuf.from_span
    pw_span.cast
  SOURCES
    gatt_integration_test.cc
  GROUPS
    modules
)
# LINT.ThenChange(gatt.bzl, BUILD.gn)
