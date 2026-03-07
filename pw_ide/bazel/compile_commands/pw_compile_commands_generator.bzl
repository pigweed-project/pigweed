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
"""Build rules to generate fixed compile command patterns."""

_CompileCommandsTargetPatternsInfo = provider(
    "A set of target patterns used for compile command collection for a single platform",
    fields = {
        "patterns": "(depset[str]) Bazel target patterns that will be used to evaluate these patterns",
        "platform": "(label) Platform that will be used to evaluate the requested target patterns",
    },
)

_CompileCommandsGroupInfo = provider(
    "A group of target patterns used for compile command collection for multiple platform",
    fields = {
        "platform_patterns": "(list[_CompileCommandsTargetPatternsInfo])",
    },
)

def _resolve_target_pattern(target_pattern):
    """Converts a string that may contain wildcards to its canonical form."""
    is_subtracted = target_pattern.startswith("-")
    target_pattern = target_pattern.lstrip("-")
    return "{}{}".format(
        "-" if is_subtracted else "",
        _resolve_target_pattern_base(target_pattern),
    )

def _resolve_target_pattern_base(target_pattern):
    if not target_pattern.endswith("..."):
        return native.package_relative_label(target_pattern)

    if target_pattern == "...":
        return str(native.package_relative_label(":all")).replace(":all", "/...")

    # Two cases to handle:
    #   //...
    #   //foo_package/...
    wildcard_token = "..." if target_pattern.endswith("//...") else "/..."
    fake_target_name = ":_pw_internal_fake_target_name"
    absolute_pattern = target_pattern.replace(
        wildcard_token,
        fake_target_name,
    )
    return str(native.package_relative_label(absolute_pattern)).replace(
        fake_target_name,
        wildcard_token,
    )

def _collect_target_patterns(ctx):
    """Builds a _CompileCommandsGroupInfo from the current context.

    For all dependencies, merges the target_patterns across all the
    deps, and then merges that with the target_patterns introduced by
    the requesting rule.
    """
    patterns_by_platform = {}

    if ctx.attr.platform:
        platform_label = ctx.attr.platform
        patterns_by_platform[platform_label] = depset(
            ctx.attr.target_patterns,
        )

    for dep in ctx.attr.deps:
        if _CompileCommandsGroupInfo in dep:
            for platform_patterns in dep[_CompileCommandsGroupInfo].platform_patterns:
                platform_label = platform_patterns.platform
                if platform_label not in patterns_by_platform:
                    patterns_by_platform[platform_label] = depset()
                patterns_by_platform[platform_label] = depset(
                    transitive = [
                        patterns_by_platform[platform_label],
                        platform_patterns.patterns,
                    ],
                )

    platform_patterns_list = []
    for platform, patterns in patterns_by_platform.items():
        platform_patterns_list.append(
            _CompileCommandsTargetPatternsInfo(
                platform = platform,
                patterns = patterns,
            ),
        )

    return _CompileCommandsGroupInfo(
        platform_patterns = platform_patterns_list,
    )

def _target_patterns_to_json(compile_commands_patterns):
    """Takes a _CompileCommandsGroupInfo provider and converts it JSON.

    The expected format is dictated by the merger.py script, but the rough
    format is as follows:

    {
        "compile_commands_patterns": [
            {
                "platform": "//platforms/my_device",
                "target_patterns": [
                    "//foo/...",
                    "//bar/..."
                ]
            },
            {
                "platform": "//platforms/host",
                "target_patterns": [
                    "//foo/...",
                    "//bar/..."
                ]
            }
        ]
    }
    """
    output_patterns = []
    for platform_pattern in compile_commands_patterns.platform_patterns:
        output_patterns.append({
            "platform": str(platform_pattern.platform),
            "target_patterns": platform_pattern.patterns.to_list(),
        })

    return json.encode_indent({
        "compile_commands_patterns": output_patterns,
    }, indent = "  ")

def _pw_compile_commands_generator_impl(ctx):
    compile_commands_patterns = _collect_target_patterns(ctx)
    compile_command_config_json = _target_patterns_to_json(compile_commands_patterns)

    target_patterns_file = ctx.outputs.config_out
    ctx.actions.write(
        output = target_patterns_file,
        content = compile_command_config_json,
    )

    out = ctx.actions.declare_file(ctx.attr.name + ".exe")
    ctx.actions.symlink(
        target_file = ctx.executable._updater,
        output = out,
        is_executable = True,
    )
    runfiles = ctx.runfiles(
        files = [target_patterns_file],
    )

    runfiles = runfiles.merge(ctx.attr._updater[DefaultInfo].default_runfiles)

    return [
        DefaultInfo(
            executable = out,
            files = depset([target_patterns_file]),
            runfiles = runfiles,
        ),
        compile_commands_patterns,
    ]

_pw_compile_commands_generator = rule(
    implementation = _pw_compile_commands_generator_impl,
    attrs = {
        "config_out": attr.output(mandatory = True),
        "deps": attr.label_list(providers = [_CompileCommandsGroupInfo]),
        "platform": attr.string(),
        "target_patterns": attr.string_list(),
        "_updater": attr.label(default = Label("//pw_ide/bazel:update_compile_commands"), executable = True, cfg = "target"),
    },
    executable = True,
)

def pw_compile_commands_generator(name, target_patterns = None, deps = None, platform = None, **kwargs):
    """A rule that can be used to build a compile command database.

    This rule can be `bazel run` to generate a compile command database at
    the root of the workspace.

    Args:
      name: The name of this target.
      target_patterns: A list of target patterns that will be included in this
        compile command database.
      deps: A list of other `pw_compile_commands_generator` targets to include
        in this database.
      platform: The platform to use to evaluate the provided `target_patterns`.
      **kwargs: Extra arguments to pass to the underlying `native_binary` rule.
    """
    if target_patterns == None:
        target_patterns = []
    if deps == None:
        deps = []

    args = [
        "--compile-command-groups",
        "$(rootpath :{}_target_patterns.json)".format(name),
    ]

    _pw_compile_commands_generator(
        name = name,
        deps = deps,
        target_patterns = [
            _resolve_target_pattern(pattern)
            for pattern in target_patterns
        ],
        config_out = "{}_target_patterns.json".format(name),
        args = args,
        # Don't follow aliases, they technically mean different things from
        # a configuration perspective.
        platform = str(native.package_relative_label(platform)) if platform else None,
        tags = kwargs.pop("tags", []) + ["pw_compile_commands_generator"],
        **kwargs
    )
