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
"""Providers describing userspace apps, shared by the app rules and system_image."""

load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load("@rules_rust//rust:defs.bzl", "rust_common")

PwProcessInfo = provider(
    doc = "Describes a userspace process to be compiled against a system config.",
    fields = {
        "codegen_crate_name": "str: Crate name the sources expect for the generated system config crate.",
        "compile_data": "list[File]: Files available to the compiler, e.g. for `include_str!`.",
        "crate_features": "list[str]: Crate features to enable.",
        "crate_name": "str: Crate name of the compiled process crate.",
        "crate_root": "File: Crate root source file.",
        "deps": "list[DepVariantInfo]: Direct dependencies.",
        "edition": "str: Rust edition.",
        "name": "str: Name of the process (or single-process app) in the system config.",
        "proc_macro_deps": "list[DepVariantInfo]: Direct proc-macro dependencies.",
        "rustc_env": "dict[str, str]: Additional environment variables for rustc.",
        "rustc_flags": "list[str]: Additional rustc flags.",
        "srcs": "list[File]: Source files of the crate.",
    },
)

PwAppInfo = provider(
    doc = "Describes a userspace application to be compiled against a system config.",
    fields = {
        "deps": "list[DepVariantInfo]: Dependencies of the generated entry point (multi-process apps only).",
        "multi_process": "bool: Whether the app's entry point is generated from its processes.",
        "name": "str: Name of the app in the system config.",
        "processes": "list[PwProcessInfo]: Processes making up the app.",
    },
)

_RUST_KEYWORDS = {
    "as": True,
    "async": True,
    "await": True,
    "break": True,
    "const": True,
    "continue": True,
    "crate": True,
    "dyn": True,
    "else": True,
    "enum": True,
    "extern": True,
    "false": True,
    "fn": True,
    "for": True,
    "if": True,
    "impl": True,
    "in": True,
    "let": True,
    "loop": True,
    "match": True,
    "mod": True,
    "move": True,
    "mut": True,
    "pub": True,
    "ref": True,
    "return": True,
    "self": True,
    "static": True,
    "struct": True,
    "super": True,
    "trait": True,
    "true": True,
    "try": True,
    "type": True,
    "unsafe": True,
    "use": True,
    "where": True,
    "while": True,
}

def crate_name_for(name):
    """Crate name of an app or process; must match the system generator's `to_lower_ident`.

    Args:
        name: Name of the app or process to convert.

    Returns:
        The lowercased crate name as a string.
    """
    if not name:
        fail("App or process name cannot be empty")
    chars = name.elems()
    if not (chars[0].isalpha() or chars[0] == "_"):
        fail("Invalid app or process name `{}`: must start with a letter or underscore".format(name))
    for c in chars[1:]:
        if not (c.isalnum() or c == "_"):
            fail("Invalid app or process name `{}`: must contain only letters, numbers, and underscores".format(name))
    lower = name.lower()
    if lower in _RUST_KEYWORDS:
        fail("Invalid app or process name `{}`: `{}` is a reserved Rust keyword".format(name, lower))
    return lower

def _provider_or_none(target, provider):
    return target[provider] if provider in target else None

def dep_variant_infos(targets):
    """Converts Rust / C++ dependency targets into rules_rust `DepVariantInfo`s.

    Args:
        targets: Targets from a `deps` or `proc_macro_deps` attribute.

    Returns:
        list[DepVariantInfo]
    """
    return [
        rust_common.dep_variant_info(
            crate_info = _provider_or_none(target, rust_common.crate_info),
            dep_info = _provider_or_none(target, rust_common.dep_info),
            cc_info = _provider_or_none(target, CcInfo),
            crate_group_info = _provider_or_none(target, rust_common.crate_group_info),
            build_info = None,
        )
        for target in targets
    ]

def restrict_to_app_builds(kwargs):
    """Marks a target incompatible outside system image app configurations.

    Args:
        kwargs: Rule attributes; `target_compatible_with` is updated in place.
    """
    target_compatible_with = kwargs.get("target_compatible_with") or []
    kwargs["target_compatible_with"] = target_compatible_with + select({
        "@pigweed//pw_kernel/userspace:is_app_build_enabled": [],
        "//conditions:default": ["@platforms//:incompatible"],
    })
