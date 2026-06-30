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
        "config": "(str) The Bazel config name to use for compilation and platform inference",
        "display_name": "(str) Optional display name for the platform",
        "patterns": "(depset[str]) Bazel target patterns that will be used to evaluate these patterns",
        "platform": "(label) Platform that will be used to evaluate the requested target patterns",
        "rust_patterns": "(depset[str]) Bazel target patterns used to evaluate Rust code intelligence",
    },
)

_CompileCommandsGroupInfo = provider(
    "A group of target patterns used for compile command collection for multiple platforms",
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

def _collect_cc_target_patterns(ctx, platform_key):
    patterns_by_platform = {}
    if platform_key:
        patterns_by_platform[platform_key] = depset(
            ctx.attr.target_patterns,
        )

    for dep in ctx.attr.deps:
        if _CompileCommandsGroupInfo in dep:
            for platform_patterns in dep[_CompileCommandsGroupInfo].platform_patterns:
                dep_key = platform_patterns.platform if platform_patterns.platform else platform_patterns.config
                if not dep_key:
                    continue
                if dep_key not in patterns_by_platform:
                    patterns_by_platform[dep_key] = depset()
                patterns_by_platform[dep_key] = depset(
                    transitive = [
                        patterns_by_platform[dep_key],
                        platform_patterns.patterns,
                    ],
                )
    return patterns_by_platform

def _collect_rust_target_patterns(ctx, platform_key):
    rust_patterns_by_platform = {}
    if platform_key:
        rust_patterns_by_platform[platform_key] = depset(
            ctx.attr.rust_target_patterns,
        )

    for dep in ctx.attr.deps:
        if _CompileCommandsGroupInfo in dep:
            for platform_patterns in dep[_CompileCommandsGroupInfo].platform_patterns:
                dep_key = platform_patterns.platform if platform_patterns.platform else platform_patterns.config
                if not dep_key:
                    continue
                if dep_key not in rust_patterns_by_platform:
                    rust_patterns_by_platform[dep_key] = depset()
                rust_patterns_by_platform[dep_key] = depset(
                    transitive = [
                        rust_patterns_by_platform[dep_key],
                        platform_patterns.rust_patterns,
                    ],
                )
    return rust_patterns_by_platform

def _collect_platform_metadata(ctx, platform_key):
    configs_by_platform = {}
    display_names_by_platform = {}
    all_keys = {}
    if platform_key:
        all_keys[platform_key] = True
        if ctx.attr.config:
            configs_by_platform[platform_key] = ctx.attr.config
        if hasattr(ctx.attr, "display_name") and ctx.attr.display_name:
            display_names_by_platform[platform_key] = ctx.attr.display_name

    for dep in ctx.attr.deps:
        if _CompileCommandsGroupInfo in dep:
            for platform_patterns in dep[_CompileCommandsGroupInfo].platform_patterns:
                dep_key = platform_patterns.platform if platform_patterns.platform else platform_patterns.config
                if not dep_key:
                    continue
                all_keys[dep_key] = True
                if not configs_by_platform.get(dep_key):
                    configs_by_platform[dep_key] = platform_patterns.config
                if platform_patterns.display_name:
                    if dep_key in display_names_by_platform:
                        if display_names_by_platform[dep_key] != platform_patterns.display_name:
                            fail("Conflicting display names for platform {}: found '{}' and '{}'".format(
                                dep_key,
                                display_names_by_platform[dep_key],
                                platform_patterns.display_name,
                            ))
                    else:
                        display_names_by_platform[dep_key] = platform_patterns.display_name
    return all_keys, configs_by_platform, display_names_by_platform

def _collect_target_patterns(ctx):
    """Builds a _CompileCommandsGroupInfo from the current context.

    For all dependencies, merges the target_patterns across all the
    deps, and then merges that with the target_patterns introduced by
    the requesting rule.
    """
    platform_key = None
    if ctx.attr.platform:
        platform_key = ctx.attr.platform
    elif ctx.attr.config:
        platform_key = ctx.attr.config

    all_keys, configs_by_platform, display_names_by_platform = _collect_platform_metadata(ctx, platform_key)
    patterns_by_platform = _collect_cc_target_patterns(ctx, platform_key)
    rust_patterns_by_platform = _collect_rust_target_patterns(ctx, platform_key)

    platform_patterns_list = []
    for key in all_keys:
        is_platform = key.startswith("//") or key.startswith("@")
        platform_patterns_list.append(
            _CompileCommandsTargetPatternsInfo(
                platform = key if is_platform else "",
                config = key if not is_platform else configs_by_platform.get(key, ""),
                patterns = patterns_by_platform.get(key, depset()),
                rust_patterns = rust_patterns_by_platform.get(key, depset()),
                display_name = display_names_by_platform.get(key, ""),
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
                "config": "k_host",
                "display_name": "My Device",
                "rust_target_patterns": [
                    "//foo/..."
                ],
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
            "config": platform_pattern.config,
            "display_name": platform_pattern.display_name,
            "platform": str(platform_pattern.platform) if platform_pattern.platform else "",
            "rust_target_patterns": platform_pattern.rust_patterns.to_list(),
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
        "config": attr.string(),
        "config_out": attr.output(mandatory = True),
        "deps": attr.label_list(providers = [_CompileCommandsGroupInfo]),
        "display_name": attr.string(),
        "platform": attr.string(),
        "rust_target_patterns": attr.string_list(),
        "symlink_prefix": attr.string(default = ""),
        "target_patterns": attr.string_list(),
        "_updater": attr.label(default = Label("//pw_ide/bazel:update_compile_commands"), executable = True, cfg = "target"),
    },
    executable = True,
)

def pw_compile_commands_generator(name, target_patterns = None, deps = None, platform = None, symlink_prefix = "", display_name = "", rust_target_patterns = None, config = "", **kwargs):
    """A rule that can be used to build a compile command database.

    Each instance of `pw_compile_commands_generator` is intended to represent
    a unique target platform.

    This rule can be `bazel run` to generate a compile command database at
    the root of the workspace.

    Args:
      name: The name of this target.
      target_patterns: A list of target patterns that will be included in this
        compile command database.
      deps: A list of other `pw_compile_commands_generator` targets to include
        in this database.
      platform: The platform to use to evaluate the provided `target_patterns`.
      symlink_prefix: The prefix used for Bazel convenience symlinks (e.g. "out/").
      display_name: Optional display name for the platform.
      rust_target_patterns: A list of target patterns used to generate Rust code intelligence.
      config: Optional build configuration for compilation and platform inference.
      **kwargs: Extra arguments to pass to the underlying `native_binary` rule.
    """
    if target_patterns == None:
        target_patterns = []
    if deps == None:
        deps = []
    if rust_target_patterns == None:
        rust_target_patterns = []

    args = [
        "--compile-command-groups",
        "$(rootpath :{}_target_patterns.json)".format(name),
    ]

    if symlink_prefix:
        args.extend(["--symlink-prefix", symlink_prefix])

    _pw_compile_commands_generator(
        name = name,
        deps = deps,
        target_patterns = [
            _resolve_target_pattern(pattern)
            for pattern in target_patterns
        ],
        rust_target_patterns = [
            _resolve_target_pattern(pattern)
            for pattern in rust_target_patterns
        ],
        config = config,
        config_out = "{}_target_patterns.json".format(name),
        args = args,
        symlink_prefix = symlink_prefix,
        # Don't follow aliases, they technically mean different things from
        # a configuration perspective.
        platform = str(native.package_relative_label(platform)) if platform else None,
        display_name = display_name,
        tags = kwargs.pop("tags", []) + ["pw_compile_commands_generator"],
        **kwargs
    )
