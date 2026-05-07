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

###############################################################################
##          Everything below here is intended to be emboss only              ##
##          and will be skipped if emboss isn't enabled.                     ##
###############################################################################
if("${dir_pw_third_party_emboss}" STREQUAL "")
  # Skip emboss defs if it's not configured
  return()
endif()

# LINT.IfChange
set(RFCOMM_HEADERS
  public/pw_bluetooth_proxy/rfcomm/internal/rfcomm_channel_internal.h
  public/pw_bluetooth_proxy/rfcomm/rfcomm_channel_manager_interface.h
  public/pw_bluetooth_proxy/rfcomm/rfcomm_common.h
  public/pw_bluetooth_proxy/rfcomm/rfcomm_config.h
  public/pw_bluetooth_proxy/rfcomm/rfcomm_manager.h
)
# LINT.ThenChange(rfcomm.bzl, BUILD.gn)

# LINT.IfChange
set(RFCOMM_PUBLIC_DEPS
  pw_allocator
  pw_bluetooth.emboss_rfcomm_frames
  pw_containers.dynamic_map
  pw_containers.intrusive_map
  pw_checksum
  pw_multibuf
)

set(RFCOMM_PRIVATE_DEPS
  pw_assert.check
  pw_bluetooth.emboss_util
  pw_span.cast
)

set(RFCOMM_SOURCES
  rfcomm_channel_internal.cc
  rfcomm_channel.cc
  rfcomm_manager.cc
)
# LINT.ThenChange(Android.bp, rfcomm.bzl, BUILD.gn)

# Libraries

pw_add_library(pw_bluetooth_proxy.rfcomm STATIC
  HEADERS
    ${RFCOMM_HEADERS}
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    ${RFCOMM_PUBLIC_DEPS}
    pw_bluetooth_proxy
  PRIVATE_DEPS
    ${RFCOMM_PRIVATE_DEPS}
  SOURCES
    ${RFCOMM_SOURCES}
)

pw_add_library(pw_bluetooth_proxy.rfcomm._sync STATIC
  HEADERS
    ${RFCOMM_HEADERS}
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    ${RFCOMM_PUBLIC_DEPS}
    pw_bluetooth_proxy._sync
  PRIVATE_DEPS
    ${RFCOMM_PRIVATE_DEPS}
  SOURCES
    ${RFCOMM_SOURCES}
)
target_compile_definitions(pw_bluetooth_proxy.rfcomm._sync PUBLIC
  PW_BLUETOOTH_PROXY_ASYNC=0
)

pw_add_library(pw_bluetooth_proxy.rfcomm._async STATIC
  HEADERS
    ${RFCOMM_HEADERS}
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    ${RFCOMM_PUBLIC_DEPS}
    pw_bluetooth_proxy._async
  PRIVATE_DEPS
    ${RFCOMM_PRIVATE_DEPS}
  SOURCES
    ${RFCOMM_SOURCES}
)
target_compile_definitions(pw_bluetooth_proxy.rfcomm._async PUBLIC
  PW_BLUETOOTH_PROXY_ASYNC=1
)

# Tests

set(RFCOMM_TEST_PRIVATE_DEPS
  pw_allocator
  pw_allocator.libc_allocator
  pw_allocator.testing
  pw_bluetooth.emboss_rfcomm_frames
  pw_bluetooth.emboss_util
  pw_checksum
  pw_multibuf.simple_allocator
  pw_unit_test
)

set(RFCOMM_TEST_SOURCES
  rfcomm_channel_internal_test.cc
  rfcomm_manager_test.cc
)

pw_add_test(pw_bluetooth_proxy.rfcomm.rfcomm_test_sync
  PRIVATE_DEPS
    ${RFCOMM_TEST_PRIVATE_DEPS}
    pw_bluetooth_proxy.rfcomm._sync
  SOURCES
    ${RFCOMM_TEST_SOURCES}
  GROUPS
    modules
)

pw_add_test(pw_bluetooth_proxy.rfcomm.rfcomm_test_async
  PRIVATE_DEPS
    ${RFCOMM_TEST_PRIVATE_DEPS}
    pw_bluetooth_proxy.rfcomm._async
  SOURCES
    ${RFCOMM_TEST_SOURCES}
  GROUPS
    modules
)
