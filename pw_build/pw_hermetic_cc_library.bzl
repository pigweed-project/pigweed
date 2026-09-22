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
"""Bazel rule for creating incrementally-linked hermetic C/C++ libraries.

Derived from
https://cs.opensource.google/fuchsia/fuchsia/+/main:build/toolchain/hermetic_source_set.gni

An incrementally-linked hermetic library takes compiled objects from its `srcs`
and private dependencies (`deps`), performs a relocatable link (`ld -r`), and
localizes all internal symbols using `objcopy --keep-global-symbol`. Only
symbols explicitly listed in `global_symbols` (or `global_symbols_file`) remain
globally visible. Public dependencies and headers passed via `public_deps` /
`hdrs` are propagated to consumers without localization.
"""

load("@bazel_tools//tools/build_defs/cc:action_names.bzl", "ACTION_NAMES")
load(
    "@bazel_tools//tools/cpp:toolchain_utils.bzl",
    "find_cpp_toolchain",
    "use_cpp_toolchain",
)
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

def _get_static_library_name(name):
    prefix = "" if name.startswith("lib") else "lib"
    return prefix + name + ".a"

def _pw_hermetic_cc_library_impl(ctx):
    cc_toolchain = find_cpp_toolchain(ctx)

    feature_configuration = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
        requested_features = ctx.features,
        unsupported_features = ctx.disabled_features,
    )

    use_pic = cc_common.is_enabled(
        feature_configuration = feature_configuration,
        feature_name = "pic",
    )

    # Compile srcs and collect compilation context
    compilation_contexts = []
    for dep in ctx.attr.deps + ctx.attr.public_deps:
        if CcInfo in dep:
            compilation_contexts.append(dep[CcInfo].compilation_context)

    compilation_context, compilation_outputs = cc_common.compile(
        name = ctx.label.name,
        actions = ctx.actions,
        feature_configuration = feature_configuration,
        cc_toolchain = cc_toolchain,
        srcs = ctx.files.srcs,
        public_hdrs = ctx.files.hdrs,
        includes = ctx.attr.includes,
        strip_include_prefix = ctx.attr.strip_include_prefix,
        defines = ctx.attr.defines,
        user_compile_flags = ctx.attr.copts,
        compilation_contexts = compilation_contexts,
    )

    src_objects = []
    if use_pic and compilation_outputs.pic_objects:
        src_objects = compilation_outputs.pic_objects
    elif compilation_outputs.objects:
        src_objects = compilation_outputs.objects

    # Collect linking inputs from private `deps`
    dep_objects = []
    dep_static_libs = []

    for dep in ctx.attr.deps:
        if CcInfo in dep:
            linking_context = dep[CcInfo].linking_context
            for linker_input in linking_context.linker_inputs.to_list():
                for lib in linker_input.libraries:
                    if use_pic:
                        # PIC: Prefer PIC inputs
                        if lib.pic_static_library:
                            dep_static_libs.append(lib.pic_static_library)
                        elif lib.static_library:
                            dep_static_libs.append(lib.static_library)
                        elif lib.pic_objects:
                            dep_objects.extend(lib.pic_objects)
                        elif lib.objects:
                            dep_objects.extend(lib.objects)
                    else:
                        # Non-PIC: Prefer non-PIC inputs
                        if lib.static_library:
                            dep_static_libs.append(lib.static_library)
                        elif lib.pic_static_library:
                            dep_static_libs.append(lib.pic_static_library)
                        elif lib.objects:
                            dep_objects.extend(lib.objects)
                        elif lib.pic_objects:
                            dep_objects.extend(lib.pic_objects)

    # Deduplicate dependencies while preserving order
    dep_static_libs = depset(dep_static_libs).to_list()
    dep_objects = depset(dep_objects).to_list()

    # Perform relocatable link (ld -r / CC -r)
    linker_path = cc_common.get_tool_for_action(
        feature_configuration = feature_configuration,
        action_name = ACTION_NAMES.cpp_link_executable,
    )

    relocatable_o = ctx.actions.declare_file(ctx.label.name + ".relocatable.o")

    link_inputs = list(src_objects) + list(dep_objects) + list(dep_static_libs)
    if not link_inputs:
        fail("""pw_hermetic_cc_library target %s has no source or dependency
                object files to link.""" % ctx.label)

    link_variables = cc_common.create_link_variables(
        feature_configuration = feature_configuration,
        cc_toolchain = cc_toolchain,
    )
    toolchain_link_flags = cc_common.get_memory_inefficient_command_line(
        feature_configuration = feature_configuration,
        action_name = ACTION_NAMES.cpp_link_executable,
        variables = link_variables,
    )

    link_args = list(toolchain_link_flags) + [
        "-r",
        "-nostdlib",
        # Toolchain executable link flags include runtime library options (e.g.
        # -pthread, --rtlib=compiler-rt, --unwindlib=libunwind) that clang++ warns
        # about as unused when performing a relocatable link (-r -nostdlib).
        "-Wno-unused-command-line-argument",
        # Override -Wl,--gc-sections in toolchain config, as that is applicable
        # only for a true, final link.
        "-Wl,--no-gc-sections",
    ]
    if ctx.attr.linker_flags:
        link_args.extend(ctx.attr.linker_flags)

    # Require global symbols to be defined / kept
    for sym in ctx.attr.global_symbols:
        link_args.append("-Wl,--undefined=" + sym)

    if dep_static_libs:
        link_args.append("-Wl,--whole-archive")
        for lib in dep_static_libs:
            link_args.append(lib.path)
        link_args.append("-Wl,--no-whole-archive")

    for obj in src_objects + dep_objects:
        link_args.append(obj.path)

    link_args.extend(["-o", relocatable_o.path])

    ctx.actions.run(
        outputs = [relocatable_o],
        inputs = depset(
            direct = link_inputs,
            transitive = [cc_toolchain.all_files],
        ),
        executable = linker_path,
        arguments = link_args,
        mnemonic = "HermeticLink",
        progress_message = "Hermetic partial link for %s" % ctx.label,
    )

    # Localize symbols with objcopy
    if (not ctx.attr.global_symbols and not ctx.file.global_symbols_file and
        not ctx.attr.localize_hidden):
        fail(
            "pw_hermetic_cc_library target {}".format(ctx.label) +
            " must specify 'global_symbols', 'global_symbols_file', or set" +
            " 'localize_hidden = True'.",
        )

    objcopy_path = cc_common.get_tool_for_action(
        feature_configuration = feature_configuration,
        action_name = ACTION_NAMES.objcopy_embed_data,
    )

    hermetic_o = ctx.actions.declare_file(ctx.label.name + ".hermetic.o")

    objcopy_args = []
    objcopy_inputs = [relocatable_o]

    if ctx.file.global_symbols_file:
        objcopy_args.append("--keep-global-symbols=" +
                            ctx.file.global_symbols_file.path)
        objcopy_inputs.append(ctx.file.global_symbols_file)

    if ctx.attr.global_symbols:
        for sym in ctx.attr.global_symbols:
            objcopy_args.append("--keep-global-symbol=" + sym)
    elif not ctx.file.global_symbols_file and ctx.attr.localize_hidden:
        objcopy_args.append("--localize-hidden")

    if ctx.attr.objcopy_flags:
        objcopy_args.extend(ctx.attr.objcopy_flags)

    objcopy_args.extend([relocatable_o.path, hermetic_o.path])

    ctx.actions.run(
        outputs = [hermetic_o],
        inputs = depset(
            direct = objcopy_inputs,
            transitive = [cc_toolchain.all_files],
        ),
        executable = objcopy_path,
        arguments = objcopy_args,
        mnemonic = "HermeticObjcopy",
        progress_message = "Localizing symbols for %s" % ctx.label,
    )

    # Undefined symbols and init/fini section verification
    validation_outputs = []
    verify_stamp = ctx.actions.declare_file(
        ctx.label.name + ".verify.stamp",
    )

    args = ctx.actions.args()
    args.add("--elf-file", hermetic_o.path)
    args.add("--stamp-file", verify_stamp.path)
    args.add("--label", str(ctx.label))

    if ctx.attr.allow_init:
        args.add("--allow-init")
    if ctx.attr.allow_fini:
        args.add("--allow-fini")

    for sym in ctx.attr.undefined_symbols:
        args.add("--allowed-symbol", sym)

    ctx.actions.run(
        outputs = [verify_stamp],
        inputs = [hermetic_o],
        executable = ctx.executable._verifier,
        arguments = [args],
        mnemonic = "VerifyHermetic",
        progress_message = (
            "Verifying hermetic library constraints for %s" % ctx.label
        ),
    )
    validation_outputs.append(verify_stamp)

    # Archive into .a library
    ar_path = cc_common.get_tool_for_action(
        feature_configuration = feature_configuration,
        action_name = ACTION_NAMES.cpp_link_static_library,
    )

    output_a_name = _get_static_library_name(ctx.label.name)
    output_a = ctx.actions.declare_file(output_a_name)

    ctx.actions.run(
        outputs = [output_a],
        inputs = depset(
            direct = [hermetic_o],
            transitive = [cc_toolchain.all_files],
        ),
        executable = ar_path,
        arguments = ["rcs", output_a.path, hermetic_o.path],
        mnemonic = "HermeticArchive",
        progress_message = "Archiving hermetic library for %s" % ctx.label,
    )

    # Build CcInfo
    library_to_link = cc_common.create_library_to_link(
        actions = ctx.actions,
        feature_configuration = feature_configuration,
        cc_toolchain = cc_toolchain,
        static_library = output_a,
        alwayslink = ctx.attr.alwayslink,
    )

    linker_input = cc_common.create_linker_input(
        owner = ctx.label,
        libraries = depset([library_to_link]),
    )

    public_linking_contexts = [
        dep[CcInfo].linking_context
        for dep in ctx.attr.public_deps
        if CcInfo in dep
    ]
    new_linking_context = cc_common.create_linking_context(
        linker_inputs = depset(
            direct = [linker_input],
            transitive = [lc.linker_inputs for lc in public_linking_contexts],
        ),
    )

    public_compilation_contexts = [
        dep[CcInfo].compilation_context
        for dep in ctx.attr.public_deps
        if CcInfo in dep
    ]
    merged_compilation_context = cc_common.merge_compilation_contexts(
        compilation_contexts = [compilation_context] + public_compilation_contexts,
    )

    return [
        DefaultInfo(files = depset([output_a])),
        OutputGroupInfo(_validation = depset(validation_outputs)),
        CcInfo(
            compilation_context = merged_compilation_context,
            linking_context = new_linking_context,
        ),
    ]

pw_hermetic_cc_library = rule(
    implementation = _pw_hermetic_cc_library_impl,
    doc = """Produces an incrementally-linked hermetic C/C++ static library.

    An incrementally-linked hermetic library compiles sources from `srcs`,
    takes private dependencies from `deps`, performs a relocatable link
    (`ld -r`), and localizes all internal symbols using `objcopy`.

    Only linkage symbols explicitly listed in `global_symbols` (or
    `global_symbols_file`) remain globally exported. Public dependencies and
    headers specified in `public_deps` and `hdrs` are propagated to downstream
    consumers without localization.

    This rule only supports ELF-based toolchains.
    """,
    attrs = {
        "allow_fini": attr.bool(
            default = False,
            doc = """If True, allow .fini_array section
                     (global destructors).""",
        ),
        "allow_init": attr.bool(
            default = False,
            doc = """If True, allow .init_array section
                     (global constructors).""",
        ),
        "alwayslink": attr.bool(
            default = False,
            doc = "If True, force linking of this library into binaries.",
        ),
        "copts": attr.string_list(
            doc = "Compiler flags for compilation.",
        ),
        "defines": attr.string_list(
            doc = "Preprocessor defines.",
        ),
        "deps": attr.label_list(
            providers = [CcInfo],
            doc = """Private dependencies to be partially linked into the
                     hermetic library and localized.""",
        ),
        "global_symbols": attr.string_list(
            doc = """List of linkage symbol names to retain as global symbols.
                     All other defined symbols will be localized.""",
        ),
        "global_symbols_file": attr.label(
            allow_single_file = True,
            doc = """File containing symbol names to retain as global symbols
                     (one per line).""",
        ),
        "hdrs": attr.label_list(
            allow_files = True,
            doc = "Public header files to expose to consumers.",
        ),
        "includes": attr.string_list(
            doc = "Include directories.",
        ),
        "linker_flags": attr.string_list(
            default = ["-Wl,--force-group-allocation"],
            doc = "Additional linker flags for the relocatable link step.",
        ),
        "localize_hidden": attr.bool(
            default = False,
            doc = """If True and global_symbols is empty, localize all
                     hidden symbols.""",
        ),
        "objcopy_flags": attr.string_list(
            doc = "Additional flags for objcopy.",
        ),
        "public_deps": attr.label_list(
            providers = [CcInfo],
            doc = """Public dependencies propagated to consumers without
                     localization.""",
        ),
        "srcs": attr.label_list(
            allow_files = True,
            doc = "Source files to compile into the hermetic library.",
        ),
        "strip_include_prefix": attr.string(
            doc = "Prefix to strip from header paths.",
        ),
        "undefined_symbols": attr.string_list(
            doc = "List of allowed undefined symbol names.",
        ),
        "_verifier": attr.label(
            default = "//pw_build/py:verify_hermetic_elf",
            executable = True,
            cfg = "exec",
        ),
    },
    provides = [CcInfo],
    fragments = ["cpp"],
    toolchains = use_cpp_toolchain(),
)
