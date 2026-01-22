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
"""
This file contains custom rules for building the pw_bluetooth_proxy library and
tests with different versions of MultiBufs. This allows testing with both
MultiBuf v1 and v2, and allows downstream consumers to select the version in use
with a third version that uses a module configuration option.
"""

load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("//pw_unit_test:pw_cc_test.bzl", "pw_cc_test")

def pw_bluetooth_proxy_library(name, versioned_deps, **kwargs):
    """Creates a cc_library for bt-proxy with a specific version of some deps.

    TODO(b/448714138): This really ought to be achieved using an aspect.

    Args:
      name:           Name of the target.
      versioned_deps: List of labels of a version-specific dependencies.
      **kwargs:       Additional arguments to pass to cc_library.
    """
    cc_library(
        name = name,

        # LINT.IfChange
        srcs = [
            "acl_data_channel.cc",
            "basic_l2cap_channel.cc",
            "gatt_notify_channel.cc",
            "gatt_notify_tx_engine.cc",
            "generic_l2cap_channel.cc",
            "generic_l2cap_channel_async.cc",
            "generic_l2cap_channel_sync.cc",
            "l2cap_channel.cc",
            "l2cap_channel_async.cc",
            "l2cap_channel_sync.cc",
            "l2cap_channel_manager.cc",
            "l2cap_channel_manager_async.cc",
            "l2cap_channel_manager_sync.cc",
            "l2cap_coc.cc",
            "l2cap_signaling_channel.cc",
            "l2cap_status_tracker.cc",
            "l2cap_logical_link.cc",
            "multibuf_v1.cc",
            "multibuf_v2.cc",
            "proxy_host.cc",
            "proxy_host_async.cc",
            "proxy_host_sync.cc",
            "recombiner.cc",
            "basic_mode_tx_engine.cc",
            "basic_mode_rx_engine.cc",
            "credit_based_flow_control_tx_engine.cc",
            "credit_based_flow_control_rx_engine.cc",
            "channel_proxy_impl.cc",
        ],
        # LINT.ThenChange(Android.bp, BUILD.gn, CMakeLists.txt)

        # LINT.IfChange
        hdrs = [
            "public/pw_bluetooth_proxy/basic_l2cap_channel.h",
            "public/pw_bluetooth_proxy/connection_handle.h",
            "public/pw_bluetooth_proxy/config.h",
            "public/pw_bluetooth_proxy/direction.h",
            "public/pw_bluetooth_proxy/gatt_notify_channel.h",
            "public/pw_bluetooth_proxy/h4_packet.h",
            "public/pw_bluetooth_proxy/internal/acl_data_channel.h",
            "public/pw_bluetooth_proxy/internal/basic_mode_tx_engine.h",
            "public/pw_bluetooth_proxy/internal/basic_mode_rx_engine.h",
            "public/pw_bluetooth_proxy/internal/channel_proxy_impl.h",
            "public/pw_bluetooth_proxy/internal/credit_based_flow_control_rx_engine.h",
            "public/pw_bluetooth_proxy/internal/credit_based_flow_control_tx_engine.h",
            "public/pw_bluetooth_proxy/internal/gatt_notify_rx_engine.h",
            "public/pw_bluetooth_proxy/internal/gatt_notify_tx_engine.h",
            "public/pw_bluetooth_proxy/internal/generic_l2cap_channel.h",
            "public/pw_bluetooth_proxy/internal/generic_l2cap_channel_async.h",
            "public/pw_bluetooth_proxy/internal/generic_l2cap_channel_sync.h",
            "public/pw_bluetooth_proxy/internal/hci_transport.h",
            "public/pw_bluetooth_proxy/internal/l2cap_channel.h",
            "public/pw_bluetooth_proxy/internal/l2cap_channel_async.h",
            "public/pw_bluetooth_proxy/internal/l2cap_channel_sync.h",
            "public/pw_bluetooth_proxy/internal/l2cap_channel_manager.h",
            "public/pw_bluetooth_proxy/internal/l2cap_channel_manager_async.h",
            "public/pw_bluetooth_proxy/internal/l2cap_channel_manager_sync.h",
            "public/pw_bluetooth_proxy/internal/l2cap_logical_link.h",
            "public/pw_bluetooth_proxy/internal/l2cap_signaling_channel.h",
            "public/pw_bluetooth_proxy/internal/l2cap_status_tracker.h",
            "public/pw_bluetooth_proxy/internal/locked_l2cap_channel.h",
            "public/pw_bluetooth_proxy/internal/logical_transport.h",
            "public/pw_bluetooth_proxy/internal/multibuf.h",
            "public/pw_bluetooth_proxy/internal/mutex.h",
            "public/pw_bluetooth_proxy/internal/proxy_host_async.h",
            "public/pw_bluetooth_proxy/internal/proxy_host_sync.h",
            "public/pw_bluetooth_proxy/internal/recombiner.h",
            "public/pw_bluetooth_proxy/internal/rx_engine.h",
            "public/pw_bluetooth_proxy/internal/tx_engine.h",
            "public/pw_bluetooth_proxy/l2cap_channel_common.h",
            "public/pw_bluetooth_proxy/l2cap_coc.h",
            "public/pw_bluetooth_proxy/l2cap_coc_config.h",
            "public/pw_bluetooth_proxy/l2cap_status_delegate.h",
            "public/pw_bluetooth_proxy/proxy_host.h",
            "public/pw_bluetooth_proxy/channel_proxy.h",
            "public/pw_bluetooth_proxy/l2cap_channel_manager_interface.h",
        ],
        features = ["-conversion_warnings"],
        # LINT.ThenChange(BUILD.gn, CMakeLists.txt)
        implementation_deps = [
            "//pw_assert:check",
            "//pw_bluetooth:emboss_att",
            "//pw_bluetooth:emboss_hci_commands",
            "//pw_bluetooth:emboss_util",
            "//pw_containers:algorithm",
            "//pw_log",
            "//pw_span:cast",
        ],
        strip_include_prefix = "public",

        # LINT.IfChange
        deps = [
            ":config",
            "//pw_allocator",
            "//pw_allocator:best_fit",
            "//pw_allocator:synchronized_allocator",
            "//pw_async2:basic_dispatcher",
            "//pw_async2:channel",
            "//pw_async2",
            "//pw_async2:poll",
            "//pw_bluetooth:emboss_hci_common",
            "//pw_bluetooth:emboss_hci_data",
            "//pw_bluetooth:emboss_hci_events",
            "//pw_bluetooth:emboss_hci_h4",
            "//pw_bluetooth:emboss_l2cap_frames",
            "//pw_bytes",
            "//pw_containers:flat_map",
            "//pw_containers:inline_queue",
            "//pw_containers:vector",
            "//pw_containers:intrusive_map",
            "//pw_function",
            "//pw_multibuf:allocator",
            "//pw_multibuf:multibuf_v1",
            "//pw_multibuf:multibuf_v2",
            "//pw_result",
            "//pw_span",
            "//pw_status",
            "//pw_sync:lock_annotations",
            "//pw_sync:mutex",
            "//pw_sync:thread_notification",
            "//pw_thread:id",
        ] + versioned_deps,
        # LINT.ThenChange(Android.bp, BUILD.gn, CMakeLists.txt)
        **kwargs
    )

def pw_bluetooth_proxy_test(name, versioned_deps, **kwargs):
    """Creates a cc_library for bt-proxy with a specific version of some deps.

    Args:
      name:           Name of the target.
      versioned_deps: List of labels of a version-specific dependencies.
      **kwargs:       Additional arguments to pass to pw_cc_test.
    """
    pw_cc_test(
        name = name,

        # LINT.IfChange
        srcs = [
            "pw_bluetooth_proxy_private/test_utils.h",
            "basic_mode_tx_engine_test.cc",
            "basic_mode_rx_engine_test.cc",
            "basic_mode_channel_proxy_test.cc",
            "channel_proxy_test.cc",
            "credit_based_flow_control_tx_engine_test.cc",
            "credit_based_flow_control_rx_engine_test.cc",
            "gatt_notify_test.cc",
            "gatt_notify_tx_engine_test.cc",
            "h4_packet_test.cc",
            "l2cap_coc_test.cc",
            "proxy_host_test.cc",
            "recombiner_test.cc",
            "test_utils.cc",
            "test_utils_async.cc",
            "test_utils_sync.cc",
            "utils_test.cc",
        ],
        features = ["-conversion_warnings"],
        deps = [
            "//pw_allocator:libc_allocator",
            "//pw_allocator:null_allocator",
            "//pw_allocator:synchronized_allocator",
            "//pw_allocator:testing",
            "//pw_async2:notified_dispatcher",
            "//pw_assert:check",
            "//pw_bluetooth:emboss_att",
            "//pw_bluetooth:emboss_hci_commands",
            "//pw_bluetooth:emboss_hci_common",
            "//pw_bluetooth:emboss_hci_events",
            "//pw_bluetooth:emboss_hci_h4",
            "//pw_bluetooth:emboss_util",
            "//pw_span:cast",
            "//pw_sync:mutex",
            "//pw_sync:thread_notification",
            "//pw_thread:test_thread_context",
            "//pw_thread:thread",
            "//pw_unit_test",
        ] + versioned_deps,
        # LINT.ThenChange(BUILD.gn)
        **kwargs
    )

def pw_bluetooth_proxy_test_utils(name, versioned_deps, **kwargs):
    """Creates a cc_library for test tools with a specific version of some deps.

    Args:
      name:           Name of the target.
      versioned_deps: List of labels of a version-specific dependencies.
      **kwargs:       Additional arguments to pass to cc_library.
    """
    cc_library(
        name = name,
        testonly = True,
        hdrs = [
            "pw_bluetooth_proxy_private/test_utils.h",
        ],

        # LINT.IfChange
        srcs = [
            "test_utils.cc",
            "test_utils_async.cc",
            "test_utils_sync.cc",
        ],
        features = ["-conversion_warnings"],
        deps = [
            "//pw_allocator:null_allocator",
            "//pw_allocator:synchronized_allocator",
            "//pw_allocator:testing",
            "//pw_assert:check",
            "//pw_bluetooth:emboss_att",
            "//pw_bluetooth:emboss_hci_commands",
            "//pw_bluetooth:emboss_hci_common",
            "//pw_bluetooth:emboss_hci_events",
            "//pw_bluetooth:emboss_hci_h4",
            "//pw_bluetooth:emboss_l2cap_frames",
            "//pw_bluetooth:emboss_util",
            "//pw_unit_test",
            "//pw_sync:thread_notification",
            "//pw_thread:test_thread_context",
            "//pw_thread:thread",
            "//pw_function",
            "//pw_allocator:libc_allocator",
            "//pw_async2:notified_dispatcher",
            "//pw_span:cast",
            "//pw_sync:mutex",
        ] + versioned_deps,
        # LINT.ThenChange(BUILD.gn)
        **kwargs
    )
