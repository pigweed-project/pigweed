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

"""Rules for creating runnable accessors to toolchain tools.

This module provides the `pw_cc_tool_for_action` rule, which locates a tool by
its *action name* (e.g. `objdump-disassemble`). This is the preferred way to
access toolchain binaries as it ensures the tool used matches the one Bazel uses
during the build.
"""

load(
    "@bazel_tools//tools/cpp:toolchain_utils.bzl",
    "find_cpp_toolchain",
    "use_cpp_toolchain",
)
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")

# This is easy to misuse. Don't expose publicly for now.
visibility("private")

def _pw_cc_tool_for_action_impl(ctx):
    cc_toolchain = find_cpp_toolchain(ctx)
    feature_configuration = cc_common.configure_features(
        cc_toolchain = cc_toolchain,
        ctx = ctx,
        requested_features = ctx.features,
        unsupported_features = ctx.disabled_features,
    )

    action_name = ctx.attr.action_name

    # Check if the action is actually supported by the current toolchain.
    if not cc_common.action_is_enabled(
        action_name = action_name,
        feature_configuration = feature_configuration,
    ):
        # If the action is not supported, generate a script that fails with a
        # helpful error message when run. This allows the target to exist (so
        # builds and queries don't break) but fail if someone tries to use it.
        fail_script = ctx.actions.declare_file(ctx.label.name)
        ctx.actions.write(
            content = (
                'echo "Error: The active toolchain does not support the ' +
                "'{action}' action.\"\n" +
                'echo "This usually means the toolchain is missing this tool ' +
                '(e.g. llvm-cov) or does not have it mapped to this action."\n' +
                "exit 1\n"
            ).format(action = action_name),
            is_executable = True,
            output = fail_script,
        )
        return [DefaultInfo(executable = fail_script)]

    tool_path = cc_common.get_tool_for_action(
        action_name = action_name,
        feature_configuration = feature_configuration,
    )

    all_files = cc_toolchain.all_files.to_list()
    tool_file = None

    # Find the File object matching the path returned by get_tool_for_action.
    for file in all_files:
        if file.path == tool_path:
            tool_file = file
            break

    if not tool_file:
        fail("Could not find tool for action: %s (path: %s)" % (
            action_name,
            tool_path,
        ))

    script = ctx.actions.declare_file(ctx.label.name)
    ctx.actions.symlink(
        is_executable = True,
        output = script,
        target_file = tool_file,
    )

    return DefaultInfo(
        executable = script,
        files = depset(
            direct = [script, tool_file],
            transitive = [cc_toolchain.all_files],
        ),
        runfiles = ctx.runfiles(
            files = [script, tool_file],
            transitive_files = cc_toolchain.all_files,
        ),
    )

pw_cc_tool_for_action = rule(
    implementation = _pw_cc_tool_for_action_impl,
    attrs = {
        "action_name": attr.string(mandatory = True),
    },
    doc = """Exposes the C/C++ toolchain tool for the specified action as a
runnable binary.

This is intended exclusively to create `bazel run`-able targets for interactive
or debugging purposes.

WARNING: This should never be used as a dependency of another rule, as it
interacts with target/exec configurations in confusing ways. If used behind
`cfg="target"`, a rule might get a tool that is not `exec_compatible_with` the
executor of the consuming rule. If used behind `cfg="exec"`, a rule will always
get the version of the tool needed to build binaries for the exec platform of
the executor of the consuming rule (i.e. you'll never get an MCU-targeted
toolchain).

These interactions are less catastrophic when only a single execution platform
is registered, but still confusing.
""",
    executable = True,
    fragments = ["cpp"],
    toolchains = use_cpp_toolchain(),
)
