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
"""Bazel rules for versioned enums from C++ headers or pw_enum targets."""

load("@bazel_tools//tools/build_defs/cc:action_names.bzl", "ACTION_NAMES")
load("@bazel_tools//tools/cpp:toolchain_utils.bzl", "find_cpp_toolchain", "use_cpp_toolchain")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load("//pw_enum:pw_enum.bzl", "PwEnumInfo", "join_paths", "pw_enum_codegen_args")

# This need to be disabled until the `module_name` variable is properly
# set by create_compile_variables().
_UNSUPPORTED_FEATURES = ["module_maps", "use_header_modules"]

# Generated headers include pw_status for PW_ENUM_STATUS declarations.
_PW_STATUS = Label("//pw_status")

def _with_pw_status(deps):
    """Adds pw_status to deps, unless it is already present."""
    if type(deps) != "list":
        # `select()` can't be introspected, so always add the implicit dep.
        return deps + [str(_PW_STATUS)]
    for dep in deps:
        if native.package_relative_label(dep) == _PW_STATUS:
            return deps
    return deps + [str(_PW_STATUS)]

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

_PwCcEnumInfo = provider(
    doc = "C++ code generated from a pw_enum target.",
    fields = {
        "cc_info": "CcInfo: The generated headers and their dependencies.",
        "hdrs": "depset[File]: The generated headers.",
    },
)

def _pw_cc_enum_aspect_impl(target, ctx):
    info = target[PwEnumInfo]

    hdrs = [ctx.actions.declare_file(stem + ".h") for stem in info.output_stems]

    args, inputs = pw_enum_codegen_args(ctx.actions, "cc", info)
    args.add("--outputs")
    args.add_all(hdrs)

    ctx.actions.run(
        inputs = inputs,
        outputs = hdrs,
        executable = ctx.executable._codegen,
        arguments = [args],
        mnemonic = "PwEnumCcGen",
        progress_message = "Generating pw_enum C++ headers for %{label}",
    )

    cc_toolchain = find_cpp_toolchain(ctx)
    feature_configuration = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
        requested_features = ctx.features,
        unsupported_features = ctx.disabled_features,
    )

    includes = []
    if info.include_root:
        includes.append(join_paths(
            ctx.bin_dir.path,
            ctx.label.workspace_root,
            ctx.label.package,
            info.include_root,
        ))

    dep_cc_infos = [dep[CcInfo] for dep in ctx.attr._cc_deps]
    compilation_context, _ = cc_common.compile(
        name = ctx.label.name + ".pw_cc_enum",
        actions = ctx.actions,
        feature_configuration = feature_configuration,
        cc_toolchain = cc_toolchain,
        public_hdrs = hdrs,
        includes = includes,
        compilation_contexts = [i.compilation_context for i in dep_cc_infos],
    )

    return [_PwCcEnumInfo(
        cc_info = cc_common.merge_cc_infos(
            direct_cc_infos = [CcInfo(compilation_context = compilation_context)],
            cc_infos = dep_cc_infos,
        ),
        hdrs = depset(hdrs),
    )]

# Generates C++ headers once per pw_enum target, no matter how many
# pw_cc_enum_library targets reference it.
_pw_cc_enum_aspect = aspect(
    implementation = _pw_cc_enum_aspect_impl,
    attrs = {
        "_cc_deps": attr.label_list(
            default = [
                Label("//pw_enum"),
                Label("//pw_log:args"),
                _PW_STATUS,
                Label("//pw_tokenizer"),
            ],
            providers = [CcInfo],
        ),
        "_cc_toolchain": attr.label(
            default = Label("@bazel_tools//tools/cpp:current_cc_toolchain"),
        ),
        "_codegen": attr.label(
            default = Label("//pw_enum/py:codegen"),
            executable = True,
            cfg = "exec",
        ),
    },
    required_providers = [PwEnumInfo],
    provides = [_PwCcEnumInfo],
    toolchains = use_cpp_toolchain(),
    fragments = ["cpp"],
)

def _pw_cc_enum_merge_impl(ctx):
    enum_infos = [e[_PwCcEnumInfo] for e in ctx.attr.enums]
    cc_infos = [i.cc_info for i in enum_infos] + [d[CcInfo] for d in ctx.attr.deps]

    # Like cc_proto_library, export the headers as direct headers, so targets
    # that depend on this one may include them with layering_check enabled.
    return [
        cc_common.merge_cc_infos(direct_cc_infos = cc_infos),
        DefaultInfo(
            files = depset(
                transitive = [i.hdrs for i in enum_infos] +
                             [d[DefaultInfo].files for d in ctx.attr.deps],
            ),
            runfiles = ctx.runfiles().merge_all(
                [d[DefaultInfo].default_runfiles for d in ctx.attr.deps],
            ),
        ),
    ]

_pw_cc_enum_merge = rule(
    implementation = _pw_cc_enum_merge_impl,
    attrs = {
        "deps": attr.label_list(providers = [CcInfo]),
        "enums": attr.label_list(
            providers = [PwEnumInfo],
            aspects = [_pw_cc_enum_aspect],
        ),
    },
    provides = [CcInfo],
)

# Arguments that apply to the wrapper target as well as the cc_library.
_WRAPPER_KWARGS = ("tags", "target_compatible_with", "testonly")

def pw_cc_enum_library(
        name,
        enum_headers = [],
        enums = [],
        hdrs = [],
        srcs = [],
        deps = [],
        strip_include_prefix = None,
        visibility = None,
        **kwargs):
    """Defines a C++ library that automatically tokenizes versioned enums.

    Enums may be defined in C++ headers that call `PW_ENUM()` (`enum_headers`),
    in `pw_enum` targets (`enums`), or both. A `pw_enum` source at
    `bar/baz/foo.toml` generates the header `bar/baz/foo.h`. Headers in
    `enum_headers` may include and reference headers generated from `enums`.

    Args:
      name: The name of the library target.
      enum_headers: Standard .h files containing enum definitions to tokenize.
      enums: pw_enum targets to generate C++ headers for.
      hdrs: Public headers of the library.
      srcs: Source files of the library.
      deps: Dependencies.
      strip_include_prefix: Prefix to strip from the include path of
        `enum_headers` and `hdrs`. Does not affect headers generated from
        `enums`; use `strip_import_prefix` on the `pw_enum` target instead.
      visibility: Visibility of the target.
      **kwargs: Additional arguments passed to the underlying cc_library.
    """
    if not enum_headers and not enums:
        fail("pw_cc_enum_library requires enum_headers, enums, or both")
    if strip_include_prefix != None and not (enum_headers or hdrs):
        fail("strip_include_prefix only applies to enum_headers and hdrs; " +
             "set strip_import_prefix on the pw_enum target instead")

    wrapper_kwargs = {k: kwargs[k] for k in _WRAPPER_KWARGS if k in kwargs}

    if not enums:
        _header_enum_library(
            name = name,
            enum_headers = enum_headers,
            hdrs = hdrs,
            srcs = srcs,
            deps = deps,
            strip_include_prefix = strip_include_prefix,
            visibility = visibility,
            **kwargs
        )
        return

    has_cc_kwargs = any([k not in _WRAPPER_KWARGS for k in kwargs])
    if not (enum_headers or hdrs or srcs or has_cc_kwargs):
        _pw_cc_enum_merge(
            name = name,
            enums = enums,
            deps = deps,
            visibility = visibility,
            **wrapper_kwargs
        )
        return

    # Headers and sources may include the headers generated from enums.
    _pw_cc_enum_merge(
        name = name + ".enums",
        enums = enums,
        visibility = ["//visibility:private"],
        **wrapper_kwargs
    )
    enum_deps = [":" + name + ".enums"]

    if enum_headers:
        _header_enum_library(
            name = name + ".lib",
            enum_headers = enum_headers,
            hdrs = hdrs,
            srcs = srcs,
            deps = enum_deps + deps,
            strip_include_prefix = strip_include_prefix,
            visibility = ["//visibility:private"],
            **kwargs
        )
    else:
        cc_library(
            name = name + ".lib",
            srcs = srcs,
            hdrs = hdrs,
            strip_include_prefix = strip_include_prefix,
            deps = enum_deps + deps,
            visibility = ["//visibility:private"],
            **kwargs
        )

    _pw_cc_enum_merge(
        name = name,
        enums = enums,
        deps = [":" + name + ".lib"],
        visibility = visibility,
        **wrapper_kwargs
    )

def _header_enum_library(
        name,
        enum_headers,
        hdrs,
        srcs,
        deps,
        strip_include_prefix,
        visibility,
        **kwargs):
    """Defines a cc_library with versioned enums from PW_ENUM() headers."""
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
        hdrs = enum_headers + hdrs,
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
        hdrs = enum_headers,
        deps = deps + ["//pw_enum:_generate_internal_do_not_use"],
        strip_include_prefix = strip_include_prefix,
        visibility = ["//visibility:private"],
        testonly = testonly,
        tags = ["no-clang-tidy-headers"],
    )

    cc_library(
        name = name,
        srcs = srcs,
        hdrs = [":" + name + ".generate"] + hdrs,
        strip_include_prefix = strip_include_prefix,
        deps = [
            "//pw_enum:_generate_internal_do_not_use",
            "//pw_log:args",
            "//pw_tokenizer",
        ] + _with_pw_status(deps),
        visibility = visibility,
        **kwargs
    )
