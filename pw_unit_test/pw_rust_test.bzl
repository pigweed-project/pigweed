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
"""Macro for declaring Rust unit tests across host and device platforms."""

load("@pigweed//pw_unit_test:pw_cc_test.bzl", "pw_cc_test")
load("@rules_rust//rust:defs.bzl", "rust_library", "rust_test")

# Forwarding test rule to dispatch between host and device test executables.
# native.alias cannot be used because it is a non-test rule (test = False),
# and native.test_suite does not support select() on its tests attribute.
def _alias_test_impl(ctx):
    target = ctx.attr.actual

    executable = ctx.actions.declare_file(ctx.label.name)
    ctx.actions.symlink(
        output = executable,
        target_file = target[DefaultInfo].files_to_run.executable,
        is_executable = True,
    )
    return [
        DefaultInfo(
            executable = executable,
            runfiles = ctx.runfiles().merge(target[DefaultInfo].default_runfiles),
            files = target[DefaultInfo].files,
        ),
    ]

_alias_test = rule(
    implementation = _alias_test_impl,
    test = True,
    attrs = {
        "actual": attr.label(mandatory = True),
    },
)

def pw_rust_test(name, srcs, deps = None, **kwargs):
    """Declares a Rust unit test target.

    On host platforms, builds a native `rust_test` using Rust's libtest runner.
    On device platforms, builds a `pw_cc_test` wrapping a `rust_library` using
    Pigweed's C++ test runner.

    Args:
        name: base name of the test target.
        srcs: Rust source files for the test.
        deps: Dependencies forwarded to the test.
        **kwargs: Attributes forwarded to rust_test.
    """
    deps = [] if deps == None else deps
    crate_name = kwargs.pop("crate_name", name)
    edition = kwargs.pop("edition", "2021")
    tags = kwargs.pop("tags", [])
    subtarget_tags = tags + ["manual"]
    target_compatible_with = kwargs.pop("target_compatible_with", [])
    crate_features = kwargs.pop("crate_features", [])

    # Host: Native Rust test executable (rules_rust rust_test)
    host_test_name = name + ".rust_test"
    rust_test(
        name = host_test_name,
        crate_name = crate_name,
        srcs = srcs,
        deps = deps + ["@pigweed//pw_unit_test/rust:pw_unit_test"],
        edition = edition,
        tags = subtarget_tags,
        crate_features = crate_features + ["std"],
        # Sanitizers do not work with Rust tests because prebuilt libstd is not
        # compiled with sanitizer instrumentation.
        target_compatible_with = target_compatible_with + select({
            "@pigweed//pw_build/constraints/rust:no_std": ["@platforms//:incompatible"],
            "@pigweed//pw_toolchain/host_clang:asan_enabled": ["@platforms//:incompatible"],
            "@pigweed//pw_toolchain/host_clang:msan_enabled": ["@platforms//:incompatible"],
            "@pigweed//pw_toolchain/host_clang:tsan_enabled": ["@platforms//:incompatible"],
            "@pigweed//pw_toolchain/host_clang:ubsan_enabled": ["@platforms//:incompatible"],
            "//conditions:default": [],
        }),
        **kwargs
    )

    # Device: Rust library (named $name.lib to match pw_test convention)
    lib_name = name + ".lib"
    rustc_flags = kwargs.pop("rustc_flags", [])
    rust_library(
        name = lib_name,
        crate_name = crate_name,
        srcs = srcs,
        deps = deps + [
            "@pigweed//pw_unit_test/rust:pw_unit_test",
            "@pigweed//pw_build:default_link_extra_lib",
        ],
        edition = edition,
        tags = subtarget_tags,
        crate_features = crate_features,
        rustc_flags = rustc_flags + [
            "--cfg=test",
            "--cfg=pw_unit_test",
        ],
        target_compatible_with = target_compatible_with + select({
            "@pigweed//pw_build/constraints/rust:enabled": [],
            "//conditions:default": ["@platforms//:incompatible"],
        }),
        testonly = True,
    )

    device_test_name = name + ".pw_cc_test"
    pw_cc_test(
        name = device_test_name,
        deps = [
            ":" + lib_name,
            "@pigweed//pw_unit_test/rust:register_ffi_runner",
        ],
        tags = subtarget_tags,
        linkopts = [
            "-Wl,--whole-archive",
            "$(location :" + lib_name + ")",
            "-Wl,--no-whole-archive",
        ],
        additional_linker_inputs = [
            ":" + lib_name,
        ],
        target_compatible_with = target_compatible_with + select({
            "@pigweed//pw_build/constraints/rust:enabled": [],
            "//conditions:default": ["@platforms//:incompatible"],
        }),
    )

    # Top-level test target rule dispatching host vs device executable
    _alias_test(
        name = name,
        actual = select({
            "@platforms//os:none": ":" + device_test_name,
            "//conditions:default": ":" + host_test_name,
        }),
        tags = tags,
        target_compatible_with = target_compatible_with,
        visibility = kwargs.get("visibility"),
    )
