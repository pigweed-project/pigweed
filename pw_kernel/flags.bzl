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
"""Standard kernel flags which can be used by the various platform definitions..
"""

load("@bazel_skylib//lib:dicts.bzl", "dicts")

KERNEL_COMMON_FLAGS = {
    "@pigweed//pw_assert/rust:debug_assertions": True,
    # Default to using the tokenized backend.  Platforms can overwrite this value
    "@pigweed//pw_log/rust:pw_log_backend": "@pigweed//pw_kernel:log_backend_tokenized",
    "@pigweed//pw_toolchain:cortex-m_toolchain_kind": "clang",
}

KERNEL_DEVICE_COMMON_FLAGS = dicts.add(KERNEL_COMMON_FLAGS, {
    # Don't use LTO on host builds, as it doesn't
    # currently work on mac x86. There's also no real size
    # constraints on host builds.  Once b/419358200 is
    # fixed, we can re-consider whether it's useful
    # for host builds.
    "@rules_rust//rust/settings:lto": "fat",
})

KERNEL_TEST_RUSTC_FLAGS = select({
    "@pigweed//pw_kernel:tests_enabled_setting": ["--cfg=test"],
    "//conditions:default": [],
})

KERNEL_TEST_DEPS = select({
    "@pigweed//pw_kernel:tests_enabled_setting": ["@pigweed//pw_kernel/lib/unittest"],
    "//conditions:default": [],
})
