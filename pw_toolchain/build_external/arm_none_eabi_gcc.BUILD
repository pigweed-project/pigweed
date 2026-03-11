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

load("@rules_cc//cc/toolchains:tool.bzl", "cc_tool")
load("@rules_cc//cc/toolchains:tool_map.bzl", "cc_tool_map")

package(default_visibility = ["//visibility:public"])

licenses(["notice"])

# This build file defines a complete set of tools for an arm-none-eabi GNU
# binutils-based toolchain.

exports_files(glob(["bin/**"]))

cc_tool_map(
    name = "all_tools",
    tools = {
        "@pigweed//pw_toolchain/action:gcov": ":arm-none-eabi-gcov",
        "@pigweed//pw_toolchain/action:nm": ":arm-none-eabi-nm",
        "@pigweed//pw_toolchain/action:objdump_disassemble": ":arm-none-eabi-objdump",
        "@pigweed//pw_toolchain/action:readelf": ":arm-none-eabi-readelf",
        "@pigweed//pw_toolchain/action:size": ":arm-none-eabi-size",
        "@rules_cc//cc/toolchains/actions:ar_actions": ":arm-none-eabi-ar",
        "@rules_cc//cc/toolchains/actions:assembly_actions": ":asm",
        "@rules_cc//cc/toolchains/actions:c_compile_actions": ":arm-none-eabi-gcc",
        "@rules_cc//cc/toolchains/actions:cpp_compile_actions": ":arm-none-eabi-g++",
        "@rules_cc//cc/toolchains/actions:link_actions": ":arm-none-eabi-ld",
        "@rules_cc//cc/toolchains/actions:objcopy_embed_data": ":arm-none-eabi-objcopy",
        "@rules_cc//cc/toolchains/actions:strip": ":arm-none-eabi-strip",
    },
)

cc_tool(
    name = "arm-none-eabi-ar",
    src = select({
        "@platforms//os:windows": "//:bin/arm-none-eabi-ar.exe",
        "//conditions:default": "//:bin/arm-none-eabi-ar",
    }),
)

cc_tool(
    name = "arm-none-eabi-g++",
    src = select({
        "@platforms//os:windows": "//:bin/arm-none-eabi-g++.exe",
        "//conditions:default": "//:bin/arm-none-eabi-g++",
    }),
    data = glob([
        "**/*.spec",
        "**/*.specs",
        "arm-none-eabi/include/**",
        "lib/gcc/arm-none-eabi/*/include/**",
        "lib/gcc/arm-none-eabi/*/include-fixed/**",
        "libexec/**",
    ]),
)

# TODO: https://github.com/bazelbuild/rules_cc/issues/235 - Workaround until
# Bazel has a more robust way to implement `cc_tool_map`.
alias(
    name = "asm",
    actual = ":arm-none-eabi-gcc",
)

cc_tool(
    name = "arm-none-eabi-gcc",
    src = select({
        "@platforms//os:windows": "//:bin/arm-none-eabi-gcc.exe",
        "//conditions:default": "//:bin/arm-none-eabi-gcc",
    }),
    data = glob([
        "**/*.spec",
        "**/*.specs",
        "arm-none-eabi/include/**",
        "lib/gcc/arm-none-eabi/*/include/**",
        "lib/gcc/arm-none-eabi/*/include-fixed/**",
        "libexec/**",
    ]) +
    # The assembler needs to be explicilty added. Note that the path is
    # intentionally different here as `as` is called from arm-none-eabi-gcc.
    # `arm-none-eabi-as` will not suffice for this context.
    select({
        "@platforms//os:windows": ["//:arm-none-eabi/bin/as.exe"],
        "//conditions:default": ["//:arm-none-eabi/bin/as"],
    }),
)

# This tool is actually just g++ under the hood, we just enumerate this
# tool differently to specify a different set of additional files.
cc_tool(
    name = "arm-none-eabi-ld",
    src = select({
        "@platforms//os:windows": "//:bin/arm-none-eabi-g++.exe",
        "//conditions:default": "//:bin/arm-none-eabi-g++",
    }),
    data = glob([
        "**/*.a",
        "**/*.ld",
        "**/*.o",
        "**/*.spec",
        "**/*.specs",
        "**/*.so",
        "libexec/**",
    ]) + [
        "//:arm-none-eabi/bin/ld",
    ],
)

cc_tool(
    name = "arm-none-eabi-gcov",
    src = select({
        "@platforms//os:windows": "//:bin/arm-none-eabi-gcov.exe",
        "//conditions:default": "//:bin/arm-none-eabi-gcov",
    }),
)

cc_tool(
    name = "arm-none-eabi-objcopy",
    src = select({
        "@platforms//os:windows": "//:bin/arm-none-eabi-objcopy.exe",
        "//conditions:default": "//:bin/arm-none-eabi-objcopy",
    }),
)

cc_tool(
    name = "arm-none-eabi-objdump",
    src = select({
        "@platforms//os:windows": "//:bin/arm-none-eabi-objdump.exe",
        "//conditions:default": "//:bin/arm-none-eabi-objdump",
    }),
)

cc_tool(
    name = "arm-none-eabi-strip",
    src = select({
        "@platforms//os:windows": "//:bin/arm-none-eabi-strip.exe",
        "//conditions:default": "//:bin/arm-none-eabi-strip",
    }),
)

cc_tool(
    name = "arm-none-eabi-nm",
    src = select({
        "@platforms//os:windows": "//:bin/arm-none-eabi-nm.exe",
        "//conditions:default": "//:bin/arm-none-eabi-nm",
    }),
)

cc_tool(
    name = "arm-none-eabi-readelf",
    src = select({
        "@platforms//os:windows": "//:bin/arm-none-eabi-readelf.exe",
        "//conditions:default": "//:bin/arm-none-eabi-readelf",
    }),
)

cc_tool(
    name = "arm-none-eabi-size",
    src = select({
        "@platforms//os:windows": "//:bin/arm-none-eabi-size.exe",
        "//conditions:default": "//:bin/arm-none-eabi-size",
    }),
)
