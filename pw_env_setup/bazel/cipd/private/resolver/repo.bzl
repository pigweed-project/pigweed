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
"""Repository rule for writing resolution report."""

def _resolution_repo_impl(rctx):
    rctx.file("BUILD", """
exports_files(
    ["resolution.md", "resolution.bzl"],
    visibility = ["//visibility:public"],
)
sh_binary(
    name = "show",
    srcs = ["show.sh"],
)
""")
    rctx.file("resolution.md", rctx.attr.content)
    rctx.file("resolution.bzl", "CIPD_RESOLUTION = " + rctx.attr.data_content)
    rctx.file("show.sh", "#!/bin/bash\ncat " + str(rctx.path("resolution.md")), executable = True)

    # rctx.repo_metadata is only available in Bazel>=8.3.0
    if hasattr(rctx, "repo_metadata"):
        return rctx.repo_metadata(reproducible = True)
    return None

resolution_repo = repository_rule(
    implementation = _resolution_repo_impl,
    attrs = {
        "content": attr.string(mandatory = True),
        "data_content": attr.string(mandatory = True),
    },
    doc = "Creates a repository containing resolution details.",
)
