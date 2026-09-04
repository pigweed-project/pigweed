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

"""Helpers for using Pigweed with zephyr-bazel."""

load("//pw_build:merge_flags.bzl", "flags_from_dict")

_PW_ZEPHYR_DEFAULT_FLAGS = {
    "@pigweed//pw_assert:assert_backend": "@pigweed//pw_assert_basic",
    "@pigweed//pw_assert:assert_backend_impl": "@pigweed//pw_assert_basic:impl",
    "@pigweed//pw_assert:check_backend": "@pigweed//pw_assert_basic",
    "@pigweed//pw_assert:check_backend_impl": "@pigweed//pw_assert_basic:impl",
    "@pigweed//pw_interrupt:backend": "@pigweed//pw_interrupt_zephyr:context",
    "@pigweed//pw_log:backend": "@pigweed//pw_log_basic",
    "@pigweed//pw_log:backend_impl": "@pigweed//pw_build:empty_cc_library",
    "@pigweed//pw_malloc:backend": "@pigweed//pw_malloc:bucket_block_allocator",
    "@pigweed//pw_sys_io:backend": "@pigweed//pw_sys_io_zephyr",
}

def pw_zephyr_platform(name, parents = [], flags = [], constraint_values = []):
    """Creates a Zephyr platform configured with standard Pigweed build settings.

    Args:
        name: Name of the platform target.
        parents: List of parent platforms (typically one, e.g., the base board platform from Zephyr).
        flags: A list of already-formatted flags to append. Overrides the defaults if duplicates exist.
        constraint_values: List of constraint_values for the platform.
    """
    merged_constraint_values = ["@pigweed//pw_build/constraints/rtos:zephyr"] + constraint_values

    native.platform(
        name = name,
        parents = parents,
        flags = flags_from_dict(_PW_ZEPHYR_DEFAULT_FLAGS) + flags,
        constraint_values = merged_constraint_values,
    )
