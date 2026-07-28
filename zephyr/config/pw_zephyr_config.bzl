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

"""A Pigweed extension module for configuring Pigweed to use pigweed-zephyr.

A Bazel module that uses both Pigweed and Zephyr-Bazel should put this its
MODULE.bazel:

    pw_zephyr_config = use_extension(
        "@pigweed//zephyr/config:pw_zephyr_config.bzl",
        "pw_zephyr_config"
    )
    override_repo(
        pw_zephyr_config,
        "zephyr-bazel",
        "zephyr",
        "kconfig",
        "cmsis_6"
    )

This is in addition to the setup needed by zephyr-bazel to define those repos.
"""

def _symlinked_repo_impl(rctx):
    for path, label in rctx.attr.files.items():
        src_path = rctx.path(label)

        # On Windows `ctx.symlink` may be implemented as a copy, so the file MUST be watched
        rctx.watch(src_path)
        if not src_path.exists:
            fail("Input %s does not exist" % label)
        if rctx.path(path).exists:
            rctx.delete(path)
        rctx.symlink(src_path, path)

symlinked_repo = repository_rule(
    implementation = _symlinked_repo_impl,
    attrs = {"files": attr.string_keyed_label_dict()},
)

def _pw_zephyr_config_impl(mctx):
    default_files = {
        "BUILD.bazel": Label("//zephyr/config/stubs:stub_BUILD"),
        "REPO.bazel": Label("//zephyr/config/stubs:stub_REPO"),
    }

    # Set up minimal skeletons for each of the zephyr-bazel internal repos.
    symlinked_repo(name = "kconfig", files = default_files)

    symlinked_repo(
        name = "zephyr",
        files = default_files | {
            "cc.bzl": Label("//zephyr/config/stubs:stub_cc.bzl"),
            "cc_test.bzl": Label("//zephyr/config/stubs:stub_cc_test.bzl"),
            "modules/cmsis_6/BUILD.bazel": Label("//zephyr/config/stubs:stub_BUILD"),
        },
    )
    symlinked_repo(name = "zephyr_version", files = default_files)

    return mctx.extension_metadata(reproducible = True)

pw_zephyr_config = module_extension(implementation = _pw_zephyr_config_impl)
