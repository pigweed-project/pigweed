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
"""Rules and macros related to platform compatibility."""

load("@bazel_skylib//lib:selects.bzl", "selects")

HOST_PLATFORMS = (
    "@platforms//os:android",
    "@platforms//os:chromiumos",
    "@platforms//os:ios",
    "@platforms//os:linux",
    "@platforms//os:macos",
    "@platforms//os:windows",
)

def host_backend_alias(name, backend):
    """An alias that resolves to the backend for host platforms."""
    native.alias(
        name = name,
        actual = selects.with_or({
            HOST_PLATFORMS: backend,
            "//conditions:default": str(Label("//pw_build:unspecified_backend")),
        }),
    )

def boolean_constraint_value(name, **kwargs):
    """Syntactic sugar for a constraint with just two possible values.

    Args:
      name: The name of the "True" value of the generated constraint.
      **kwargs: Passed on to native.constraint_value.
    """
    constraint_setting_name = name + ".constraint_setting"
    false_value_name = name + ".not"

    native.constraint_setting(
        name = constraint_setting_name,
        default_constraint_value = ":" + false_value_name,
        # Do not allow anyone to declare additional values for this setting.
        # It's boolean, so by definition it's true or false, that's it!
        visibility = ["//visibility:private"],
    )

    native.constraint_value(
        name = false_value_name,
        constraint_setting = ":" + constraint_setting_name,
        **kwargs
    )

    native.constraint_value(
        name = name,
        constraint_setting = ":" + constraint_setting_name,
        **kwargs
    )

def incompatible_with_mcu(unless_platform_has = None):
    """Helper for expressing incompatibility with MCU platforms.

    This helper should be used in `target_compatible_with` attributes to
    express:

    *  That a target is only compatible with platforms that have a
       full-featured OS, see
       https://pigweed.dev/bazel_compatibility.html#cross-platform-modules-requiring-an-os
    *  That a target is compatible with platforms with a full-featured OS, and
       also any platform that explicitly declares compatibility with it:
       https://pigweed.dev/bazel_compatibility.html#special-case-host-compatible-platform-specific-modules

    Args:
       unless_platform_has: A constraint_value that the target is compatible with
          by definition. Optional.
    """
    return select({
        "@platforms//os:none": [unless_platform_has] if (unless_platform_has != None) else ["@platforms//:incompatible"],
        "//conditions:default": [],
    })

def minimum_cxx_20():
    """Helper for expressing a C++20 requirement.

    This helper should be used in `target_compatible_with` attributes to express
    that a target requires C++20 or newer.
    """
    return select({
        "//pw_toolchain/cc:c++20_enabled": [],
        "//conditions:default": ["@platforms//:incompatible"],
    })
