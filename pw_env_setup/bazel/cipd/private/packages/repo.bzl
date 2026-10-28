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
"""Internal repository rules for CIPD module extension."""

load("@bazel_tools//tools/build_defs/repo:utils.bzl", "patch")
load("//pw_env_setup/bazel/cipd/private/versions:classify.bzl", "is_immutable_version")

_CIPD_CACHE_DIR_ENV_VAR = "CIPD_CACHE_DIR"
_DEFAULT_CIPD_CACHE_DIR = "~/.cipd-cache-dir"
_DEFAULT_CIPD_BINARY = "@cipd_client//:cipd"

# TODO(b/234874582) Does windows need to be a verified platform?
_ENSURE_FILE_TEMPLATE = """
$VerifiedPlatform linux-amd64
$VerifiedPlatform mac-arm64
$ParanoidMode CheckPresence
@Subdir
"""

_BUILD_FILE_TEMPLATE = """
exports_files(
    glob(["**"]),
    visibility = ["//visibility:public"],
)

filegroup(
    name = "all",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)
"""

def _get_cipd_cache_dir(rctx):
    """Resolves the cache directory to use.

    Args:
        rctx: Repository context.

    Returns:
        The cache directory, or None if undetermined.
    """
    cipd_cache_dir = rctx.getenv(_CIPD_CACHE_DIR_ENV_VAR)
    if cipd_cache_dir != None:
        return cipd_cache_dir

    if "windows" in rctx.os.name:
        user_home = rctx.getenv("USERPROFILE")
    else:
        user_home = rctx.getenv("HOME")

    if user_home == None:
        return None

    return _DEFAULT_CIPD_CACHE_DIR.replace("~", user_home)

def _generate_ensure_file_for_packages(package_version_map):
    ensure_content = _ENSURE_FILE_TEMPLATE
    for pkg, version in package_version_map.items():
        ensure_content += "{} {}\n".format(pkg, version)
    return ensure_content

def _ensure_packages(rctx, package_version_map):
    ensure_path = rctx.name + ".ensure"

    rctx.file(ensure_path, _generate_ensure_file_for_packages(package_version_map))

    args = [
        rctx.path(rctx.attr.cipd_bin),
        "ensure",
        "-root",
        ".",
        "-ensure-file",
        ensure_path,
    ]

    cipd_cache_dir = _get_cipd_cache_dir(rctx)
    if cipd_cache_dir:
        args.extend(["-cache-dir", cipd_cache_dir])

    result = rctx.execute(args)

    if result.return_code != 0:
        fail("Failed to ensure the CIPD packages for the repo `{}`:\n{}".format(rctx.name, result.stderr))

def _cipd_repo_impl(rctx):
    _ensure_packages(rctx, rctx.attr.packages)

    patch(rctx)

    if rctx.attr.build_file:
        rctx.file("BUILD", rctx.read(rctx.attr.build_file))
    elif not rctx.path("BUILD").exists and not rctx.path("BUILD.bazel").exists:
        rctx.file("BUILD", _BUILD_FILE_TEMPLATE)

    reproducible = True
    for version in rctx.attr.packages.values():
        reproducible = is_immutable_version(version) and reproducible

    # rctx.repo_metadata is only available in Bazel>=8.3.0
    if hasattr(rctx, "repo_metadata"):
        return rctx.repo_metadata(reproducible = reproducible)
    return None

def _cipd_stub_repo_impl(rctx):
    rctx.file("BUILD", _BUILD_FILE_TEMPLATE)

    # rctx.repo_metadata is only available in Bazel>=8.3.0
    if hasattr(rctx, "repo_metadata"):
        return rctx.repo_metadata(reproducible = True)
    return None

package_repo = repository_rule(
    implementation = _cipd_repo_impl,
    attrs = {
        "build_file": attr.label(
            allow_single_file = True,
            doc = "If specified, overrides the BUILD file in the repo with the content of this file.",
        ),
        "cipd_bin": attr.label(
            default = _DEFAULT_CIPD_BINARY,
            doc = "Location of the CIPD client binary.",
        ),
        "packages": attr.string_dict(
            mandatory = True,
            doc = "Map of CIPD package to versions to install in the repo.",
        ),
        "patch_args": attr.string_list(
            doc = "Arguments to pass to patch.",
        ),
        "patches": attr.label_list(
            doc = "Patches to apply to the repo.",
        ),
    },
    doc = "Creates a repository for a set of CIPD packages.",
)

stub_package_repo = repository_rule(
    implementation = _cipd_stub_repo_impl,
    attrs = {},
    doc = "Creates an stub repository.",
)
