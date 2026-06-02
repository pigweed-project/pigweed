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
"""Rule to validate a rust binary contains no panics.
"""

def _rust_binary_no_panics_test_impl(ctx):
    binary = ctx.files.binary[0]
    run_script = ctx.actions.declare_file("%s.sh" % ctx.label.name)

    ctx.actions.write(run_script, "#!/bin/bash\n{panic_detector} --image {image}\n".format(
        panic_detector = ctx.executable._panic_detector.short_path,
        image = binary.short_path,
    ))

    # When running no_panic tests, some bazel configs will use
    # --run_under="//pw_kernel/tooling:qemu.
    # Set this env var to tell the qemu wrapper script to invoke
    # the host binary directly and not try to run under qemu.
    env = dict(ctx.attr.env)
    env["PW_RUNNER_PASSTHROUGH"] = "1"

    return [
        DefaultInfo(
            files = depset([run_script]),
            runfiles = ctx.runfiles([
                ctx.executable._panic_detector,
                binary,
            ]),
            executable = run_script,
        ),
        RunEnvironmentInfo(
            environment = env,
        ),
    ]

_rust_binary_no_panics_test = rule(
    implementation = _rust_binary_no_panics_test_impl,
    test = True,
    attrs = {
        "binary": attr.label(
            mandatory = True,
        ),
        "env": attr.string_dict(
            doc = "Environment variables to set for the test",
            default = {},
        ),
        "_panic_detector": attr.label(
            executable = True,
            cfg = "exec",
            default = "//pw_kernel/tooling/panic_detector:panic_detector_tool",
        ),
    },
    doc = "Check whether the rust binary contains any panics.",
)

def rust_binary_no_panics_test(name, binary, **kwargs):
    """Check whether the rust binary contains any panics.

    Args:
        name: Name of the target
        binary: Target to check for panics
        **kwargs: Args to pass through to the underlying rule.
    """
    _rust_binary_no_panics_test(
        name = name,
        binary = binary,
        **kwargs
    )
