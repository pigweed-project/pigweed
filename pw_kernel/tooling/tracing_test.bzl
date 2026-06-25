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

load("//pw_kernel/tooling:system_image.bzl", "SystemImageInfo")

def _qemu_tracing_test_impl(ctx):
    runner = ctx.actions.declare_file(ctx.label.name + ".sh")
    elf_file = ctx.attr.image[SystemImageInfo].elf

    ws = ctx.workspace_name if ctx.workspace_name else "_main"
    if elf_file.short_path.startswith("../"):
        image_path = elf_file.short_path[3:]
    else:
        image_path = ws + "/" + elf_file.short_path

    # Construct the output path at runtime. If TEST_UNDECLARED_OUTPUTS_DIR
    # is set (standard for `bazel test`), we write there so Bazel collects it.
    # Otherwise, we fall back to a local path.
    script = """#!/bin/sh
if [ -n "$TEST_UNDECLARED_OUTPUTS_DIR" ]; then
  OUTPUT_FILE="$TEST_UNDECLARED_OUTPUTS_DIR/{trace_file}"
else
  OUTPUT_FILE="{trace_file}"
fi

exec "{test_binary}" --cpu "{cpu}" --machine "{machine}" --image "{image}" --output-file "$OUTPUT_FILE" "$@"
""".format(
        test_binary = ctx.executable._test_binary.short_path,
        cpu = ctx.attr.cpu,
        machine = ctx.attr.machine,
        image = image_path,
        trace_file = ctx.attr.trace_file,
    )

    ctx.actions.write(
        output = runner,
        content = script,
        is_executable = True,
    )

    runfiles = ctx.runfiles(files = [elf_file]).merge(
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
