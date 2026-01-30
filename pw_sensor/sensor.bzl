# Copyright 2022 The Pigweed Authors
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
"""A rule for translating pw_sensor YAML definitions into C++.
"""

load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

def _pw_sensor_library_impl(ctx):
    out_header = ctx.actions.declare_file(ctx.attr.out_header)
    sources = ctx.files.srcs
    inputs = ctx.files.inputs

    sensor_desc_target = ctx.attr._sensor_desc[DefaultInfo]
    generator_target = ctx.attr.generator[DefaultInfo]

    args = []

    # Add include paths for the generator
    for generator_include in ctx.attr.generator_includes:
        args.extend(["-I", generator_include])

    # Add the generator and args
    args.extend([
        "-g",
        (
            "python3 " + generator_target.files_to_run.executable.path + " " +
            " ".join(ctx.attr.generator_args)
        ),
    ])

    # Add the output file
    args.extend(["-o", out_header.path])

    for source in sources:
        args.append(source.path)

    # Run the generator
    ctx.actions.run(
        inputs = (
            sources + inputs + generator_target.files.to_list() +
            sensor_desc_target.files.to_list()
        ),
        outputs = [out_header],
        executable = sensor_desc_target.files_to_run.executable,
        arguments = args,
        mnemonic = "SensorCodeGen",
        progress_message = "Generating " + out_header.path,
    )

    # Get compilation contexts from dependencies
    dep_compilation_contexts = [
        dep[CcInfo].compilation_context
        for dep in ctx.attr.deps
    ]
    compilation_context = cc_common.create_compilation_context(
        includes = depset([ctx.bin_dir.path]),
        headers = depset([out_header]),
    )

    # Return a library made up of the generated header
    return [
        DefaultInfo(files = depset([out_header])),
        CcInfo(
            compilation_context = cc_common.merge_compilation_contexts(
                compilation_contexts = dep_compilation_contexts + [compilation_context],
            ),
        ),
    ]

pw_sensor_library = rule(
    implementation = _pw_sensor_library_impl,
    attrs = {
        "deps": attr.label_list(),
        "generator": attr.label(
            default = str(Label("//pw_sensor/py:constants_generator")),
            executable = True,
            cfg = "exec",
        ),
        "generator_args": attr.string_list(
            default = ["--package", "pw.sensor"],
        ),
        "generator_includes": attr.string_list(),
        "inputs": attr.label_list(allow_files = True),
        "out_header": attr.string(),
        "srcs": attr.label_list(allow_files = True),
        "_sensor_desc": attr.label(
            default = str(Label("//pw_sensor/py:sensor_desc")),
            executable = True,
            cfg = "exec",
        ),
    },
    provides = [CcInfo],
)
