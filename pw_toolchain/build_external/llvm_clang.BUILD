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

load("@bazel_skylib//lib:selects.bzl", "selects")
load("@bazel_skylib//rules:native_binary.bzl", "native_binary")
load("@bazel_skylib//rules/directory:directory.bzl", "directory")
load("@bazel_skylib//rules/directory:subdirectory.bzl", "subdirectory")
load("@pigweed//pw_build:glob_dirs.bzl", "match_dir")
load("@pigweed//pw_build:pw_py_importable_runfile.bzl", "pw_py_importable_runfile")
load("@pigweed//pw_build/constraints/arm:lists.bzl", "ALL_CORTEX_M_CPUS")
load("@pigweed//pw_build/constraints/riscv:lists.bzl", "ALL_RISCV_CPUS")
load("@rules_cc//cc/toolchains:args.bzl", "cc_args")
load("@rules_cc//cc/toolchains:args_list.bzl", "cc_args_list")
load("@rules_cc//cc/toolchains:tool.bzl", "cc_tool")
load("@rules_cc//cc/toolchains:tool_map.bzl", "cc_tool_map")

package(default_visibility = ["//visibility:public"])

licenses(["notice"])

# This build file defines a complete set of tools for a LLVM-based toolchain.

exports_files(glob(["**"]))

filegroup(
    name = "all",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)

# Export other tools in `bin` so they can be directly referenced if necessary.
exports_files(glob(["bin/**"]))

#####################
#       Tools       #
#####################

alias(
    name = "all_tools",
    actual = select({
        "@platforms//os:macos": ":macos_tools",
        "//conditions:default": ":default_tools",
    }),
)

COMMON_TOOLS = {
    "@pigweed//pw_toolchain/action:cov": ":llvm-cov",
    "@pigweed//pw_toolchain/action:gcov": ":llvm-gcov",
    "@pigweed//pw_toolchain/action:nm": ":llvm-nm",
    "@pigweed//pw_toolchain/action:objdump_disassemble": ":llvm-objdump",
    "@pigweed//pw_toolchain/action:readelf": ":llvm-readelf",
    "@pigweed//pw_toolchain/action:size": ":llvm-size",
    "@rules_cc//cc/toolchains/actions:assembly_actions": ":asm",
    "@rules_cc//cc/toolchains/actions:c_compile_actions": ":clang",
    "@rules_cc//cc/toolchains/actions:cpp_compile_actions": ":clang++",
    "@rules_cc//cc/toolchains/actions:link_actions": ":lld",
    "@rules_cc//cc/toolchains/actions:objcopy_embed_data": ":llvm-objcopy",
    "@rules_cc//cc/toolchains/actions:strip": ":llvm-strip",
}

cc_tool_map(
    name = "default_tools",
    tools = COMMON_TOOLS | {
        "@rules_cc//cc/toolchains/actions:ar_actions": ":llvm-ar",
    },
    visibility = ["//visibility:private"],
)

cc_tool_map(
    name = "macos_tools",
    tools = COMMON_TOOLS | {
        "@rules_cc//cc/toolchains/actions:ar_actions": ":llvm-libtool-darwin",
    },
    visibility = ["//visibility:private"],
)

# TODO: https://github.com/bazelbuild/rules_cc/issues/235 - Workaround until
# Bazel has a more robust way to implement `cc_tool_map`.
alias(
    name = "asm",
    actual = ":clang",
)

cc_tool(
    name = "clang",
    src = select({
        "@platforms//os:windows": "//:bin/clang.exe",
        "//conditions:default": "//:bin/clang",
    }),
    data = glob([
        "bin/llvm",
        "include/**",
        "lib/clang/*/include/**",
        "lib/clang/*/share/**",
    ]),
)

cc_tool(
    name = "clang++",
    src = select({
        "@platforms//os:windows": "//:bin/clang++.exe",
        "//conditions:default": "//:bin/clang++",
    }),
    data = glob([
        "bin/llvm",
        "include/**",
        "lib/clang/*/include/**",
        "lib/clang/*/share/**",
    ]),
)

cc_tool(
    name = "lld",
    src = select({
        "@platforms//os:windows": "//:bin/clang++.exe",
        "//conditions:default": "//:bin/clang++",
    }),
    data = glob(
        [
            "bin/llvm",
            "bin/lld*",
            "bin/ld*",
            "lib/**/*.a",
            "lib/**/*.dylib",
            "lib/**/*.so*",
            "lib/**/*.o",
        ],
        allow_empty = True,
    ),
)

cc_tool(
    name = "llvm-ar",
    src = select({
        "@platforms//os:windows": "//:bin/llvm-ar.exe",
        "//conditions:default": "//:bin/llvm-ar",
    }),
    data = glob(["bin/llvm"]),
)

cc_tool(
    name = "llvm-libtool-darwin",
    src = select({
        "@platforms//os:windows": "//:bin/llvm-libtool-darwin.exe",
        "//conditions:default": "//:bin/llvm-libtool-darwin",
    }),
    data = glob(["bin/llvm"]),
)

cc_tool(
    name = "llvm-objcopy",
    src = select({
        "@platforms//os:windows": "//:bin/llvm-objcopy.exe",
        "//conditions:default": "//:bin/llvm-objcopy",
    }),
    data = glob(["bin/llvm"]),
)

cc_tool(
    name = "llvm-objdump",
    src = select({
        "@platforms//os:windows": "//:bin/llvm-objdump.exe",
        "//conditions:default": "//:bin/llvm-objdump",
    }),
    data = glob(["bin/llvm"]),
)

cc_tool(
    name = "llvm-cov",
    src = select({
        "@platforms//os:windows": "//:bin/llvm-cov.exe",
        "//conditions:default": "//:bin/llvm-cov",
    }),
    data = glob(["bin/llvm"]),
)

cc_tool(
    name = "llvm-strip",
    src = select({
        "@platforms//os:windows": "//:bin/llvm-strip.exe",
        "//conditions:default": "//:bin/llvm-strip",
    }),
    data = glob(["bin/llvm"]),
)

cc_tool(
    name = "llvm-readelf",
    src = select({
        "@platforms//os:windows": "//:bin/llvm-readelf.exe",
        "//conditions:default": "//:bin/llvm-readelf",
    }),
    data = glob(["bin/llvm"]),
)

# These tools are mapped to actions that can be used via standard accessors
# defined in pw_toolchain/action.
cc_tool(
    name = "llvm-nm",
    src = select({
        "@platforms//os:windows": "//:bin/llvm-nm.exe",
        "//conditions:default": "//:bin/llvm-nm",
    }),
    data = glob(["bin/llvm"]),
)

cc_tool(
    name = "llvm-size",
    src = select({
        "@platforms//os:windows": "//:bin/llvm-size.exe",
        "//conditions:default": "//:bin/llvm-size",
    }),
    data = glob(["bin/llvm"]),
)

cc_tool(
    name = "llvm-gcov",
    src = select({
        "@platforms//os:windows": "//:bin/llvm-cov.exe",
        "//conditions:default": "//:bin/llvm-cov",
    }),
    data = glob(["bin/llvm"]),
)

# TODO(amontanez): Add sysroot for macos to the `data` field selection once
# Pigweed migrates to use rules_cc toolchains.
native_binary(
    name = "clang-tidy",
    src = select({
        "@platforms//os:windows": "//:bin/clang-tidy.exe",
        "//conditions:default": "//:bin/clang-tidy",
    }),
    out = select({
        "@platforms//os:windows": "clang-tidy.exe",
        "//conditions:default": "clang-tidy",
    }),
    data = glob([
        "include/**",
        "lib/clang/*/include/**",
        "lib/clang/*/share/**",
    ]) + select({
        "@platforms//os:linux": ["@linux_sysroot//:sysroot"],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:public"],
)

pw_py_importable_runfile(
    name = "py_clang_format",
    src = ":bin/clang-format",
    executable = True,
    import_location = "llvm_toolchain.clang_format",
)

#####################
#     Arguments     #
#####################

# None...

#####################
#     Features      #
#####################

# None...

#####################
#     Builtins      #
#####################

subdirectory(
    name = "include-c++-v1",
    parent = ":toolchain_root",
    path = "include/c++/v1",
)

subdirectory(
    name = "lib-clang-include",
    parent = ":toolchain_root",
    path = match_dir(
        ["lib/clang/*/include"],
        allow_empty = False,
    ),
)

subdirectory(
    name = "lib-clang-share",
    parent = ":toolchain_root",
    path = match_dir(
        ["lib/clang/*/share"],
        allow_empty = False,
    ),
)

subdirectory(
    name = "include-x86_64-unknown-linux-gnu-c++-v1",
    parent = ":toolchain_root",
    path = "include/x86_64-unknown-linux-gnu/c++/v1",
)

directory(
    name = "toolchain_root",
    srcs = glob([
        "lib/**",
        "include/**",
    ]),
)

filegroup(
    name = "llvm-libc_files",
    srcs = selects.with_or({
        ALL_CORTEX_M_CPUS: [
            ":llvm-libc_arm-none-eabi_compile_files",
            ":llvm-libc_arm-none-eabi_link_files",
        ],
        ALL_RISCV_CPUS: [
            ":llvm-libc_riscv-unknown-elf_compile_files",
            ":llvm-libc_riscv-unknown-elf_link_files",
        ],
        "//conditions:default": [],
    }),
)

config_setting(
    name = "x86_64-unknown-linux-gnu",
    constraint_values = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
    visibility = ["//visibility:private"],
)

#####################
#     llvm-libc     #
#####################

filegroup(
    name = "llvm-libc_arm-none-eabi_compile_files",
    srcs = glob([
        "include/armv*-unknown-none-eabi/**",
    ]),
    visibility = ["//visibility:public"],
)

filegroup(
    name = "llvm-libc_arm-none-eabi_link_files",
    srcs = glob([
        "lib/armv*-unknown-none-eabi/**",
        "lib/clang/*/lib/armv*-unknown-none-eabi/**",
    ]),
    visibility = ["//visibility:public"],
)

filegroup(
    name = "llvm-libc_riscv-unknown-elf_compile_files",
    srcs = glob([
        "include/riscv*-unknown-unknown-elf/**",
    ]),
    visibility = ["//visibility:public"],
)

filegroup(
    name = "llvm-libc_riscv-unknown-elf_link_files",
    srcs = glob([
        "lib/riscv*-unknown-unknown-elf/**",
        "lib/clang/*/lib/riscv*-unknown-unknown-elf/**",
    ]),
    visibility = ["//visibility:public"],
)

cc_args(
    name = "llvm-libc_link_args",
    actions = ["@rules_cc//cc/toolchains/actions:link_actions"],
    args = selects.with_or({
        ALL_CORTEX_M_CPUS: [
            "-nostdlib++",
            "-nostartfiles",
            "-unwindlib=none",
            "-Wl,-lc++",
            "-Wl,-lm",
        ],
        ALL_RISCV_CPUS: [
            "-nostdlib++",
            "-nostartfiles",
            "-unwindlib=none",
            "-Wl,-lc++",
            "-Wl,-lm",
        ],
        "//conditions:default": [],
    }),
    data = selects.with_or({
        ALL_CORTEX_M_CPUS: [
            ":llvm-libc_arm-none-eabi_link_files",
        ],
        ALL_RISCV_CPUS: [
            ":llvm-libc_riscv-unknown-elf_link_files",
        ],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:private"],
)

cc_args(
    name = "llvm-libc_compile_args",
    actions = ["@rules_cc//cc/toolchains/actions:compile_actions"],
    args = selects.with_or({
        ALL_CORTEX_M_CPUS: [],
        ALL_RISCV_CPUS: [],
        "//conditions:default": [],
    }),
    data = selects.with_or({
        ALL_CORTEX_M_CPUS: [
            ":llvm-libc_arm-none-eabi_compile_files",
        ],
        ALL_RISCV_CPUS: [
            ":llvm-libc_riscv-unknown-elf_compile_files",
        ],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:private"],
)

cc_args_list(
    name = "llvm-libc_args",
    args = [
        ":llvm-libc_compile_args",
        ":llvm-libc_link_args",
    ],
)
