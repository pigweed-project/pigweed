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
include($ENV{PW_ROOT}/pw_thread/backend.cmake)
include($ENV{PW_ROOT}/third_party/emboss/emboss.cmake)

add_subdirectory(hci)

###############################################################################
##          Everything below here is intended to be emboss only              ##
##          and will be skipped if emboss isn't enabled.                     ##
###############################################################################
if("${dir_pw_third_party_emboss}" STREQUAL "")
  # Skip emboss defs if it's not configured
  return()
endif()

# TODO(b/448714138): Find a better way of creating and testing specific module
# configurations.

# LINT.IfChange
set(PW_BLUETOOTH_PROXY_HEADERS
  public/pw_bluetooth_proxy/basic_l2cap_channel.h
  public/pw_bluetooth_proxy/connection_handle.h
  public/pw_bluetooth_proxy/channel_proxy.h
  public/pw_bluetooth_proxy/direction.h
  public/pw_bluetooth_proxy/gatt_notify_channel.h
  public/pw_bluetooth_proxy/h4_packet.h
  public/pw_bluetooth_proxy/internal/acl_data_channel.h
  public/pw_bluetooth_proxy/internal/basic_mode_tx_engine.h
  public/pw_bluetooth_proxy/internal/basic_mode_rx_engine.h
  public/pw_bluetooth_proxy/internal/channel_proxy_impl.h
  public/pw_bluetooth_proxy/internal/credit_based_flow_control_tx_engine.h
  public/pw_bluetooth_proxy/internal/credit_based_flow_control_rx_engine.h
  public/pw_bluetooth_proxy/internal/gatt_notify_rx_engine.h
  public/pw_bluetooth_proxy/internal/gatt_notify_tx_engine.h
  public/pw_bluetooth_proxy/internal/generic_l2cap_channel.h
  public/pw_bluetooth_proxy/internal/generic_l2cap_channel_sync.h
  public/pw_bluetooth_proxy/internal/generic_l2cap_channel_async.h
  public/pw_bluetooth_proxy/internal/hci_transport.h
  public/pw_bluetooth_proxy/internal/l2cap_channel_manager.h
  public/pw_bluetooth_proxy/internal/l2cap_channel_manager_sync.h
  public/pw_bluetooth_proxy/internal/l2cap_channel_manager_async.h
  public/pw_bluetooth_proxy/internal/l2cap_channel.h
  public/pw_bluetooth_proxy/internal/l2cap_channel_sync.h
  public/pw_bluetooth_proxy/internal/l2cap_channel_async.h
  public/pw_bluetooth_proxy/internal/l2cap_signaling_channel.h
  public/pw_bluetooth_proxy/internal/l2cap_status_tracker.h
  public/pw_bluetooth_proxy/internal/l2cap_logical_link.h
  public/pw_bluetooth_proxy/internal/locked_l2cap_channel.h
  public/pw_bluetooth_proxy/internal/logical_transport.h
  public/pw_bluetooth_proxy/internal/mutex.h
  public/pw_bluetooth_proxy/internal/proxy_host_sync.h
  public/pw_bluetooth_proxy/internal/proxy_host_async.h
  public/pw_bluetooth_proxy/internal/recombiner.h
  public/pw_bluetooth_proxy/internal/rx_engine.h
  public/pw_bluetooth_proxy/internal/tx_engine.h
  public/pw_bluetooth_proxy/l2cap_channel_common.h
  public/pw_bluetooth_proxy/l2cap_channel_manager_interface.h
  public/pw_bluetooth_proxy/l2cap_coc.h
  public/pw_bluetooth_proxy/l2cap_coc_config.h
  public/pw_bluetooth_proxy/l2cap_status_delegate.h
  public/pw_bluetooth_proxy/proxy_host.h
  public/pw_bluetooth_proxy/connection_handle.h
  public/pw_bluetooth_proxy/channel_proxy.h
  public/pw_bluetooth_proxy/l2cap_channel_manager_interface.h
)
# LINT.ThenChange(bt-proxy.bzl, BUILD.gn)

# LINT.IfChange
set(PW_BLUETOOTH_PROXY_PUBLIC_DEPS
  pw_allocator
  pw_allocator.best_fit
  pw_allocator.synchronized_allocator
  pw_async2.basic_dispatcher
  pw_async2.channel
  pw_async2.dispatcher
  pw_async2.poll
  pw_bluetooth.emboss_hci_data
  pw_bluetooth.emboss_hci_events
  pw_bluetooth.emboss_hci_h4
  pw_bluetooth.emboss_l2cap_frames
  pw_bytes
  pw_containers.flat_map
  pw_containers.inline_queue
  pw_containers.intrusive_map
  pw_containers.vector
  pw_function
  pw_bluetooth_proxy.multibuf
  pw_result
  pw_span
  pw_span.cast
  pw_status
  pw_sync.lock_annotations
  pw_sync.mutex
  pw_sync.thread_notification
  pw_thread.id
)

set(PW_BLUETOOTH_PROXY_PRIVATE_DEPS
  pw_containers.intrusive_map
  pw_allocator.best_fit
  pw_allocator.synchronized_allocator
  pw_bluetooth.emboss_att
  pw_bluetooth.emboss_hci_commands
  pw_bluetooth.emboss_util
  pw_containers.algorithm
  pw_log
  pw_span.cast
)

set(PW_BLUETOOTH_PROXY_SOURCES
  acl_data_channel.cc
  basic_l2cap_channel.cc
  basic_mode_tx_engine.cc
  basic_mode_rx_engine.cc
  channel_proxy_impl.cc
  credit_based_flow_control_tx_engine.cc
  credit_based_flow_control_rx_engine.cc
  gatt_notify_channel.cc
  gatt_notify_tx_engine.cc
  generic_l2cap_channel_async.cc
  generic_l2cap_channel_sync.cc
  generic_l2cap_channel.cc
  l2cap_channel_manager_async.cc
  l2cap_channel_manager_sync.cc
  l2cap_channel_manager.cc
  l2cap_channel_async.cc
  l2cap_channel_sync.cc
  l2cap_channel.cc
  l2cap_coc.cc
  l2cap_signaling_channel.cc
  l2cap_status_tracker.cc
  l2cap_logical_link.cc
  proxy_host.cc
  proxy_host_async.cc
  proxy_host_sync.cc
  recombiner.cc
)
# LINT.ThenChange(Android.bp, bt-proxy.bzl, BUILD.gn)

# Module configuration

pw_add_module_config(pw_bluetooth_proxy_CONFIG)

pw_add_library(pw_bluetooth_proxy.config INTERFACE
  HEADERS
    public/pw_bluetooth_proxy/config.h
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    ${pw_bluetooth_proxy_CONFIG}
)

# Libraries

pw_add_library(pw_bluetooth_proxy STATIC
  HEADERS
    ${PW_BLUETOOTH_PROXY_HEADERS}
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    ${PW_BLUETOOTH_PROXY_PUBLIC_DEPS}
    pw_bluetooth_proxy.multibuf
  PRIVATE_DEPS
    ${PW_BLUETOOTH_PROXY_PRIVATE_DEPS}
  SOURCES
    ${PW_BLUETOOTH_PROXY_SOURCES}
)

pw_add_library(pw_bluetooth_proxy.multibuf STATIC

# LINT.IfChange
  HEADERS
    public/pw_bluetooth_proxy/internal/multibuf.h
  PUBLIC_DEPS
    pw_allocator
    pw_bluetooth_proxy.config
    pw_bytes
    pw_multibuf.allocator
    pw_multibuf.multibuf_v1
    pw_multibuf.multibuf_v2
  PRIVATE_DEPS
    pw_assert.check
    pw_span.cast
  SOURCES
    multibuf_v1.cc
    multibuf_v2.cc
# LINT.ThenChange(Android.bp, bt-proxy.bzl, BUILD.gn)

)

pw_add_library(pw_bluetooth_proxy._multibuf_v1 STATIC
  HEADERS
    public/pw_bluetooth_proxy/internal/multibuf.h
  PUBLIC_DEPS
    pw_bluetooth_proxy.config
    pw_bytes
    pw_multibuf.allocator
    pw_multibuf.multibuf_v1
  PRIVATE_DEPS
    pw_assert.check
    pw_span.cast
  SOURCES
    multibuf_v1.cc
)
target_compile_definitions(pw_bluetooth_proxy._multibuf_v1 PUBLIC
  PW_BLUETOOTH_PROXY_MULTIBUF=PW_BLUETOOTH_PROXY_MULTIBUF_V1
)

pw_add_library(pw_bluetooth_proxy._multibuf_v2 STATIC
  HEADERS
    public/pw_bluetooth_proxy/internal/multibuf.h
  PUBLIC_DEPS
    pw_allocator
    pw_bluetooth_proxy.config
    pw_bytes
    pw_multibuf.multibuf_v2
  PRIVATE_DEPS
    pw_assert.check
    pw_span.cast
  SOURCES
    multibuf_v2.cc
)
target_compile_definitions(pw_bluetooth_proxy._multibuf_v2 PUBLIC
  PW_BLUETOOTH_PROXY_MULTIBUF=PW_BLUETOOTH_PROXY_MULTIBUF_V2
)

pw_add_library(pw_bluetooth_proxy._sync_mbv1 STATIC
  HEADERS
    ${PW_BLUETOOTH_PROXY_HEADERS}
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    ${PW_BLUETOOTH_PROXY_PUBLIC_DEPS}
    pw_bluetooth_proxy._multibuf_v1
  PRIVATE_DEPS
    ${PW_BLUETOOTH_PROXY_PRIVATE_DEPS}
  SOURCES
    ${PW_BLUETOOTH_PROXY_SOURCES}

)
target_compile_definitions(pw_bluetooth_proxy._sync_mbv1 PUBLIC
  PW_BLUETOOTH_PROXY_ASYNC=0
  PW_BLUETOOTH_PROXY_MULTIBUF=PW_BLUETOOTH_PROXY_MULTIBUF_V1
)

pw_add_library(pw_bluetooth_proxy._async_mbv1 STATIC
  HEADERS
    ${PW_BLUETOOTH_PROXY_HEADERS}
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    ${PW_BLUETOOTH_PROXY_PUBLIC_DEPS}
    pw_bluetooth_proxy._multibuf_v1
  PRIVATE_DEPS
    ${PW_BLUETOOTH_PROXY_PRIVATE_DEPS}
  SOURCES
    ${PW_BLUETOOTH_PROXY_SOURCES}
)
target_compile_definitions(pw_bluetooth_proxy._async_mbv1 PUBLIC
  PW_BLUETOOTH_PROXY_ASYNC=1
  PW_BLUETOOTH_PROXY_MULTIBUF=PW_BLUETOOTH_PROXY_MULTIBUF_V1
)

pw_add_library(pw_bluetooth_proxy._sync_mbv2 STATIC
  HEADERS
    ${PW_BLUETOOTH_PROXY_HEADERS}
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    ${PW_BLUETOOTH_PROXY_PUBLIC_DEPS}
    pw_bluetooth_proxy._multibuf_v2
  PRIVATE_DEPS
    ${PW_BLUETOOTH_PROXY_PRIVATE_DEPS}
  SOURCES
    ${PW_BLUETOOTH_PROXY_SOURCES}
)
target_compile_definitions(pw_bluetooth_proxy._sync_mbv2 PUBLIC
  PW_BLUETOOTH_PROXY_ASYNC=0
  PW_BLUETOOTH_PROXY_MULTIBUF=PW_BLUETOOTH_PROXY_MULTIBUF_V2
)

pw_add_library(pw_bluetooth_proxy._async_mbv2 STATIC
  HEADERS
    ${PW_BLUETOOTH_PROXY_HEADERS}
  PUBLIC_INCLUDES
    public
  PUBLIC_DEPS
    ${PW_BLUETOOTH_PROXY_PUBLIC_DEPS}
    pw_bluetooth_proxy._multibuf_v2
  PRIVATE_DEPS
    ${PW_BLUETOOTH_PROXY_PRIVATE_DEPS}
  SOURCES
    ${PW_BLUETOOTH_PROXY_SOURCES}
)
target_compile_definitions(pw_bluetooth_proxy._async_mbv2 PUBLIC
  PW_BLUETOOTH_PROXY_ASYNC=1
  PW_BLUETOOTH_PROXY_MULTIBUF=PW_BLUETOOTH_PROXY_MULTIBUF_V2
)

# GATT depends on the above targets, so add_subdirectory() must be called last.
add_subdirectory(gatt)

# Tests

# Exit early if missing test requirements.
if(("${pw_thread.thread_BACKEND}" STREQUAL "") OR
   ("${pw_thread.test_thread_context_BACKEND}" STREQUAL ""))
  return()
endif()

set(PW_BLUETOOTH_PROXY_TEST_PRIVATE_DEPS
  pw_allocator
  pw_allocator.libc_allocator
  pw_allocator.null_allocator
  pw_allocator.synchronized_allocator
  pw_allocator.testing
  pw_assert.check
  pw_bluetooth.emboss_att
  pw_bluetooth.emboss_hci_commands
  pw_bluetooth.emboss_hci_common
  pw_bluetooth.emboss_hci_events
  pw_bluetooth.emboss_hci_h4
  pw_bluetooth.emboss_l2cap_frames
  pw_bluetooth.emboss_util
  pw_containers.to_array
  pw_function
  pw_multibuf.simple_allocator
  pw_span.cast
  pw_status
  pw_sync.mutex
  pw_sync.thread_notification
  pw_third_party.fuchsia.stdcompat
  pw_thread.thread
  pw_thread.test_thread_context
  pw_unit_test
)

set(PW_BLUETOOTH_PROXY_TEST_SOURCES
  pw_bluetooth_proxy_private/test_utils.h
  basic_mode_channel_proxy_test.cc
  basic_mode_tx_engine_test.cc
  basic_mode_rx_engine_test.cc
  channel_proxy_test.cc
  credit_based_flow_control_tx_engine_test.cc
  credit_based_flow_control_rx_engine_test.cc
  gatt_notify_test.cc
  gatt_notify_tx_engine_test.cc
  h4_packet_test.cc
  l2cap_coc_test.cc
  proxy_host_test.cc
  recombiner_test.cc
  test_utils.cc
  test_utils_async.cc
  test_utils_sync.cc
  utils_test.cc
)

pw_add_library(pw_bluetooth_proxy._test_utils_async_mbv2 STATIC
  PUBLIC_DEPS
    ${PW_BLUETOOTH_PROXY_TEST_PRIVATE_DEPS}
    pw_bluetooth_proxy._async_mbv2
  HEADERS
    pw_bluetooth_proxy_private/test_utils.h
  SOURCES
    test_utils.cc
    test_utils_async.cc
    test_utils_sync.cc
)

pw_add_test(pw_bluetooth_proxy.pw_bluetooth_proxy_sync_mbv1_test
  PRIVATE_DEPS
    ${PW_BLUETOOTH_PROXY_TEST_PRIVATE_DEPS}
    pw_bluetooth_proxy._sync_mbv1
  SOURCES
    ${PW_BLUETOOTH_PROXY_TEST_SOURCES}
  GROUPS
    modules
)

pw_add_test(pw_bluetooth_proxy.pw_bluetooth_proxy_async_mbv1_test
  PRIVATE_DEPS
    ${PW_BLUETOOTH_PROXY_TEST_PRIVATE_DEPS}
    pw_bluetooth_proxy._async_mbv1
  SOURCES
    ${PW_BLUETOOTH_PROXY_TEST_SOURCES}
  GROUPS
    modules
)

pw_add_test(pw_bluetooth_proxy.pw_bluetooth_proxy_sync_mbv2_test
  PRIVATE_DEPS
    ${PW_BLUETOOTH_PROXY_TEST_PRIVATE_DEPS}
    pw_bluetooth_proxy._sync_mbv2
  SOURCES
    ${PW_BLUETOOTH_PROXY_TEST_SOURCES}
  GROUPS
    modules
)

pw_add_test(pw_bluetooth_proxy.pw_bluetooth_proxy_async_mbv2_test
  PRIVATE_DEPS
    ${PW_BLUETOOTH_PROXY_TEST_PRIVATE_DEPS}
    pw_bluetooth_proxy._async_mbv2
  SOURCES
    ${PW_BLUETOOTH_PROXY_TEST_SOURCES}
  GROUPS
    modules
)
