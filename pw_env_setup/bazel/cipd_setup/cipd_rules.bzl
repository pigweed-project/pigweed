# Copyright 2021 The Pigweed Authors
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
"""Bazel rules for downloading CIPD packages."""

load(
    "//pw_env_setup/bazel/cipd_setup/internal:cipd_internal.bzl",
    _cipd_client_impl = "cipd_client_impl",
    _cipd_composite_repository_impl = "cipd_composite_repository_impl",
    _cipd_repository_impl = "cipd_repository_impl",
)

_cipd_client_repository = repository_rule(
    _cipd_client_impl,
    attrs = {
        "_cipd_digest_file": attr.label(default = str(Label("//pw_env_setup:py/pw_env_setup/cipd_setup/.cipd_version.digests"))),
        "_cipd_version_file": attr.label(default = str(Label("//pw_env_setup:py/pw_env_setup/cipd_setup/.cipd_version"))),
    },
    doc = """
Fetches the cipd client.

This rule should not be used directly and instead should be called via
the cipd_client_repository macro.
""",
)

def cipd_client_repository():
    """Fetches the cipd client.

    Fetches the cipd client to the prescribed remote repository target
    prefix 'cipd_client'. This rule should be called before a
    cipd_repository rule is instantiated.
    """
    _cipd_client_repository(
        name = "cipd_client",
    )

cipd_repository = repository_rule(
    _cipd_repository_impl,
    attrs = {
        "build_file": attr.label(
            allow_single_file = True,
            doc = "Override the BUILD file in the new CIPD repository.",
        ),
        "patch_args": attr.string_list(
            doc = "Arguments to pass to the patch tool. List of strings.",
            default = [],
        ),
        "patches": attr.label_list(
            doc = "A list of patches to apply to the CIPD package after downloading it",
            default = [],
        ),
        "path": attr.string(
            doc = "Path within CIPD where this repository lives.",
        ),
        "tag": attr.string(
            doc = "Tag specifying which version of the repository to fetch.",
        ),
        "tag_by_os": attr.string_dict(
            doc = "Dict from OS name (mac, linux, windows) to tag. Incompatible with the 'tag' attribute.",
        ),
        "_cipd_client": attr.label(
            default = "@cipd_client//:cipd",
            doc = "Location of the CIPD client binary (internal).",
        ),
    },
    doc = """
Downloads a singular CIPD dependency to the root of a remote repository.

Example:

    load(
        "@pigweed//pw_env_setup/bazel/cipd_setup:cipd_rules.bzl",
        "cipd_client_repository",
        "cipd_repository",
    )

    # Must be called before cipd_repository
    cipd_client_repository()

    cipd_repository(
        name = "bloaty",
        path = "fuchsia/third_party/3pp/bloaty/${os=linux,mac}-${arch=amd64}",
        tag = "version:2@4a601b636e2347322d0371c8bf8ca5eaeaca4bac",
    )
""",
)

cipd_composite_repository = repository_rule(
    _cipd_composite_repository_impl,
    attrs = {
        "build_file": attr.label(
            allow_single_file = True,
            doc = "Override the BUILD file in the new CIPD repository.",
        ),
        "packages": attr.string_list(
            doc = "List of paths within CIPD where repositories lives.",
            default = [],
        ),
        "patch_args": attr.string_list(
            doc = "Arguments to pass to the patch tool. List of strings.",
            default = [],
        ),
        "patches": attr.label_list(
            doc = "A list of patches to apply to the CIPD package after downloading it",
            default = [],
        ),
        "tag": attr.string(
            doc = "Tag specifying which version of the repository to fetch.",
        ),
        "tag_by_os": attr.string_dict(
            doc = "Dict from OS name (mac, linux, windows) to tag. Incompatible with the 'tag' attribute.",
        ),
        "_cipd_client": attr.label(
            default = "@cipd_client//:cipd",
            doc = "Location of the CIPD client binary (internal).",
        ),
    },
    doc = """
Downloads multiple CIPD dependencies to the root of a remote repository.

Example:

    load(
        "@pigweed//pw_env_setup/bazel/cipd_setup:cipd_rules.bzl",
        "cipd_client_repository",
        "cipd_composite_repository",
    )

    # Must be called before cipd_composite_repository
    cipd_client_repository()

    cipd_composite_repository(
        name = "llvm_toolchain",
        packages = [
            "fuchsia/third_party/clang/${os}-${arch}",
            "fuchsia/third_party/clang/target/riscv32-unknown-elf",
	],
        tag = "git_revision:03b0f55d9c6319a851a60bb084faca0e32a38f2b",
    )
""",
)
