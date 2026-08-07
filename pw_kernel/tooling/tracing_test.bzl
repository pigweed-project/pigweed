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
"""Rule for running a tracing test for a system image."""

load("@com_google_protobuf//bazel/common:proto_info.bzl", "ProtoInfo")
load("//pw_kernel/tooling:system_image.bzl", "SystemImageInfo")

def _qemu_tracing_test_impl(ctx):
    runner = ctx.actions.declare_file(ctx.label.name + ".sh")
    elf_file = ctx.attr.image[SystemImageInfo].elf
    golden_file = ctx.file.golden_file

    # Get the descriptor set for trace_proto
    proto_info = ctx.attr._trace_proto_definition[ProtoInfo]
    descriptor_sets = proto_info.transitive_descriptor_sets.to_list()

    # We assume the first one is the one we need (since trace_proto is self-contained)
    trace_proto_descriptor = descriptor_sets[0]

    ws = ctx.workspace_name if ctx.workspace_name else "_main"
    if elf_file.short_path.startswith("../"):
        image_path = elf_file.short_path[3:]
    else:
        image_path = ws + "/" + elf_file.short_path

    if golden_file.short_path.startswith("../"):
        golden_path = golden_file.short_path[3:]
    else:
        golden_path = ws + "/" + golden_file.short_path

    if trace_proto_descriptor.short_path.startswith("../"):
        trace_proto_descriptor_path = trace_proto_descriptor.short_path[3:]
    else:
        trace_proto_descriptor_path = ws + "/" + trace_proto_descriptor.short_path

    # Construct the output path at runtime. If TEST_UNDECLARED_OUTPUTS_DIR
    # is set (standard for `bazel test`), we write there so Bazel collects it.
    # Otherwise, we fall back to a local path.
    script = """#!/bin/sh
if [ -n "$TEST_UNDECLARED_OUTPUTS_DIR" ]; then
  OUTPUT_FILE="$TEST_UNDECLARED_OUTPUTS_DIR/{trace_file}"
else
  OUTPUT_FILE="{trace_file}"
fi

exec "{test_binary}" \
  --cpu "{cpu}" \
  --machine "{machine}" \
  --image "{image}" \
  --output-file "$OUTPUT_FILE" \
  --golden-file "{golden_file}" \
  --trace-proto-descriptor "{trace_proto_descriptor}" \
  "$@"
""".format(
        test_binary = ctx.executable._test_binary.short_path,
        cpu = ctx.attr.cpu,
        machine = ctx.attr.machine,
        image = image_path,
        trace_file = ctx.attr.trace_file,
        golden_file = golden_path,
        trace_proto_descriptor = trace_proto_descriptor_path,
    )

    ctx.actions.write(
        output = runner,
        content = script,
        is_executable = True,
    )

    runfiles = ctx.runfiles(files = [elf_file, golden_file, trace_proto_descriptor]).merge(
        ctx.attr._test_binary[DefaultInfo].default_runfiles,
    )

    return [
        DefaultInfo(
            executable = runner,
            runfiles = runfiles,
        ),
    ]

_qemu_tracing_test = rule(
    implementation = _qemu_tracing_test_impl,
    test = True,
    attrs = {
        "cpu": attr.string(
            doc = "QEMU CPU type.",
            mandatory = True,
        ),
        "golden_file": attr.label(
            doc = "The path for the trace file to match the output against.",
            allow_single_file = True,
            mandatory = True,
        ),
        "image": attr.label(
            doc = "The system_image target to test.",
            mandatory = True,
            providers = [SystemImageInfo],
        ),
        "machine": attr.string(
            doc = "QEMU machine type.",
            mandatory = True,
        ),
        "trace_file": attr.string(
            doc = "The name for the output trace file.",
        ),
        "_test_binary": attr.label(
            default = "@pigweed//pw_kernel/tooling:qemu_tracing_test_runner",
            executable = True,
            cfg = "target",
        ),
        "_trace_proto_definition": attr.label(
            default = "//third_party/perfetto:trace_proto",
            providers = [ProtoInfo],
        ),
    },
    doc = "Runs the tracing test for a system_image target.",
)

def qemu_tracing_test(name, **kwargs):
    trace_file = kwargs.pop("trace_file", name + ".pb")

    _qemu_tracing_test(
        name = name,
        trace_file = trace_file,
        **kwargs
    )
