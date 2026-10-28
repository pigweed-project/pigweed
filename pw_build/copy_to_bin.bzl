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
"""Copies a source file to bazel-bin at the same workspace-relative path."""

# buildifier: disable=bzl-visibility
load("@bazel_skylib//rules/private:copy_common.bzl", "OsInfo")

# buildifier: disable=bzl-visibility
load("@bazel_skylib//rules/private:copy_file_private.bzl", "copy_bash", "copy_cmd")

def _copy_to_bin_impl(ctx):
    all_dst = []
    for src in ctx.files.srcs:
        dst = ctx.actions.declare_file(src.basename, sibling = src)
        if ctx.attr._exec_is_windows[OsInfo].is_windows:
            copy_cmd(ctx, src, dst)
        else:
            copy_bash(ctx, src, dst)
        all_dst.append(dst)
    return DefaultInfo(files = depset(all_dst), runfiles = ctx.runfiles(files = all_dst))

_copy_to_bin = rule(
    implementation = _copy_to_bin_impl,
    attrs = {
        "srcs": attr.label_list(mandatory = True, allow_files = True),
        "_exec_is_windows": attr.label(
            default = "@bazel_skylib//rules/private:is_windows",
            cfg = "exec",
        ),
    },
)

def copy_to_bin(name, srcs, **kwargs):
    """Copies a source file to bazel-bin at the same workspace-relative path.

    Although aspect_bazel_libs includes a copy_to_bin but we cannot use
    it for now since some downstream projects don't use bzlmods and don't
    load aspect libs at all. Until then, we keep a local copy of this here.
    We only need copy_to_bin due to unusual limitations of rules_js and Node,
    see https://github.com/aspect-build/rules_js/blob/main/docs/README.md#javascript

    e.g. `<workspace_root>/pw_web/main.ts -> <bazel-bin>/pw_web/main.ts`

    Args:
        name: Name of the rule.
        srcs: A List of Labels. File(s) to to copy.
        **kwargs: further keyword arguments, e.g. `visibility`
    """
    _copy_to_bin(
        name = name,
        srcs = srcs,
        **kwargs
    )
