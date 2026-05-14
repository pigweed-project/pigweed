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
"""Rule for interactively running a QEMU system image using qemu_runner."""

load("//pw_kernel/tooling:system_image.bzl", "SystemImageInfo")

def _qemu_run_impl(ctx):
    runner = ctx.actions.declare_file(ctx.label.name + ".sh")
    elf_file = ctx.attr.image[SystemImageInfo].elf

    uart_arg = ""
    if ctx.attr.uart_tcp_port > 0:
        uart_arg = " --uart-tcp-port {}".format(ctx.attr.uart_tcp_port)

    script = """#!/bin/sh
exec "{qemu_runner}" --cpu {cpu} --machine {machine} --semihosting --image "{image}"{uart_arg} "$@"
""".format(
        qemu_runner = ctx.executable._qemu_runner.short_path,
        cpu = ctx.attr.cpu,
        machine = ctx.attr.machine,
        image = elf_file.short_path,
        uart_arg = uart_arg,
    )

    ctx.actions.write(
        output = runner,
        content = script,
        is_executable = True,
    )

    runfiles = ctx.runfiles(files = [elf_file]).merge(
        ctx.attr._qemu_runner[DefaultInfo].default_runfiles,
    )

    return [
        DefaultInfo(
            executable = runner,
            runfiles = runfiles,
        ),
    ]

qemu_runner = rule(
    implementation = _qemu_run_impl,
    executable = True,
    attrs = {
        "cpu": attr.string(
            doc = "QEMU CPU type.",
            mandatory = True,
        ),
        "image": attr.label(
            doc = "The system_image target to run.",
            mandatory = True,
            providers = [SystemImageInfo],
        ),
        "machine": attr.string(
            doc = "QEMU machine type.",
            mandatory = True,
        ),
        "uart_tcp_port": attr.int(
            doc = "Optional TCP port for serial console redirection.",
            default = 0,
        ),
        "_qemu_runner": attr.label(
            default = "@pigweed//pw_kernel/tooling:qemu",
            executable = True,
            cfg = "target",
        ),
    },
    doc = "Runs a QEMU hardware target interactively.",
)
