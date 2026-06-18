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

"""A repository rule that defines what features are supported by Bazel.

Pigweed's Bazel files are meant to be usable across a broad range of Bazel
versions. This can be challenging when the latest version of Bazel requires
changes where backwards compatibility becomes an issue, or when a new feature
is compelling enough that Pigweed needs to use it.

For most cases, Pigweed will attempt to determine compatibility, by using calls
like `hasattr()` to determine if a structure has an attribute, or by some
other similar way of determining if a feature is available.

However not all features have an easy way to built-in way to detect if they are
available. The Bazel repo this repository rule creates allows corner cases to
be handled by defining a `bazel_supports` structure with boolean constants for
critical features that Pigweed uses.

Determining what features are support is done by looking at the Bazel version,
which is not something normally accessible. To keep things maintainable and
readable, this file intentionally makes itself the source of truth about what
versions of Bazel are needed for each feature, and only exposes the support
status of each feature through the constants.
"""

# buildifier: disable=unused-variable=doc  We want these to be documented.
def _define(name, after = None, doc = None):
    """Defines a new "bazel_supports" feature constant.

    Args:
        name: The name of the feature constant.
        after: The first released version of Bazel that introduced this feature.
        doc: Documentation for the constant.
    """
    return "{} = {}".format(name, "_after(\"" + after + "\")" if after else "True")

_SUPPORTS_DEFS = (
    # Only add entries below if there is no other way to determine support.
    # buildifier: keep sorted
    _define("default_test_toolchain_type", after = "9.0.0", doc = "True if @bazel_tools//tools/test:default_test_toolchain_type is available"),
    # buildifier: end
    # Only add entries above if there is no other way to determine support.
)

def _bazel_supports_repo_impl(rctx):
    rctx.file("BUILD", "exports_files([\"bazel_supports.bzl\"])")

    # `native.bazel_version` is not officially documented, and is only
    # available in a few contexts: WORKSPACE files (deprecated), and repository
    # rules. We take the hint that the version should NOT be directly exposed,
    # and only define constants in a "supports" structure to explicitly name
    # the features supported given the current version.
    rctx.file("bazel_supports.bzl", """
load("@bazel_skylib//lib:versions.bzl", "versions")

_BAZEL_VERSION = "{}"

def _after(version):
    return versions.is_at_least(version, _BAZEL_VERSION)

bazel_supports = struct(
    {},
)
""".format(native.bazel_version, ",\n    ".join(_SUPPORTS_DEFS)))

    # rctx.repo_metadata is only available in Bazel>=8.3.0
    if hasattr(rctx, "repo_metadata"):
        return rctx.repo_metadata(reproducible = True)
    return None

bazel_supports_repo = repository_rule(
    implementation = _bazel_supports_repo_impl,
    doc = "Creates a repo where @repo//:bazel_supports.bzl contains Bazel feature support metadata",
)
