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

"""A helper macro for defining Bazel 9 default test toolchains."""

load("@bazel_supports//:bazel_supports.bzl", "bazel_supports")

def pw_default_test_toolchain(*, name = None, **kwargs):
    """Bazel compatibility shim for toolchains with default_test_toolchain_type

    The toolchain type "@bazel_tools//tools/test:default_test_toolchain_type"
    was added with Bazel 9, and does not exist under Bazel 8. Bazel 9 uses
    toolchains registered with this type to determine how to execute tests.

    This shim allows toolchains to be defined so that if the end user is using
    Bazel 8, the toolchains will have a definition, but be ignored.

    This is important as toolchains cannot be conditionally registered.
    """

    if bazel_supports.default_test_toolchain_type:
        native.toolchain(
            name = name,
            **kwargs
        )
    else:
        native.toolchain(
            name = name,
            target_compatible_with = [
                "@platforms//:incompatible",
            ],
            toolchain = "@bazel_tools//tools/test:empty_toolchain",
            toolchain_type = Label("//pw_toolchain:unused_toolchain_type"),
            visibility = kwargs.get("visibility", ["//visibility:public"]),
        )
