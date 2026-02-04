# Copyright 2024 The Pigweed Authors
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
"""Private templates for generating toolchain repos."""

_rust_toolchain_no_prebuilt_template = """\
rust_toolchain(
    name = "{name}_rust_toolchain",
    binary_ext = "",
    cargo = "{toolchain_repo}//:bin/cargo",
    clippy_driver = "{toolchain_repo}//:bin/clippy-driver",
    dylib_ext = "{dylib_ext}",
    exec_compatible_with = {exec_compatible_with},
    exec_triple = "{exec_triple}",
    opt_level = {{
      "dbg": "s",
      "fastbuild": "s",
      "opt": "s",
    }},
    rust_doc = "{toolchain_repo}//:bin/rustdoc",
    {default_edition_setting}
    rust_std = select({{
        "@pigweed//pw_toolchain/rust:stdlibs_none": "{toolchain_repo}//:rust_libs_none",
        "@pigweed//pw_toolchain/rust:stdlibs_core_only": "{toolchain_repo}//:rust_libs_core_only",
        "@pigweed//pw_toolchain/rust:stdlibs_core_alloc": "{toolchain_repo}//:rust_libs_core_alloc",
        "@pigweed//pw_toolchain/rust:stdlibs_core_alloc_std": "{toolchain_repo}//:rust_libs_core_alloc_std",
        "//conditions:default": "{toolchain_repo}//:rust_libs_core",
    }}),
    rustc = "{toolchain_repo}//:bin/rustc",
    rustc_lib = "{toolchain_repo}//:rustc_lib",
    staticlib_ext = ".a",
    stdlib_linkflags = [],
    target_compatible_with = {target_compatible_with},
    target_triple = "{target_triple}",
    extra_rustc_flags = {extra_rustc_flags},
    extra_exec_rustc_flags = {extra_rustc_flags},
    # TODO: https://pwbug.dev/342695883 - Works around confusing
    # target_compatible_with semantics in rust_toolchain. Figure out how to
    # do better.
    tags = ["manual"],
)
"""

def rust_toolchain_no_prebuilt_template(
        name,
        exec_triple,
        target_triple,
        toolchain_repo,
        dylib_ext,
        exec_compatible_with,
        target_compatible_with,
        extra_rustc_flags,
        default_edition = None):
    default_edition_setting = ""
    if default_edition:
        default_edition_setting = 'default_edition = "{}",'.format(default_edition)

    return _rust_toolchain_no_prebuilt_template.format(
        name = name,
        exec_triple = exec_triple,
        target_triple = target_triple,
        toolchain_repo = toolchain_repo,
        dylib_ext = dylib_ext,
        exec_compatible_with = json.encode(exec_compatible_with),
        target_compatible_with = json.encode(target_compatible_with),
        extra_rustc_flags = json.encode(extra_rustc_flags),
        default_edition_setting = default_edition_setting,
    )

_rust_toolchain_template = """\
rust_toolchain(
    name = "{name}_rust_toolchain",
    binary_ext = "",
    cargo = "{toolchain_repo}//:bin/cargo",
    clippy_driver = "{toolchain_repo}//:bin/clippy-driver",
    dylib_ext = "{dylib_ext}",
    exec_compatible_with = {exec_compatible_with},
    exec_triple = "{exec_triple}",
    opt_level = {{
      "dbg": "s",
      "fastbuild": "s",
      "opt": "s",
    }},
    rust_doc = "{toolchain_repo}//:bin/rustdoc",
    {default_edition_setting}
    rust_std = "{target_repo}//:rust_std",
    rustc = "{toolchain_repo}//:bin/rustc",
    rustc_lib = "{toolchain_repo}//:rustc_lib",
    staticlib_ext = ".a",
    stdlib_linkflags = [],
    target_compatible_with = {target_compatible_with},
    target_triple = "{target_triple}",
    extra_rustc_flags = {extra_rustc_flags},
    extra_exec_rustc_flags = {extra_rustc_flags},
    # TODO: https://pwbug.dev/342695883 - Works around confusing
    # target_compatible_with semantics in rust_toolchain. Figure out how to
    # do better.
    tags = ["manual"],
)
"""

def rust_toolchain_template(
        name,
        exec_triple,
        target_triple,
        toolchain_repo,
        target_repo,
        dylib_ext,
        exec_compatible_with,
        target_compatible_with,
        extra_rustc_flags,
        default_edition = None):
    default_edition_setting = ""
    if default_edition:
        default_edition_setting = 'default_edition = "{}",'.format(default_edition)

    return _rust_toolchain_template.format(
        name = name,
        exec_triple = exec_triple,
        target_triple = target_triple,
        toolchain_repo = toolchain_repo,
        target_repo = target_repo,
        dylib_ext = dylib_ext,
        exec_compatible_with = json.encode(exec_compatible_with),
        target_compatible_with = json.encode(target_compatible_with),
        extra_rustc_flags = json.encode(extra_rustc_flags),
        default_edition_setting = default_edition_setting,
    )

_toolchain_template = """\
toolchain(
        name = "{name}",
        exec_compatible_with = {exec_compatible_with},
        target_compatible_with = {target_compatible_with},
        target_settings = {target_settings},
        toolchain = ":{name}_rust_toolchain",
        toolchain_type = "@rules_rust//rust:toolchain",
    )
"""

def toolchain_template(
        name,
        exec_compatible_with,
        target_compatible_with,
        target_settings):
    return _toolchain_template.format(
        name = name,
        exec_compatible_with = json.encode(exec_compatible_with),
        target_compatible_with = json.encode(target_compatible_with),
        target_settings = json.encode(target_settings),
    )

_rust_analyzer_toolchain_template = """\
rust_analyzer_toolchain(
    name = "{name}_rust_analyzer_toolchain",
    exec_compatible_with = {exec_compatible_with},
    proc_macro_srv = "{toolchain_repo}//:libexec/rust-analyzer-proc-macro-srv",
    rustc = "{toolchain_repo}//:bin/rustc",
    rustc_srcs = "{toolchain_repo}//:rustc_srcs",
    rustc_srcs_path = "lib/rustlib/src/rust/library",
    target_compatible_with = {target_compatible_with},
    visibility = ["//visibility:public"],
)

toolchain(
    name = "{name}",
    exec_compatible_with = {exec_compatible_with},
    target_compatible_with = {target_compatible_with},
    target_settings = {target_settings},
    toolchain = ":{name}_rust_analyzer_toolchain",
    toolchain_type = "@rules_rust//rust/rust_analyzer:toolchain_type",
)
"""

def rust_analyzer_toolchain_template(
        name,
        toolchain_repo,
        exec_compatible_with,
        target_compatible_with,
        target_settings):
    return _rust_analyzer_toolchain_template.format(
        name = name,
        toolchain_repo = toolchain_repo,
        exec_compatible_with = json.encode(exec_compatible_with),
        target_compatible_with = json.encode(target_compatible_with),
        target_settings = json.encode(target_settings),
    )

_rustfmt_toolchain_template = """\
rustfmt_toolchain(
    name = "{name}_rustfmt_toolchain",
    exec_compatible_with = {exec_compatible_with},
    rustc = "{toolchain_repo}//:bin/rustc",
    rustfmt = "{toolchain_repo}//:bin/rustfmt",
    rustc_lib = "{toolchain_repo}//:rustc_lib",
    target_compatible_with = {target_compatible_with},
    visibility = ["//visibility:public"],
)

toolchain(
    name = "{name}",
    exec_compatible_with = {exec_compatible_with},
    target_compatible_with = {target_compatible_with},
    target_settings = {target_settings},
    toolchain = ":{name}_rustfmt_toolchain",
    toolchain_type = "@rules_rust//rust/rustfmt:toolchain_type",
)
"""

def rustfmt_toolchain_template(
        name,
        toolchain_repo,
        exec_compatible_with,
        target_compatible_with,
        target_settings):
    return _rustfmt_toolchain_template.format(
        name = name,
        toolchain_repo = toolchain_repo,
        exec_compatible_with = json.encode(exec_compatible_with),
        target_compatible_with = json.encode(target_compatible_with),
        target_settings = json.encode(target_settings),
    )
