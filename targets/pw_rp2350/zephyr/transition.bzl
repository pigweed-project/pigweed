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

"""Bazel transition for compiling Pigweed binaries under the Zephyr build context."""

load("@rules_platform//platform_data:defs.bzl", "platform_data")
load("@zephyr-bazel//bazel:zephyr_transition.bzl", "zephyr_transition")

def _rp2350_zephyr_binary_impl(ctx):
    # Retrieve the transitioned target binary's output files.
    binary_files = ctx.attr.binary[DefaultInfo].files.to_list()
    if not binary_files:
        fail("Binary target %s produced no files" % ctx.attr.binary.label)
    in_file = binary_files[0]

    # Declare a local output file to satisfy Bazel's 'executable = True' rule requirement.
    out_file = ctx.actions.declare_file(ctx.label.name)

    # Symlink the transitioned binary ELF to the local executable file.
    ctx.actions.symlink(
        output = out_file,
        target_file = in_file,
        is_executable = True,
    )

    return [
        DefaultInfo(
            files = depset([out_file]),
            executable = out_file,
        ),
    ]

# Rule that applies the Zephyr Bzlmod configuration transition to a target binary.
# The transition maps Kconfig and Devicetree parameters to build settings.
_rp2350_zephyr_binary_rule = rule(
    implementation = _rp2350_zephyr_binary_impl,
    attrs = {
        "app_label": attr.string(default = "//app"),
        "binary": attr.label(mandatory = True, cfg = "target"),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
    },
    executable = True,
    cfg = zephyr_transition,
)

def rp2350_zephyr_binary(name = "", binary = "", testonly = False):
    """Transitions a binary target to compile under the Zephyr platform build context.

    This macro:
    1. Instantiates _rp2350_zephyr_binary_rule to transition the build context
       (Kconfig/Devicetree) based on the board platform configured in the
       platforms flag.
    2. Wraps the target in platform_data to force the initial platform
       transition to the RP2350 board.
    """
    elf_target_name = name + "_zephyr_transitioned"
    _rp2350_zephyr_binary_rule(
        name = elf_target_name,
        binary = binary,
        testonly = testonly,
    )

    platform_data(
        name = name,
        target = ":" + elf_target_name,
        testonly = testonly,
        platform = "//boards:rp2350a_m33",
    )
