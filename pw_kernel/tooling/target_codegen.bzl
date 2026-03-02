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
"""A rule for generating rust code to initialize the processes and threads etc
for a system.
"""

load("@rules_rust//rust:defs.bzl", "rust_library")

def _target_codegen_impl(ctx):
    output = ctx.actions.declare_file(ctx.attr.name + ".rs")
    args = []
    for (name, path) in ctx.attr.templates.items():
        args.append("--template")
        args.append(name + "=" + path.files.to_list()[0].path)

    args = args + [
        "--config",
        ctx.file.system_config.path,
        "--output",
        output.path,
        "--userspace",
        str(ctx.attr.user_space).lower(),
        "render-target-template",
    ]

    ctx.actions.run(
        inputs = ctx.files.system_config + ctx.files.templates,
        outputs = [output],
        executable = ctx.executable.system_generator,
        mnemonic = "CodegenSystem",
        arguments = args,
    )

    return [DefaultInfo(files = depset([output]))]

_target_codegen_rule = rule(
    implementation = _target_codegen_impl,
    attrs = {
        "system_config": attr.label(
            doc = "System config file which defines the system.",
            allow_single_file = True,
            mandatory = True,
        ),
        "system_generator": attr.label(
            executable = True,
            cfg = "exec",
        ),
        "templates": attr.string_keyed_label_dict(
            allow_files = True,
        ),
        "user_space": attr.bool(
            doc = "Whether to include user space support in the kernel.",
            default = True,
        ),
    },
    doc = "Codegen system sources from system config",
)

def target_codegen(
        name,
        system_config,
        arch,
        deps = [],
        system_generator = "@pigweed//pw_kernel/tooling/system_generator:system_generator_bin",
        templates = {
            "interrupts": "@pigweed//pw_kernel/tooling/system_generator/templates:interrupts.rs.jinja",
            "object_channel_handler": "@pigweed//pw_kernel/tooling/system_generator/templates/objects:channel_handler.rs.jinja",
            "object_channel_initiator": "@pigweed//pw_kernel/tooling/system_generator/templates/objects:channel_initiator.rs.jinja",
            "object_interrupt": "@pigweed//pw_kernel/tooling/system_generator/templates/objects:interrupt.rs.jinja",
            "object_wait_group": "@pigweed//pw_kernel/tooling/system_generator/templates/objects:wait_group.rs.jinja",
            "system": "@pigweed//pw_kernel/tooling/system_generator/templates:system.rs.jinja",
        },
        **kwargs):
    """Generated code crate.

    Args:
        name: The name of the target.
        system_config: System config file which defines the system.
        arch: The target architecture crate,
        deps: A list of Rust dependencies.
        system_generator: The code generator executable.
        templates: A dictionary of templates for the code generator.
        **kwargs: Other attributes (like `visibility`) passed to both the
            `rust_library` and the internal codegen rule.
    """

    codegen_target_name = name + "_codegen"

    # The codegen target should only ever be built as a dependency
    # and never as part of a wildcard build, as it won't have the full
    # set of required configuration flags.
    tags = kwargs.get("tags", default = [])
    tags.append("manual")
    kwargs["tags"] = tags

    _target_codegen_rule(
        name = codegen_target_name,
        system_config = system_config,
        system_generator = system_generator,
        templates = templates,
        user_space = select({
            "@pigweed//pw_kernel/userspace:userspace_build_enabled": True,
            "//conditions:default": False,
        }),
        **kwargs
    )

    rust_library(
        name = name,
        srcs = [":" + codegen_target_name],
        edition = "2024",
        crate_features = select({
            "@pigweed//pw_kernel/userspace:userspace_build_enabled": ["user_space"],
            "//conditions:default": [],
        }) + kwargs.pop("crate_features", []),
        deps = deps + [arch] + [
            "@pigweed//pw_kernel/kernel",
            "@pigweed//pw_kernel/lib/foreign_box",
            "@pigweed//pw_kernel/lib/memory_config",
            "@pigweed//pw_kernel/syscall:syscall_defs",
            "@pigweed//pw_log/rust:pw_log",
        ],
        **kwargs
    )
