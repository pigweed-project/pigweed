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
  pw_bluetooth_proxy
  pw_checksum
  pw_multibuf
)

set(RFCOMM_PRIVATE_DEPS
  pw_bluetooth.emboss_util
  pw_bluetooth.emboss_rfcomm_frames
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

pw_add_library(pw_bluetooth_proxy.rfcomm._sync_mbv1 STATIC
  HEADERS
    ${RFCOMM_HEADERS}
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    ${RFCOMM_PUBLIC_DEPS}
    pw_bluetooth_proxy._sync_mbv1
  PRIVATE_DEPS
    ${RFCOMM_PRIVATE_DEPS}
  SOURCES
    ${RFCOMM_SOURCES}
)
target_compile_definitions(pw_bluetooth_proxy.rfcomm._sync_mbv1 PUBLIC
  PW_BLUETOOTH_PROXY_ASYNC=0
  PW_BLUETOOTH_PROXY_MULTIBUF=PW_BLUETOOTH_PROXY_MULTIBUF_V1
)

pw_add_library(pw_bluetooth_proxy.rfcomm._async_mbv1 STATIC
  HEADERS
    ${RFCOMM_HEADERS}
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    ${RFCOMM_PUBLIC_DEPS}
    pw_bluetooth_proxy._async_mbv1
  PRIVATE_DEPS
    ${RFCOMM_PRIVATE_DEPS}
  SOURCES
    ${RFCOMM_SOURCES}
)
target_compile_definitions(pw_bluetooth_proxy.rfcomm._async_mbv1 PUBLIC
  PW_BLUETOOTH_PROXY_ASYNC=1
  PW_BLUETOOTH_PROXY_MULTIBUF=PW_BLUETOOTH_PROXY_MULTIBUF_V1
)

pw_add_library(pw_bluetooth_proxy.rfcomm._sync_mbv2 STATIC
  HEADERS
    ${RFCOMM_HEADERS}
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    ${RFCOMM_PUBLIC_DEPS}
    pw_bluetooth_proxy._sync_mbv2
  PRIVATE_DEPS
    ${RFCOMM_PRIVATE_DEPS}
  SOURCES
    ${RFCOMM_SOURCES}
)
target_compile_definitions(pw_bluetooth_proxy.rfcomm._sync_mbv2 PUBLIC
  PW_BLUETOOTH_PROXY_ASYNC=0
  PW_BLUETOOTH_PROXY_MULTIBUF=PW_BLUETOOTH_PROXY_MULTIBUF_V2
)

pw_add_library(pw_bluetooth_proxy.rfcomm._async_mbv2 STATIC
  HEADERS
    ${RFCOMM_HEADERS}
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    ${RFCOMM_PUBLIC_DEPS}
    pw_bluetooth_proxy._async_mbv2
  PRIVATE_DEPS
    ${RFCOMM_PRIVATE_DEPS}
  SOURCES
    ${RFCOMM_SOURCES}
)
target_compile_definitions(pw_bluetooth_proxy.rfcomm._async_mbv2 PUBLIC
  PW_BLUETOOTH_PROXY_ASYNC=1
  PW_BLUETOOTH_PROXY_MULTIBUF=PW_BLUETOOTH_PROXY_MULTIBUF_V2
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

pw_add_test(pw_bluetooth_proxy.rfcomm.rfcomm_test_sync_mbv1
  PRIVATE_DEPS
    ${RFCOMM_TEST_PRIVATE_DEPS}
    pw_bluetooth_proxy.rfcomm._sync_mbv1
  SOURCES
    ${RFCOMM_TEST_SOURCES}
  GROUPS
    modules
)

pw_add_test(pw_bluetooth_proxy.rfcomm.rfcomm_test_async_mbv1
  PRIVATE_DEPS
    ${RFCOMM_TEST_PRIVATE_DEPS}
    pw_bluetooth_proxy.rfcomm._async_mbv1
  SOURCES
    ${RFCOMM_TEST_SOURCES}
  GROUPS
    modules
)

pw_add_test(pw_bluetooth_proxy.rfcomm.rfcomm_test_sync_mbv2
  PRIVATE_DEPS
    ${RFCOMM_TEST_PRIVATE_DEPS}
    pw_bluetooth_proxy.rfcomm._sync_mbv2
  SOURCES
    ${RFCOMM_TEST_SOURCES}
  GROUPS
    modules
)

pw_add_test(pw_bluetooth_proxy.rfcomm.rfcomm_test_async_mbv2
  PRIVATE_DEPS
    ${RFCOMM_TEST_PRIVATE_DEPS}
    pw_bluetooth_proxy.rfcomm._async_mbv2
  SOURCES
    ${RFCOMM_TEST_SOURCES}
  GROUPS
    modules
)