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
"""Bazel rules for standard header-based versioned enums."""

load("@bazel_tools//tools/build_defs/cc:action_names.bzl", "ACTION_NAMES")
load("@bazel_tools//tools/cpp:toolchain_utils.bzl", "find_cpp_toolchain", "use_cpp_toolchain")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

# This need to be disabled until the `module_name` variable is properly
# set by create_compile_variables().
_UNSUPPORTED_FEATURES = ["module_maps", "use_header_modules"]

def _pw_cc_enum_generator_impl(ctx):
    internal_lib = ctx.attr.internal_lib
    comp_context = internal_lib[CcInfo].compilation_context

    cc_toolchain = find_cpp_toolchain(ctx)
    feature_configuration = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
        requested_features = ctx.features,
        unsupported_features = ctx.disabled_features + _UNSUPPORTED_FEATURES,
    )

    cc_compiler_path = cc_common.get_tool_for_action(
        feature_configuration = feature_configuration,
        action_name = ACTION_NAMES.cpp_compile,
    )

    base_cc = ctx.file.base_cc

    # Use the placeholder C++ source file to create compilation variables,
    # ensuring the exact compilation command is generated.
    cc_compile_variables = cc_common.create_compile_variables(
        feature_configuration = feature_configuration,
        cc_toolchain = cc_toolchain,
        source_file = base_cc.path,
        user_compile_flags = ctx.fragments.cpp.copts + ctx.fragments.cpp.cxxopts,
        include_directories = comp_context.includes,
        quote_include_directories = comp_context.quote_includes,
        system_include_directories = comp_context.system_includes,
        framework_include_directories = comp_context.framework_includes,
        preprocessor_defines = depset(transitive = [
            comp_context.defines,
            comp_context.local_defines,
        ]),
    )

    cc_compile_flags = cc_common.get_memory_inefficient_command_line(
        feature_configuration = feature_configuration,
        action_name = ACTION_NAMES.cpp_compile,
        variables = cc_compile_variables,
    )

    flags_file = ctx.actions.declare_file(ctx.label.name + "_flags.txt")
    ctx.actions.write(
        output = flags_file,
        content = "\n".join(cc_compile_flags),
    )

    out_files = []
    for h in ctx.files.hdrs:
        out_file = ctx.actions.declare_file(h.basename, sibling = h)
        out_files.append(out_file)

    args = ctx.actions.args()
    args.add_all(ctx.files.hdrs)
    args.add("--outputs")
    args.add_all(out_files)
    args.add("--compiler")
    args.add(cc_compiler_path)
    args.add("--compiler-flags")
    args.add(flags_file)
    args.add("--base-cc")
    args.add(base_cc.path)

    ctx.actions.run(
        inputs = depset(
            [flags_file, base_cc] + ctx.files.hdrs,
            transitive = [comp_context.headers, cc_toolchain.all_files],
        ),
        outputs = out_files,
        executable = ctx.executable._generator,
        arguments = [args],
        mnemonic = "PwCcEnumGen",
    )

    out_path = ctx.bin_dir.path
    if ctx.label.workspace_root:
        out_path += "/" + ctx.label.workspace_root
    if ctx.label.package:
        out_path += "/" + ctx.label.package

    my_includes = [out_path]
    if ctx.attr.strip_include_prefix:
        prefix = ctx.attr.strip_include_prefix
        if prefix.startswith("/"):
            prefix = prefix[1:]
        my_includes.append(out_path + "/" + prefix)

    new_comp_context = cc_common.create_compilation_context(
        headers = depset(out_files),
        includes = depset(my_includes),
    )

    my_cc_info = CcInfo(compilation_context = new_comp_context)

    # Merge with dependencies to ensure transitive includes are propagated
    return [
        cc_common.merge_cc_infos(cc_infos = [my_cc_info] + [dep[CcInfo] for dep in ctx.attr.deps]),
        DefaultInfo(files = depset(out_files)),
    ]

_pw_cc_enum_generator = rule(
    implementation = _pw_cc_enum_generator_impl,
    attrs = {
        "base_cc": attr.label(allow_single_file = [".cc"]),
        "deps": attr.label_list(providers = [CcInfo]),
        "hdrs": attr.label_list(allow_files = [".h"]),
        "internal_lib": attr.label(providers = [CcInfo]),
        "strip_include_prefix": attr.string(),
        "_cc_toolchain": attr.label(
            default = Label("@bazel_tools//tools/cpp:current_cc_toolchain"),
        ),
        "_generator": attr.label(
            default = Label("//pw_enum/py:enum_generator"),
            executable = True,
            cfg = "exec",
        ),
    },
    toolchains = use_cpp_toolchain(),
    fragments = ["cpp"],
)

def pw_cc_enum(name, hdrs, strip_include_prefix, deps = [], visibility = None, **kwargs):
    """Generates a C++ library with versioned enums.

    Args:
      name: The name of the cc_library target.
      hdrs: Standard .h files containing enum definitions.
      deps: Dependencies (other pw_cc_enum or cc_library targets).
      strip_include_prefix: Prefix to strip from the include path of generated headers.
      visibility: Visibility of the target.
      **kwargs: Additional arguments passed to the underlying rule.
    """
    testonly = kwargs.get("testonly", False)

    native.genrule(
        name = name + ".base_cc_gen",
        outs = [name + ".base.cc"],
        cmd = "echo '// placeholder' > $@",
        visibility = ["//visibility:private"],
        testonly = testonly,
    )

    cc_library(
        name = name + ".base_lib",
        srcs = [":" + name + ".base_cc_gen"],
        hdrs = hdrs,
        deps = [
            "//pw_enum:_generate_internal_do_not_use",
            "//pw_log:args",
        ] + deps,
        strip_include_prefix = strip_include_prefix,
        visibility = ["//visibility:private"],
        testonly = testonly,
        tags = ["no-clang-tidy-headers", "manual"],
    )

    _pw_cc_enum_generator(
        name = name + ".generate",
        internal_lib = ":" + name + ".base_lib",
        base_cc = ":" + name + ".base_cc_gen",
        hdrs = hdrs,
        deps = deps + ["//pw_enum:_generate_internal_do_not_use"],
        strip_include_prefix = strip_include_prefix,
        visibility = ["//visibility:private"],
        testonly = testonly,
        tags = ["no-clang-tidy-headers"],
    )
    cc_library(
        name = name,
        hdrs = [":" + name + ".generate"],
        strip_include_prefix = strip_include_prefix,
        deps = [
            "//pw_enum:_generate_internal_do_not_use",
            "//pw_log:args",
            "//pw_tokenizer",
        ] + deps,
        visibility = visibility,
        **kwargs
    )
