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

"""Helpers for dealing with the `cipd` client tool."""

load("//pw_env_setup/bazel/cipd/private/client:digest_parser.bzl", "get_digest")
load("//pw_env_setup/bazel/cipd/private/platforms:platforms.bzl", "get_cipd_platform")

_CLIENT_DOWNLOAD_URL_TEMPLATE = "https://chrome-infra-packages.appspot.com/client?platform={}&version={}"
_CLIENT_DEFAULT_DOWNLOAD_DIGEST = "//pw_env_setup:py/pw_env_setup/cipd_setup/.cipd_version.digests"
_CLIENT_DEFAULT_DOWNLOAD_VERSION = "//pw_env_setup:py/pw_env_setup/cipd_setup/.cipd_version"

def _client_repo_impl(rctx):
    client_platform = get_cipd_platform(rctx.os)
    client_version = rctx.read(rctx.attr.version).strip()
    client_digest_file_contents = rctx.read(rctx.attr.digest)

    client_sha256, error_message = get_digest(
        rctx.attr.digest,
        client_digest_file_contents,
        client_platform,
    )
    if error_message:
        fail(error_message)

    # Fetch the `cipd` binary into the repo
    rctx.download(
        output = "cipd",
        url = _CLIENT_DOWNLOAD_URL_TEMPLATE.format(client_platform, client_version),
        sha256 = client_sha256,
        executable = True,
    )

    # Write a BUILD file with an export so it can be used as "@<repo_name>//:cipd".
    rctx.file("BUILD", "exports_files([\"cipd\"], visibility = [\"//visibility:public\"])")

    # rctx.repo_metadata is only available in Bazel>=8.3.0
    if hasattr(rctx, "repo_metadata"):
        return rctx.repo_metadata(reproducible = True)
    return None

client_repo = repository_rule(
    _client_repo_impl,
    attrs = {
        "digest": attr.label(
            allow_single_file = True,
            default = Label(_CLIENT_DEFAULT_DOWNLOAD_DIGEST),
            doc = "The client tool digest file to use.",
        ),
        "version": attr.label(
            allow_single_file = True,
            default = Label(_CLIENT_DEFAULT_DOWNLOAD_VERSION),
            doc = "The client tool version file to use.",
        ),
    },
    doc = "Fetches the cipd client tool into a repository.",
)
