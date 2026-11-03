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
"""Rules for defining userspace kernel applications.

Userspace applications run on top of the kernel and are compiled by the
`system_image` that includes them, against that image's system configuration.

* `rust_app`: Defines a single-process userspace application. Add it to a
  `system_image`'s `apps` attribute to include it in the image.
* `rust_process`: Defines a process to be included in a `multi_process_app`.
"""

load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load("@rules_rust//rust:defs.bzl", "rust_common")
load(
    "//pw_kernel/tooling:app_info.bzl",
    "PwAppInfo",
    "PwProcessInfo",
    "crate_name_for",
    "dep_variant_infos",
    "restrict_to_app_builds",
)

def _crate_root(ctx, crate_type):
    if ctx.file.crate_root:
        return ctx.file.crate_root
    if len(ctx.files.srcs) == 1:
        return ctx.files.srcs[0]
    default = "main.rs" if crate_type == "bin" else "lib.rs"
    for basename in [default, ctx.label.name + ".rs"]:
        for src in ctx.files.srcs:
            if src.basename == basename:
                return src
    fail("{}: `srcs` has no {} or {}.rs; set `crate_root`".format(ctx.label, default, ctx.label.name))

def _process_info(ctx, crate_type, crate_name = None):
    return PwProcessInfo(
        name = ctx.label.name,
        crate_name = crate_name_for(crate_name or ctx.label.name),
        codegen_crate_name = ctx.attr.codegen_crate_name,
        crate_root = _crate_root(ctx, crate_type),
        srcs = ctx.files.srcs,
        compile_data = ctx.files.compile_data,
        deps = dep_variant_infos(ctx.attr.deps),
        proc_macro_deps = dep_variant_infos(ctx.attr.proc_macro_deps),
        edition = ctx.attr.edition,
        crate_features = ctx.attr.crate_features,
        rustc_flags = ctx.attr.rustc_flags,
        rustc_env = ctx.attr.rustc_env,
    )

def _rust_app_impl(ctx):
    process = _process_info(ctx, "bin", crate_name = ctx.attr.crate_name)
    return [PwAppInfo(
        name = process.name,
        processes = [process],
        multi_process = False,
        deps = [],
    )]

def _rust_process_impl(ctx):
    return [_process_info(ctx, "lib")]

_CRATE_ATTRS = {
    "codegen_crate_name": attr.string(
        doc = "Crate name the sources expect for the generated system config crate.",
        mandatory = True,
    ),
    "compile_data": attr.label_list(
        doc = "Files made available to the compiler, e.g. for `include_str!`.",
        allow_files = True,
    ),
    "crate_features": attr.string_list(
        doc = "List of features to enable for this crate.",
    ),
    "crate_root": attr.label(
        doc = "The crate root source file. Defaults to `main.rs` / `lib.rs` or `<name>.rs` in `srcs`.",
        allow_single_file = [".rs"],
    ),
    "deps": attr.label_list(
        doc = "Rust and C/C++ library dependencies.",
        providers = [[rust_common.crate_info], [rust_common.crate_group_info], [CcInfo]],
    ),
    "edition": attr.string(
        doc = "The Rust edition to use.",
        mandatory = True,
    ),
    "proc_macro_deps": attr.label_list(
        doc = "Procedural macro dependencies.",
        cfg = "exec",
        providers = [[rust_common.crate_info], [rust_common.crate_group_info]],
    ),
    "rustc_env": attr.string_dict(
        doc = "Additional environment variables to set for rustc.",
    ),
    "rustc_flags": attr.string_list(
        doc = "Additional flags to pass to rustc.",
    ),
    "srcs": attr.label_list(
        doc = "Source files of the crate.",
        allow_files = [".rs"],
        mandatory = True,
    ),
}

_rust_app = rule(
    implementation = _rust_app_impl,
    attrs = _CRATE_ATTRS | {
        "crate_name": attr.string(
            doc = "Crate name of the app binary. Defaults to the lowercased target name. " +
                  "Only affects symbol names; the app's name in the system config is always `name`.",
        ),
    },
    provides = [PwAppInfo],
    doc = "A single-process userspace app, compiled by the system images including it.",
)

_rust_process = rule(
    implementation = _rust_process_impl,
    attrs = _CRATE_ATTRS,
    provides = [PwProcessInfo],
    doc = "A userspace process of a `multi_process_app`.",
)

def rust_app(name, codegen_crate_name, srcs, deps = None, **kwargs):
    """Defines a single-process userspace application.

    List the app in a `system_image`'s `apps` attribute to build it as part of
    that image.

    Args:
        name: Target name and name of the app in the system configuration.
        codegen_crate_name: Name of the generated system config crate.
        srcs: Source files for the application.
        deps: Library dependencies for the application.
        **kwargs: `edition` (required), `compile_data`, `crate_features`,
            `crate_name`, `crate_root`, `proc_macro_deps`, `rustc_env`,
            `rustc_flags` and standard target attributes (`tags`,
            `visibility`, ...).
    """
    restrict_to_app_builds(kwargs)
    crate_name_for(name)
    _rust_app(
        name = name,
        codegen_crate_name = codegen_crate_name,
        srcs = srcs,
        deps = deps or [],
        **kwargs
    )

def rust_process(name, codegen_crate_name, srcs, deps = None, **kwargs):
    """Defines a userspace process for the `processes` attribute of a `multi_process_app`.

    Args:
        name: Target name and name of the process in the system configuration.
        codegen_crate_name: Name of the generated system config crate.
        srcs: Source files for the process.
        deps: Library dependencies for the process.
        **kwargs: `edition` (required), `compile_data`, `crate_features`,
            `crate_root`, `proc_macro_deps`, `rustc_env`, `rustc_flags` and
            standard target attributes (`tags`, `visibility`, ...).
    """
    restrict_to_app_builds(kwargs)
    crate_name_for(name)
    _rust_process(
        name = name,
        codegen_crate_name = codegen_crate_name,
        srcs = srcs,
        deps = deps or [],
        **kwargs
    )
