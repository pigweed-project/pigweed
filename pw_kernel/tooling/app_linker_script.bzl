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
"""A rule for generating the application linker script based on the system
configuration file.
"""

load("@bazel_skylib//lib:selects.bzl", "selects")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load("//pw_kernel/arch/arm_cortex_m:defs.bzl", "SUPPORTED_CORTEX_M_CPUS")

def _app_linker_script_impl(ctx):
    output = ctx.actions.declare_file(ctx.attr.name + ".ld")

    args = [
        "--template",
        "app=" + ctx.file.template.path,
        "--template",
        "pigweed_linker_sections.ld.jinja=" + ctx.file.pigweed_sections.path,
        "--config",
        ctx.file.system_config.path,
        "--output",
        output.path,
        "render-app-template",
        "--app-name",
        ctx.attr.app_name,
    ]

    ctx.actions.run(
        inputs = ctx.files.system_config + [ctx.file.template, ctx.file.pigweed_sections],
        outputs = [output],
        executable = ctx.executable.system_generator,
        mnemonic = "AppLinkerScript",
        arguments = args,
    )

    linker_input = cc_common.create_linker_input(
        owner = ctx.label,
        user_link_flags = ["-T", output.path],
        additional_inputs = depset(direct = [output]),
    )
    linking_context = cc_common.create_linking_context(
        linker_inputs = depset(direct = [linker_input]),
    )

    return [
        DefaultInfo(files = depset([output])),
        CcInfo(linking_context = linking_context),
    ]

_app_linker_script_rule = rule(
    implementation = _app_linker_script_impl,
    attrs = {
        "app_name": attr.string(
            doc = "The name of the app for this linker script.",
            mandatory = True,
        ),
        "pigweed_sections": attr.label(
            doc = "Pigweed linker sections template file.",
            allow_single_file = True,
            default = "//pw_kernel/tooling/system_generator/templates:pigweed_linker_sections.ld.jinja",
        ),
        "system_config": attr.label(
            doc = "System config file which defines the system.",
            allow_single_file = True,
            default = "//pw_kernel/target:system_config_file",
        ),
        "system_generator": attr.label(
            executable = True,
            cfg = "exec",
            default = "@pigweed//pw_kernel/tooling/system_generator:system_generator_bin",
        ),
        "template": attr.label(
            doc = "Application linker script template file.",
            allow_single_file = True,
            mandatory = True,
        ),
    },
    doc = "Generate the linker script for an app or multi-app binary based on the system config.",
)

def _set_linker_script_defaults(kwargs):
    if kwargs.get("target_compatible_with") == None:
        kwargs["target_compatible_with"] = select({
            "@pigweed//pw_kernel/target:system_config_not_set": ["@platforms//:incompatible"],
            "//conditions:default": [],
        })

    if kwargs.get("template") == None:
        template = selects.with_or({
            SUPPORTED_CORTEX_M_CPUS: "@pigweed//pw_kernel/tooling/system_generator/templates:cortex_m_app.ld.jinja",
            "@platforms//cpu:riscv32": "@pigweed//pw_kernel/tooling/system_generator/templates:riscv_app.ld.jinja",
            "//conditions:default": None,
        })
        kwargs["template"] = template

def app_linker_script(name, app_name, system_config = None, **kwargs):
    """Wrapper function to set default platform specific arguments for the linker script rule.

    Args:
        name: The name of the target.
        system_config: System config file which defines the system.
        app_name: The name of the application.
        **kwargs: Other attributes passed to the underlying rule.
    """
    _set_linker_script_defaults(kwargs)

    _app_linker_script_rule(
        name = name,
        system_config = system_config,
        app_name = app_name,
        **kwargs
    )
