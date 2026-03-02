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
"""A rule for constructing a pw_kernel system image consisting of the kernel
and any apps.
"""

load("@bazel_tools//tools/build_defs/cc:action_names.bzl", "ACTION_NAMES")
load("@bazel_tools//tools/cpp:toolchain_utils.bzl", "use_cpp_toolchain")
load("//pw_build:binary_tools.bzl", "run_action_on_executable")

def _target_transition_impl(_, attr):
    flags = {
        "//command_line_option:platforms": str(attr.platform),
        str(Label("//pw_kernel/target:system_config_file")): str(attr.system_config),
        str(Label("//pw_kernel/userspace:userspace_build")): attr.user_space,
    }

    return flags

_target_transition = transition(
    implementation = _target_transition_impl,
    inputs = [],
    outputs = [
        "//command_line_option:platforms",
        str(Label("//pw_kernel/target:system_config_file")),
        str(Label("//pw_kernel/userspace:userspace_build")),
    ],
)

def _app_target_transition_impl(_, attr):
    flags = {
        "//command_line_option:platforms": str(attr.platform),
        str(Label("//pw_kernel/target:system_config_file")): str(attr.system_config),
        str(Label("//pw_kernel/userspace:userspace_build")): True,
        str(Label("//pw_kernel/userspace:is_app_build")): True,
    }

    return flags

_app_target_transition = transition(
    implementation = _app_target_transition_impl,
    inputs = [],
    outputs = [
        "//command_line_option:platforms",
        str(Label("//pw_kernel/target:system_config_file")),
        str(Label("//pw_kernel/userspace:userspace_build")),
        str(Label("//pw_kernel/userspace:is_app_build")),
    ],
)

SystemImageInfo = provider(
    "Metadata about a system image",
    fields = {
        "bin": "[File] Path to the bin file",
        "elf": "[File] Path to the compiled elf file",
    },
)

def _system_image_impl(ctx):
    output_elf = ctx.actions.declare_file(ctx.attr.name + ".elf")

    args = [
        "--kernel",
        ctx.file.kernel.path,
        "--output",
        output_elf.path,
    ]
    for f in ctx.files.apps:
        args.append("--app")
        args.append(f.path)

    ctx.actions.run(
        inputs = ctx.files.kernel + ctx.files.apps,
        outputs = [output_elf],
        executable = ctx.executable._system_assembler,
        mnemonic = "SystemImageAssembler",
        arguments = args,
    )

    output_bin = ctx.actions.declare_file(ctx.attr.name + ".bin")
    run_action_on_executable(
        ctx = ctx,
        # TODO: https://github.com/bazelbuild/rules_cc/issues/292 - Add a helper
        # to rules cc to make it possible to get this from ctx.attr._objcopy.
        action_name = ACTION_NAMES.objcopy_embed_data,
        action_args = "{args} {input} {output}".format(
            args = "-Obinary",
            input = output_elf.path,
            output = output_bin.path,
        ),
        inputs = [output_elf],
        output = output_bin,
        additional_outputs = [],
        output_executable = True,
    )

    return [
        DefaultInfo(executable = output_elf, files = depset([output_elf, output_bin])),
        SystemImageInfo(bin = output_bin, elf = output_elf),
    ]

system_image = rule(
    implementation = _system_image_impl,
    executable = True,
    attrs = {
        "apps": attr.label_list(
            doc = "List of application images.",
            cfg = _app_target_transition,
        ),
        "kernel": attr.label(
            doc = "Kernel image.",
            allow_single_file = True,
            mandatory = True,
            cfg = _target_transition,
        ),
        "platform": attr.label(
            doc = "Bazel platform to compile image for.",
            mandatory = True,
        ),
        "system_config": attr.label(
            doc = "Optional System config file which defines the system.",
            allow_single_file = True,
        ),
        "user_space": attr.bool(
            doc = "Whether to include user space support in the kernel.",
            default = True,
        ),
        "_system_assembler": attr.label(
            executable = True,
            cfg = "exec",
            default = "//pw_kernel/tooling:system_assembler",
        ),
    },
    toolchains = use_cpp_toolchain(),
    fragments = ["cpp"],
    doc = "Assemble a system image combining apps and kernel.",
)

def _system_image_test_impl(ctx):
    executable_symlink = ctx.actions.declare_file(ctx.label.name)
    ctx.actions.symlink(
        output = executable_symlink,
        target_file = ctx.attr.image[SystemImageInfo].elf,
    )

    return [
        DefaultInfo(
            executable = executable_symlink,
            runfiles = ctx.attr.image[DefaultInfo].default_runfiles,
        ),
    ]

system_image_test = rule(
    implementation = _system_image_test_impl,
    test = True,
    attrs = {
        "image": attr.label(
            doc = "The system_image target to test.",
            mandatory = True,
            providers = [DefaultInfo],
            executable = True,
            cfg = "target",
        ),
    },
    doc = "Defines a test for a system_image target.",
)
