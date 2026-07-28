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

def pw_bluetooth_proxy_gatt_library(name, versioned_deps, **kwargs):
    """Creates a cc_library for gatt with a specific version of some deps.

    Args:
      name:           Name of the target.
      versioned_deps: List of labels of a version-specific dependencies.
      **kwargs:       Additional arguments to pass to cc_library.
    """
    cc_library(
        name = name,

        # LINT.IfChange
        srcs = [
            "gatt.cc",
        ],
        # LINT.ThenChange(Android.bp, BUILD.gn, CMakeLists.txt)

        # LINT.IfChange
        hdrs = [
            "public/pw_bluetooth_proxy/gatt/gatt.h",
        ],
        # LINT.ThenChange(BUILD.gn, CMakeLists.txt)
        strip_include_prefix = "public",

        # LINT.IfChange
        deps = [
            "//pw_allocator",
            "//pw_assert:check",
            "//pw_bluetooth:emboss_att",
            "//pw_bluetooth:emboss_util",
            "//pw_containers:dynamic_map",
            "//pw_containers:dynamic_vector",
            "//pw_log",
            "//pw_span:cast",
        ] + versioned_deps,
        # LINT.ThenChange(Android.bp, BUILD.gn, CMakeLists.txt)
        **kwargs
    )

def pw_bluetooth_proxy_gatt_test(name, versioned_deps, **kwargs):
    """Creates a cc_library gatt with a specific version of some deps.

    Args:
      name:           Name of the target.
      versioned_deps: List of labels of a version-specific dependencies.
      **kwargs:       Additional arguments to pass to pw_cc_test.
    """
    pw_cc_test(
        name = name,

        # LINT.IfChange
        srcs = [
            "gatt_test.cc",
        ],
        deps = [
            "//pw_allocator:libc_allocator",
            "//pw_allocator:synchronized_allocator",
            "//pw_allocator:testing",
            "//pw_chrono:system_clock",
            "//pw_containers:vector",
            "//pw_multibuf:simple_allocator",
            "//pw_span:cast",
            "//pw_sync:no_lock",
            "//pw_sync:timed_thread_notification",
            "//pw_thread:test_thread_context",
            "//pw_thread:thread",
        ] + versioned_deps,
        # LINT.ThenChange(BUILD.gn, CMakeLists.txt)
        **kwargs
    )
