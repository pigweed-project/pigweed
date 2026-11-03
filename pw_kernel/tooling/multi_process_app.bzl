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
"""Rule for defining multi-process userspace applications."""

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

def _multi_process_app_impl(ctx):
    return [PwAppInfo(
        name = ctx.label.name,
        processes = [process[PwProcessInfo] for process in ctx.attr.processes],
        multi_process = True,
        deps = dep_variant_infos(ctx.attr.deps + ctx.attr._entry_deps),
    )]

_multi_process_app = rule(
    implementation = _multi_process_app_impl,
    attrs = {
        "deps": attr.label_list(
            doc = "Additional dependencies of the generated entry point.",
            providers = [[rust_common.crate_info], [rust_common.crate_group_info], [CcInfo]],
        ),
        "processes": attr.label_list(
            doc = "Processes making up the app.",
            providers = [PwProcessInfo],
            mandatory = True,
            allow_empty = False,
        ),
        "_entry_deps": attr.label_list(
            doc = "Dependencies of the generated entry point.",
            default = [
                "@pigweed//pw_kernel/userspace",
                "@pigweed//pw_kernel/syscall:syscall_user",
                "@pigweed//pw_status/rust:pw_status",
            ],
        ),
    },
    provides = [PwAppInfo],
    doc = "A multi-process userspace app, compiled by the system images including it.",
)

def multi_process_app(name, processes, **kwargs):
    """Defines a userspace multi-process application.

    Bundles multiple `rust_process` targets into a single application binary
    with a generated entry point. List the app in a `system_image`'s `apps`
    attribute to build it as part of that image.

    Args:
        name: Target name and name of the app in the system configuration.
        processes: List of `rust_process` targets to include in the app.
        **kwargs: Optional `deps` for the generated entry point and standard
            target attributes (`tags`, `visibility`, ...).
    """
    restrict_to_app_builds(kwargs)
    crate_name_for(name)
    _multi_process_app(
        name = name,
        processes = processes,
        **kwargs
    )
