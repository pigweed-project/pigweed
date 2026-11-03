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
"""Rule for assembling a pw_kernel system image from a kernel and its apps."""

load("@bazel_tools//tools/build_defs/cc:action_names.bzl", "ACTION_NAMES")
load("@bazel_tools//tools/cpp:toolchain_utils.bzl", "use_cpp_toolchain")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load("@rules_rust//rust:defs.bzl", "rust_common")

# rules_rust's compile action is used directly so that this rule owns the
# per-image app outputs. These symbols are not part of rules_rust's public API;
# check them when rolling rules_rust.
# buildifier: disable=bzl-visibility
load("@rules_rust//rust/private:rust.bzl", "RUSTC_ATTRS")

# buildifier: disable=bzl-visibility
load("@rules_rust//rust/private:rustc.bzl", "rustc_compile_action")
load("//pw_build:binary_tools.bzl", "run_action_on_executable")
load("//pw_kernel/tooling:app_info.bzl", "PwAppInfo", "crate_name_for", "dep_variant_infos")

_PLATFORMS_OPTION = "//command_line_option:platforms"
_PLATFORM_SUFFIX_OPTION = "//command_line_option:platform_suffix"

def _platform_settings(settings, platform):
    """Returns the transition settings that select `platform`.

    Bazel names output directories after the platform's name, not its label,
    and flags set by the platform do not contribute to the name either. Two
    platforms that share a name (e.g. a kernel platform and a board platform
    both called `pw_rp2350`) reached in one build would therefore write
    conflicting outputs to the same directory. When the transition changes the
    platform, append its label to --platform_suffix so every platform gets its
    own directory. When the build already targets `platform`, leave the
    configuration alone so it stays shared with the top-level build.
    """
    suffix = settings[_PLATFORM_SUFFIX_OPTION]
    if [str(p) for p in settings[_PLATFORMS_OPTION]] != [str(platform)]:
        label = "{}_{}_{}".format(platform.repo_name, platform.package, platform.name)
        label = "".join([c if c.isalnum() else "_" for c in label.elems()]).strip("_")
        suffix = "{}-{}".format(suffix, label) if suffix else label
    return {
        _PLATFORMS_OPTION: str(platform),
        _PLATFORM_SUFFIX_OPTION: suffix,
    }

def _target_transition_impl(settings, attr):
    return _platform_settings(settings, attr.platform) | {
        str(Label("//pw_kernel/userspace:userspace_build")): attr.userspace,
        # Only unittests should ever enable tests
        str(Label("//pw_kernel:enable_tests")): False,
    }

_target_transition = transition(
    implementation = _target_transition_impl,
    inputs = [_PLATFORMS_OPTION, _PLATFORM_SUFFIX_OPTION],
    outputs = [
        _PLATFORMS_OPTION,
        _PLATFORM_SUFFIX_OPTION,
        str(Label("//pw_kernel/userspace:userspace_build")),
        str(Label("//pw_kernel:enable_tests")),
    ],
)

def _app_target_transition_impl(settings, attr):
    return _platform_settings(settings, attr.platform) | {
        str(Label("//pw_kernel/userspace:userspace_build")): True,
        str(Label("//pw_kernel/userspace:is_app_build")): True,
        # Only unittests should ever enable tests
        str(Label("//pw_kernel:enable_tests")): False,
    }

_app_target_transition = transition(
    implementation = _app_target_transition_impl,
    inputs = [_PLATFORMS_OPTION, _PLATFORM_SUFFIX_OPTION],
    outputs = [
        _PLATFORMS_OPTION,
        _PLATFORM_SUFFIX_OPTION,
        str(Label("//pw_kernel/userspace:userspace_build")),
        str(Label("//pw_kernel/userspace:is_app_build")),
        str(Label("//pw_kernel:enable_tests")),
    ],
)

SystemImageInfo = provider(
    "Metadata about a system image",
    fields = {
        "bin": "[File] Path to the bin file",
        "elf": "[File] Path to the compiled elf file",
    },
)

_TEMPLATES = "@pigweed//pw_kernel/tooling/system_generator/templates"

# Edition of the multi-process entry point rendered from _TEMPLATES.
_GENERATED_ENTRY_EDITION = "2024"

# `[]` / `{}` literals created inside a rule implementation stay mutable until
# it returns, and a depset rejects a provider that holds a mutable value.
# CrateInfos built here are placed in the next crate's `deps` depset, so use
# frozen module-level constants.
_FROZEN_EMPTY_DICT = {}
_FROZEN_EMPTY_LIST = []

def _render_app_template(ctx, output, templates, args, mnemonic):
    arguments = []
    for name, template in templates.items():
        arguments += ["--template", "{}={}".format(name, template.path)]
    arguments += [
        "--config",
        ctx.file.system_config.path,
        "--output",
        output.path,
        "render-app-template",
    ] + args

    ctx.actions.run(
        inputs = [ctx.file.system_config] + templates.values(),
        outputs = [output],
        executable = ctx.executable._system_generator,
        mnemonic = mnemonic,
        arguments = arguments,
    )

def _find_provider(providers, field, mandatory = True):
    for provider in providers:
        if hasattr(provider, field):
            return provider
    if mandatory:
        fail("rustc_compile_action did not return a provider with field `{}`".format(field))
    return None

def _compile_crate(
        ctx,
        out_dir,
        crate_name,
        crate_type,
        crate_root,
        srcs,
        deps,
        compile_data = _FROZEN_EMPTY_LIST,
        proc_macro_deps = _FROZEN_EMPTY_LIST,
        edition = None,
        crate_features = _FROZEN_EMPTY_LIST,
        rustc_flags = _FROZEN_EMPTY_LIST,
        rustc_env = _FROZEN_EMPTY_DICT,
        output = None):
    """Compiles a crate with outputs owned by the calling rule.

    Args:
        ctx: Rule context.
        out_dir: Directory (relative to package) to place outputs in.
        crate_name: Name of the crate.
        crate_type: `bin` or `rlib`.
        crate_root: Crate root source file.
        srcs: Source files of the crate.
        deps: `DepVariantInfo` dependencies.
        compile_data: Files made available to the compiler.
        proc_macro_deps: `DepVariantInfo` proc-macro dependencies.
        edition: Rust edition.
        crate_features: Crate features to enable.
        rustc_flags: Additional rustc flags.
        rustc_env: Additional rustc environment variables.
        output: Output file of a `bin` crate.

    Returns:
        DepVariantInfo for the compiled crate.
    """
    if crate_type == "bin":
        output_hash = None
    else:
        # Seeds rustc's `-C metadata` / `-C extra-filename`, mirroring
        # rules_rust's `lib<name>-<hash>.rlib`.
        output_hash = repr(abs(hash(crate_root.path) + hash(out_dir)))
        output = ctx.actions.declare_file(
            "{}/lib{}-{}.rlib".format(out_dir, crate_name, output_hash),
        )

    crate_info_dict = dict(
        name = crate_name,
        type = crate_type,
        root = crate_root,
        srcs = srcs,
        deps = deps,
        proc_macro_deps = proc_macro_deps,
        aliases = _FROZEN_EMPTY_DICT,
        output = output,
        rustc_output = None,
        metadata = None,
        metadata_supports_pipelining = False,
        rustc_rmeta_output = None,
        edition = edition,
        rustc_env = rustc_env,
        rustc_env_files = _FROZEN_EMPTY_LIST,
        is_test = False,
        data = depset([]),
        compile_data = depset(compile_data),
        compile_data_targets = depset([]),
        owner = ctx.label,
        cfgs = _FROZEN_EMPTY_LIST,
    )

    providers = rustc_compile_action(
        ctx = ctx,
        attr = struct(
            name = crate_name,
            crate_features = crate_features,
            rustc_flags = rustc_flags,
            _error_format = ctx.attr._error_format,
            _is_proc_macro_dep = ctx.attr._is_proc_macro_dep,
            _is_proc_macro_dep_enabled = ctx.attr._is_proc_macro_dep_enabled,
        ),
        toolchain = ctx.toolchains["@rules_rust//rust:toolchain_type"],
        output_hash = output_hash,
        crate_info_dict = crate_info_dict,
        # Avoid conflicting baseline_coverage.dat actions across multi-crate rules.
        include_coverage = False,
    )

    # rustc_compile_action returns a CrateInfo whose rustc_env is a mutable
    # dict, which cannot go into the next crate's `deps` depset. Rebuild it with
    # frozen values; dependents never read a dependency's rustc_env.
    return rust_common.dep_variant_info(
        crate_info = rust_common.create_crate_info(
            **(crate_info_dict | {
                "deps": depset(deps),
                "proc_macro_deps": depset(proc_macro_deps),
                "rustc_env": _FROZEN_EMPTY_DICT,
                "srcs": depset(srcs),
            })
        ),
        dep_info = _find_provider(providers, "direct_crates"),
        # Only library crates carry a CcInfo.
        cc_info = _find_provider(providers, "linking_context", mandatory = False),
        build_info = None,
        crate_group_info = None,
    )

def _app_linker_script(ctx, out_dir, app_name):
    """Renders the app's linker script and returns it as a link-time dependency."""
    output = ctx.actions.declare_file("{}/{}.ld".format(out_dir, app_name))
    _render_app_template(
        ctx,
        output = output,
        templates = {
            "app": ctx.file._app_linker_script_template,
            "pigweed_linker_sections.ld.jinja": ctx.file._pigweed_linker_sections_template,
        },
        args = ["--app-name", app_name],
        mnemonic = "AppLinkerScript",
    )

    linker_input = cc_common.create_linker_input(
        owner = ctx.label,
        user_link_flags = ["-T", output.path],
        additional_inputs = depset(direct = [output]),
    )
    return rust_common.dep_variant_info(
        cc_info = CcInfo(
            linking_context = cc_common.create_linking_context(
                linker_inputs = depset(direct = [linker_input]),
            ),
        ),
        crate_info = None,
        dep_info = None,
        build_info = None,
        crate_group_info = None,
    )

def _build_app(ctx, app, out_dir, output, codegen_deps):
    """Compiles a `PwAppInfo` against the rule's `system_config` into `output`."""
    process_crates = []
    entry = None
    for process in app.processes:
        codegen_src = ctx.actions.declare_file(
            "{}/{}_{}.rs".format(out_dir, process.name, process.codegen_crate_name),
        )
        _render_app_template(
            ctx,
            output = codegen_src,
            templates = {"app": ctx.file._app_template},
            args = ["--process-name", process.name] if app.multi_process else ["--app-name", app.name],
            mnemonic = "AppCodegenSrc",
        )
        codegen = _compile_crate(
            ctx,
            out_dir = out_dir,
            crate_name = process.codegen_crate_name,
            crate_type = "rlib",
            crate_root = codegen_src,
            srcs = [codegen_src],
            deps = codegen_deps,
            edition = process.edition,
        )

        crate = dict(
            crate_name = process.crate_name,
            crate_root = process.crate_root,
            srcs = process.srcs,
            compile_data = process.compile_data,
            deps = process.deps + [codegen],
            proc_macro_deps = process.proc_macro_deps,
            edition = process.edition,
            crate_features = process.crate_features,
            rustc_flags = process.rustc_flags,
            rustc_env = process.rustc_env,
        )
        if app.multi_process:
            process_crates.append(_compile_crate(
                ctx,
                out_dir = out_dir,
                crate_type = "rlib",
                **crate
            ))
        else:
            entry = crate

    if app.multi_process:
        main_src = ctx.actions.declare_file("{}/main.rs".format(out_dir))
        _render_app_template(
            ctx,
            output = main_src,
            templates = {"app": ctx.file._multi_process_app_template},
            args = ["--app-name", app.name],
            mnemonic = "MultiProcessAppSrc",
        )
        entry = dict(
            crate_name = crate_name_for(app.name),
            crate_root = main_src,
            srcs = [main_src],
            deps = process_crates + app.deps,
            edition = _GENERATED_ENTRY_EDITION,
        )

    entry["deps"] = entry["deps"] + [_app_linker_script(ctx, out_dir, app.name)]

    _compile_crate(
        ctx,
        out_dir = out_dir,
        crate_type = "bin",
        output = output,
        **entry
    )

def _system_image_apps_impl(ctx):
    # TODO: b/579015673 - App crates compiled here are not seen by rules_rust's
    # clippy / rustfmt aspects.
    codegen_deps = dep_variant_infos([ctx.attr._syscall_defs])
    elfs = []
    for app in ctx.attr.apps:
        # Name output ELF after the app so the assembler derives matching symbol names.
        name = app[PwAppInfo].name
        out_dir = "{}/{}".format(ctx.label.name, name)
        output = ctx.actions.declare_file("{}/{}".format(out_dir, name))
        _build_app(ctx, app[PwAppInfo], out_dir, output, codegen_deps)
        elfs.append(output)
    return [DefaultInfo(files = depset(elfs))]

_system_image_apps = rule(
    implementation = _system_image_apps_impl,
    cfg = _app_target_transition,
    attrs = {
        "apps": attr.label_list(
            doc = "Apps to include in the image.",
            providers = [PwAppInfo],
        ),
        "platform": attr.label(
            doc = "Bazel platform to compile the apps for.",
            mandatory = True,
        ),
        "system_config": attr.label(
            doc = "System config file which defines the system.",
            allow_single_file = True,
            mandatory = True,
        ),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
        "_app_linker_script_template": attr.label(
            allow_single_file = True,
            default = _TEMPLATES + ":app_linker_script_template",
        ),
        "_app_template": attr.label(
            allow_single_file = True,
            default = _TEMPLATES + ":app.rs.jinja",
        ),
        "_multi_process_app_template": attr.label(
            allow_single_file = True,
            default = _TEMPLATES + ":multi_process_app.rs.jinja",
        ),
        "_pigweed_linker_sections_template": attr.label(
            allow_single_file = True,
            default = _TEMPLATES + ":pigweed_linker_sections.ld.jinja",
        ),
        "_syscall_defs": attr.label(
            doc = "Dependency of the generated system config crates.",
            default = "@pigweed//pw_kernel/syscall:syscall_defs",
        ),
        "_system_generator": attr.label(
            executable = True,
            cfg = "exec",
            default = "@pigweed//pw_kernel/tooling/system_generator:system_generator_bin",
        ),
    } | RUSTC_ATTRS,
    fragments = ["cpp"],
    toolchains = ["@rules_rust//rust:toolchain_type"] + use_cpp_toolchain(mandatory = False),
    doc = "Compiles the apps of a system image against its system_config: renders each " +
          "app's system config crate and linker script, then compiles and links its crates " +
          "with rules_rust's compile action.",
)

def _system_image_impl(ctx):
    output_elf = ctx.actions.declare_file(ctx.attr.name + ".elf")

    args = [
        "--kernel",
        ctx.file.kernel.path,
        "--output",
        output_elf.path,
    ]
    for f in ctx.files.apps:
        args.append("--app")
        args.append(f.path)

    ctx.actions.run(
        inputs = ctx.files.kernel + ctx.files.apps,
        outputs = [output_elf],
        executable = ctx.executable._system_assembler,
        mnemonic = "SystemImageAssembler",
        arguments = args,
    )

    output_bin = ctx.actions.declare_file(ctx.attr.name + ".bin")
    run_action_on_executable(
        ctx = ctx,
        # TODO: https://github.com/bazelbuild/rules_cc/issues/292 - Add a helper
        # to rules cc to make it possible to get this from ctx.attr._objcopy.
        action_name = ACTION_NAMES.objcopy_embed_data,
        action_args = "{args} {input} {output}".format(
            args = "-Obinary",
            input = output_elf.path,
            output = output_bin.path,
        ),
        inputs = [output_elf],
        output = output_bin,
        additional_outputs = [],
        output_executable = True,
    )

    return [
        DefaultInfo(executable = output_elf, files = depset([output_elf, output_bin])),
        SystemImageInfo(bin = output_bin, elf = output_elf),
    ]

_system_image = rule(
    implementation = _system_image_impl,
    executable = True,
    attrs = {
        "apps": attr.label(
            doc = "The `<name>.apps` target that compiles this image's apps.",
            allow_files = True,
        ),
        "kernel": attr.label(
            doc = "Kernel image.",
            allow_single_file = True,
            mandatory = True,
            cfg = _target_transition,
        ),
        "platform": attr.label(
            doc = "Bazel platform to compile image for.",
            mandatory = True,
        ),
        "userspace": attr.bool(
            doc = "Whether to include userspace support in the kernel.",
            default = True,
        ),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
        "_system_assembler": attr.label(
            executable = True,
            cfg = "exec",
            default = "@pigweed//pw_kernel/tooling:system_assembler",
        ),
    },
    toolchains = use_cpp_toolchain(),
    fragments = ["cpp"],
    doc = "Assemble a system image combining apps and kernel.",
)

def system_image(name, kernel, platform, apps = None, system_config = None, userspace = True, **kwargs):
    """Assembles a bootable system image containing the kernel and applications.

    Args:
        name: Target name for the system image.
        kernel: Kernel binary target.
        platform: Platform target the system image and apps are built for.
        apps: Optional list of `rust_app` or `multi_process_app` targets to include.
        system_config: System configuration file the `apps` are compiled
            against; required if `apps` is provided. The kernel receives its
            config via `target_codegen` / `target_linker_script`.
        userspace: Whether to enable userspace support in the kernel.
        **kwargs: Target attributes (`tags`, `visibility`, etc.).
    """
    target_compatible_with = kwargs.pop("target_compatible_with", [])

    if userspace:
        target_compatible_with = target_compatible_with + select({
            "@pigweed//pw_kernel/userspace:no_userspace_support": ["@platforms//:incompatible"],
            "//conditions:default": [],
        })

    apps_target = None
    if apps:
        if system_config == None:
            fail("system_image `{}`: `system_config` is required when `apps` is set".format(name))
        apps_target = name + ".apps"
        _system_image_apps(
            name = apps_target,
            apps = apps,
            platform = platform,
            tags = kwargs.get("tags", []) + ["manual"],
            target_compatible_with = target_compatible_with,
            testonly = kwargs.get("testonly", False),
            visibility = ["//visibility:private"],
            system_config = system_config,
        )

    _system_image(
        name = name,
        apps = apps_target,
        kernel = kernel,
        platform = platform,
        userspace = userspace,
        target_compatible_with = target_compatible_with,
        **kwargs
    )

def _system_image_test_impl(ctx):
    executable_symlink = ctx.actions.declare_file(ctx.label.name)
    ctx.actions.symlink(
        output = executable_symlink,
        target_file = ctx.attr.image[SystemImageInfo].elf,
    )

    return [
        DefaultInfo(
            executable = executable_symlink,
            runfiles = ctx.runfiles(
                files = [
                    ctx.attr.image[SystemImageInfo].elf,
                    ctx.attr.image[SystemImageInfo].bin,
                ],
            ).merge(ctx.attr.image[DefaultInfo].default_runfiles),
        ),
    ]

system_image_test = rule(
    implementation = _system_image_test_impl,
    test = True,
    attrs = {
        "image": attr.label(
            doc = "The system_image target to test.",
            mandatory = True,
            providers = [DefaultInfo, SystemImageInfo],
            executable = True,
            cfg = "target",
        ),
    },
    doc = "Defines a test for a system_image target.",
)
