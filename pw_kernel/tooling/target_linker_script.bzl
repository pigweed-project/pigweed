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
"""A rule for generating the system linker script based on the system
configuration file.
"""

load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

def _target_linker_script_impl(ctx):
    output = ctx.actions.declare_file(ctx.attr.name + ".ld")

    args = [
        "--template",
        "system=" + ctx.file.template.path,
        "--template",
        "pigweed_linker_sections.ld.jinja=" + ctx.file.pigweed_sections.path,
        "--config",
        ctx.file.system_config.path,
        "--output",
        output.path,
        "render-target-template",
    ]

    ctx.actions.run(
        inputs = ctx.files.system_config + [ctx.file.template, ctx.file.pigweed_sections],
        outputs = [output],
        executable = ctx.executable.system_generator,
        mnemonic = "SystemLinkerScript",
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

target_linker_script = rule(
    implementation = _target_linker_script_impl,
    attrs = {
        "pigweed_sections": attr.label(
            doc = "Pigweed linker sections template file.",
            allow_single_file = True,
            default = "//pw_kernel/tooling/system_generator/templates:pigweed_linker_sections.ld.jinja",
        ),
        "system_config": attr.label(
            doc = "System config file which defines the system.",
            allow_single_file = True,
            mandatory = True,
        ),
        "system_generator": attr.label(
            executable = True,
            cfg = "exec",
            default = "//pw_kernel/tooling/system_generator:system_generator_bin",
        ),
        "template": attr.label(
            doc = "System linker script template file.",
            allow_single_file = True,
            mandatory = True,
        ),
    },
    doc = "Generate the system linker script based on the system config.",
)
