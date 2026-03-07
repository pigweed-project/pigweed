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
"""
A self-contained Bazel aspect to generate compile_commands.json fragments.

NOTE: This functionality may eventually belong in rules_cc. See b/437157251
"""

load("@bazel_skylib//lib:paths.bzl", "paths")
load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")
load("@rules_cc//cc:action_names.bzl", "ACTION_NAMES")
load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cc_toolchain", "use_cc_toolchain")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

CompileCommandsFragmentInfo = provider(
    "Provides the depset of compile command fragment files.",
    fields = {
        "fragments": "A depset of compile_commands.json fragment files.",
    },
)

VirtualIncludeMappingInfo = provider(
    "A single virtual include path mapping",
    fields = {
        "real_path": "(string) the real path to the file",
        "virtual_path": "(string) The virtual path",
    },
)

VirtualIncludesInfo = provider(
    "Provides virtual include path remappings",
    fields = {
        "mappings": "(depset[VirtualIncludeMappingInfo]) Virtual include path mappings",
    },
)

# This is the set of attribute types that do not expose targets.
_NO_PROVIDER_ATTR_TYPES = set([
    "bool",
    "int",
    "float",
    "string",
    "Label",
    "License",
    "NoneType",
])

_C_COMPILE_EXTS = [
    "c",
]
_CPP_COMPILE_EXTS = [
    "cc",
    "cpp",
]
_ALL_COMPILE_EXTS = _C_COMPILE_EXTS + _CPP_COMPILE_EXTS

# This must match rules_cc.
_VIRTUAL_INCLUDES_DIR = "_virtual_includes"

_SHORTEN_VIRTUAL_INCLUDES_FEATURE_NAME = "shorten_virtual_includes"

# This value is somewhat arbitrary. It's roughly associated with the maximum
# number of direct dependencies any one target may have.
_MAX_STACK_ITERATIONS = 99999

def _package_path(label):
    """
    Returns the execroot-relative path to the specified package.

    Args:
        label: The label that the returned package path should be extracted
            from.

    Returns:
        The execroot-relative path to the specified package.
    """
    sibling_repository_layout = label.workspace_root.startswith("../")
    repository = label.workspace_name
    package = label.package
    if repository == "" or sibling_repository_layout:
        return package
    return paths.join(paths.join("external", repository), package)

def _get_direct_virtual_includes(ctx, target):
    if not hasattr(ctx.rule.attr, "strip_include_prefix"):
        return []
    label = target.label
    source_package_path = _package_path(label)

    cc_toolchain = find_cc_toolchain(ctx)
    feature_configuration = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
    )
    shorten_virtual_includes = cc_common.is_enabled(
        feature_configuration = feature_configuration,
        feature_name = _SHORTEN_VIRTUAL_INCLUDES_FEATURE_NAME,
    )

    if shorten_virtual_includes:
        virtual_includes = paths.join(
            ctx.bin_dir.path,
            _VIRTUAL_INCLUDES_DIR,
            "%x" % hash(paths.join(source_package_path, label.name)),
        )
    else:
        virtual_includes = paths.join(
            ctx.bin_dir.path,
            source_package_path,
            _VIRTUAL_INCLUDES_DIR,
            label.name,
        )
    return [
        VirtualIncludeMappingInfo(
            virtual_path = virtual_includes,
            real_path = paths.join(
                source_package_path,
                ctx.rule.attr.strip_include_prefix,
            ),
        ),
    ]

def _remap_arg(arg, virtual_path_map):
    """Remaps a single argument using the virtual path map."""
    # We iterate over the map to support substring replacement for flags
    # like -I/path/to/virtual where the path is concatenated with the flag.

    # Short circuit checking if there is a path separator at all before iterating,
    # because resolving virtual includes is an expensive operation.
    if "/" not in arg:
        return arg

    for v_path, r_path in virtual_path_map.items():
        if v_path in arg:
            return arg.replace(v_path, r_path)
    return arg

def _remap_args(args, virtual_path_map):
    if not virtual_path_map:
        return args
    return [
        _remap_arg(arg, virtual_path_map)
        for arg in args
    ]

def _remap_commands(commands, virtual_path_map):
    """Replace all virtual includes with their actual path."""
    # cc_library's strip_include_prefix points to a sandboxed, generated include
    # path in Bazel's build outputs directory. We replace these with their
    # canonical paths for a few reasons:
    #
    #   * Clicking through to a header and ending up in the build directory
    #     is unexpected.
    #   * The generated directories/symlinks must be built before they're valid.
    #     Since the aspect implicitly builds them, this is sort of irrelevant
    #     Except...
    #   * The generated directories disappear if a user runs `bazel clean`.
    #
    # The logic to remap virtual include directories is more or less a
    # reimplementation of the same logic for building these virtual directories
    # in rules_cc:
    #
    # See https://cs.opensource.google/bazel/bazel/+/main:src/main/starlark/builtins_bzl/common/cc/compile/cc_compilation_helper.bzl;l=115-119;drc=075249556b518947c3b533cdf0358709b89db24d

    rebuilt_commands = []
    for command in commands:
        cmd_dict = {key: getattr(command, key) for key in dir(command)}
        cmd_dict["arguments"] = _remap_args(command.arguments, virtual_path_map)
        cmd_dict["file"] = _remap_arg(cmd_dict["file"], virtual_path_map)

        rebuilt_commands.append(struct(**cmd_dict))

    return rebuilt_commands

def _remap_target_infos(target_infos, virtual_path_map):
    rebuilt_target_infos = []
    for info in target_infos:
        rebuilt_info = {
            "deps": info["deps"],
            "hdrs": _remap_args(info["hdrs"], virtual_path_map),
            "label": info["label"],
            "srcs": _remap_args(info["srcs"], virtual_path_map),
        }
        rebuilt_target_infos.append(rebuilt_info)
    return rebuilt_target_infos

def _get_one_compile_command(ctx, src, action):
    """Extracts compile commands associated with the source.

    Args:
        ctx: Rule context.
        src: The specific source file to extract a compile command for.
        action: The list of actions that may contain a compiler invocation
            for the requested source file.

    Returns:
        A single compile commands struct, or None.
    """
    if src.extension not in _ALL_COMPILE_EXTS:
        return None

    # Don't generate compile commands for arbitrary inputs that aren't
    # compiled. E.g. sources placed in `extra_compiler_inputs`.
    if src.path not in action.argv:
        return None

    cc_toolchain = find_cc_toolchain(ctx)
    feature_configuration = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
    )
    action_name = (
        ACTION_NAMES.c_compile if src.extension in _C_COMPILE_EXTS else ACTION_NAMES.cpp_compile
    )
    tool = cc_common.get_tool_for_action(
        feature_configuration = feature_configuration,
        action_name = action_name,
    )
    if not action.argv or action.argv[0] != tool:
        return None

    return struct(
        directory = "__WORKSPACE_ROOT__",
        file = src.path,
        arguments = action.argv,
        # The same file may be compiled multiple times with different arguments.
        # This field signals which outputs are associated with this compilation.
        outputs = [f.path for f in action.outputs.to_list()],
    )

def _get_commands_for_target(ctx, target):
    """Iterates over actions to find compile commands."""
    commands = []
    if CcInfo not in target:
        return commands

    for action in target.actions:
        # Don't try to evaluate anything without arguments. This saves a
        # considerable amount of CPU cycles.
        if not action.argv:
            continue

        for src in action.inputs.to_list():
            # BE CAREFUL when putting logic here, as it's multiplied by
            # ALL inputs to the sandbox, which is on the order of thousands
            # of files *per compile action*.
            result = _get_one_compile_command(ctx, src, action)
            if result != None:
                commands.append(result)
    return commands

def _get_target_infos(ctx, target):
    target_infos = [{
        "deps": [str(d.label) for d in ctx.rule.attr.deps] if hasattr(ctx.rule.attr, "deps") else [],
        "hdrs": [f.path for f in ctx.rule.files.hdrs] if hasattr(ctx.rule.files, "hdrs") else [],
        "label": str(target.label),
        "srcs": [f.path for f in ctx.rule.files.srcs] if hasattr(ctx.rule.files, "srcs") else [],
    }]

    if hasattr(ctx.rule.attr, "deps"):
        for d in ctx.rule.attr.deps:
            dep_info = {
                "deps": [],
                "hdrs": [],
                "label": str(d.label),
                "srcs": [],
            }
            if CcInfo in d:
                # Fallback to scanning direct_public_headers/direct_headers safely
                ctx_cc = d[CcInfo].compilation_context
                if hasattr(ctx_cc, "direct_public_headers"):
                    dep_info["hdrs"] = [f.path for f in ctx_cc.direct_public_headers]
                elif hasattr(ctx_cc, "direct_headers"):
                    dep_info["hdrs"] = [f.path for f in ctx_cc.direct_headers]

            target_infos.append(dep_info)
    return target_infos

def _get_all_virtual_include_mappings(ctx, target):
    if CcInfo not in target:
        return depset()
    direct_virtual_includes = _get_direct_virtual_includes(ctx, target)
    transitive_virtual_includes = _collect_fragments(
        ctx.rule,
        target.label,
        VirtualIncludesInfo,
        lambda virtual_includes: virtual_includes.mappings,
    )
    return depset(
        direct = direct_virtual_includes,
        transitive = transitive_virtual_includes,
    )

def _get_cpp_compile_commands(ctx, target):
    """Collects C/C++ compile commands for the provided target.

    Args:
        ctx: Rule context.
        target: The target to extract compile commands from.

    Returns:
        List of compile commands `struct` objects.
    """

    # Workaround: Rules with `analysis_test=true` cannot register actions.
    # Analysis tests can be identified because they return `AnalysisTestResultInfo`.
    # However, since we might not have `AnalysisTestResultInfo` imported, we can
    # also check if the rule kind explicitly implies tests that we shouldn't touch.
    if (
        ctx.rule.kind == "_load_phase_test" or
        "analysis_test" in ctx.rule.kind or
        "pylint_aspect_test" in ctx.rule.kind
    ):
        return struct(
            fragment_files = [],
            providers = [
                VirtualIncludesInfo(
                    mappings = depset(),
                ),
            ],
        )

    all_virtual_include_mappings = _get_all_virtual_include_mappings(ctx, target)

    # Convert depset to dict once for O(1) lookups during remapping
    virtual_path_map = {
        mapping.virtual_path: mapping.real_path
        for mapping in all_virtual_include_mappings.to_list()
    }

    commands = _get_commands_for_target(ctx, target)
    target_infos = _get_target_infos(ctx, target)

    commands = _remap_commands(commands, virtual_path_map)
    target_infos = _remap_target_infos(target_infos, virtual_path_map)

    platform_fragment = ctx.bin_dir.path.split("/")[1]

    fragment_file = ctx.actions.declare_file(
        "{name}.{platform}.pw_aspect.compile_commands.json".format(
            name = ctx.label.name,
            platform = platform_fragment,
        ),
    )

    debug_mappings = []

    for v_path, r_path in virtual_path_map.items():
        debug_mappings.append({"real_path": r_path, "virtual_path": v_path})

    fragment_data = {
        "commands": commands,
        "target_info": target_infos,
        "virtual_mappings": debug_mappings,
    }

    ctx.actions.write(
        output = fragment_file,
        content = json.encode(fragment_data),
    )

    return struct(
        fragment_files = [fragment_file],
        providers = [
            DefaultInfo(files = depset([fragment_file])),
            VirtualIncludesInfo(
                mappings = all_virtual_include_mappings,
            ),
        ],
    )

def _collect_fragments(ctx, label, requested_provider, depset_getter):
    """A Generic helper to build a depset from all a transitive dependencies.

    Args:
        ctx: Rule context.
        label: Label of the current target under evaluation, used for error
            reporting.
        requested_provider: The provider to collect a depset from.
        depset_getter: A callback that takes the requested provider and extracts
            the correct depset from the provider.

    Returns:
        A list of depsets.
    """

    # NOTE: This looks like it should be a recursive method, but recursion is
    # strictly prohibited in Bazel/starlark. This is implemented as a loop with
    # a work stack instead.
    transitive_providers = []
    stack = []
    for attr_name in dir(ctx.attr):
        if attr_name in ["def_parser", "grep_includes"] or attr_name.startswith("_"):
            continue
        stack.append(getattr(ctx.attr, attr_name))

    for i in range(_MAX_STACK_ITERATIONS + 1):
        if i == _MAX_STACK_ITERATIONS:
            fail(
                "{} has too many direct dependencies.".format(label),
                "Reached maximum depth ({})".format(_MAX_STACK_ITERATIONS),
                "of direct dependency expansion.",
            )
        if not stack:
            break
        actual_attr = stack.pop()
        if type(actual_attr) in _NO_PROVIDER_ATTR_TYPES:
            continue

        if type(actual_attr) == "Target":
            if requested_provider not in actual_attr:
                continue
            transitive_providers.append(
                depset_getter(actual_attr[requested_provider]),
            )
            continue

        if type(actual_attr) == "list":
            stack.extend(actual_attr)
            continue

        if type(actual_attr) == "dict":
            stack.extend(actual_attr.keys())
            stack.extend(actual_attr.values())
            continue

        if ctx.attr._strict_errors[BuildSettingInfo].value:
            # Note to maintainers:
            #   * If the unhandled type is a data structure that may contain
            #     Target objects, handle it above.
            #   * Otherwise, add to _NO_PROVIDER_ATTR_TYPES.
            fail(
                "Unhandled type: {}".format(type(actual_attr)),
                "",
                "This means a new attribute has been introduced into Bazel.",
                "To work around this:",
                "   * Set {}=False".format(ctx.attr._strict_errors.label),
                "   * Report this failure to Pigweed, making sure to include ",
                "     the Bazel version you are using.",
                sep = "\n",
            )

    return transitive_providers

def _compile_commands_aspect_impl(target, ctx):
    """Generates a compile_commands.json fragment for a single target."""

    dep_fragments = _collect_fragments(
        ctx.rule,
        target.label,
        CompileCommandsFragmentInfo,
        lambda command_fragment_info: command_fragment_info.fragments,
    )

    result = _get_cpp_compile_commands(
        ctx,
        target,
    )
    providers = result.providers
    fragment_files = result.fragment_files

    direct_files = fragment_files
    compile_command_fragments = depset(
        direct = direct_files,
        transitive = dep_fragments,
    )

    return [
        # Always return the transitive fragments. This ensures that
        # transitive dependencies chain across targets that do not contain
        # compile commands.
        CompileCommandsFragmentInfo(fragments = compile_command_fragments),
        # Always return this OutputGroupInfo, or transitive compile commands
        # will not be generated if the target itself doesn't emit compile
        # commands.
        OutputGroupInfo(
            pw_cc_compile_commands_fragments = compile_command_fragments,
        ),
    ] + providers

pw_cc_compile_commands_aspect = aspect(
    implementation = _compile_commands_aspect_impl,
    attr_aspects = ["deps", "implementation_deps", "srcs", "target", "backend", "includes"],
    provides = [VirtualIncludesInfo, CompileCommandsFragmentInfo],
    fragments = ["cpp"],
    attrs = {
        "_strict_errors": attr.label(
            default = "//pw_ide/bazel/compile_commands:strict_errors",
        ),
    },
    toolchains = use_cc_toolchain(),
)

def _pw_cc_compile_commands_fragments_impl(ctx):
    dep_fragments = _collect_fragments(
        ctx,
        ctx.label,
        CompileCommandsFragmentInfo,
        lambda command_fragment_info: command_fragment_info.fragments,
    )
    return DefaultInfo(
        files = depset(transitive = dep_fragments),
    )

pw_cc_compile_commands_fragments = rule(
    implementation = _pw_cc_compile_commands_fragments_impl,
    attrs = {
        "targets": attr.label_list(
            mandatory = True,
            aspects = [pw_cc_compile_commands_aspect],
        ),
    },
)

compile_commands_aspect_testing = struct(
    remap_arg = _remap_arg,
    remap_target_infos = _remap_target_infos,
    remap_commands = _remap_commands,
    get_cpp_compile_commands = _get_cpp_compile_commands,
)
