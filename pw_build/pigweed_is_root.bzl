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
"""A helper macro for writing Pigweed-specific build file logic."""

# Please be cautious when expanding use of this. All usages of this implicitly
# create code paths that cannot be validated from Pigweed. This helper should
# be reserved for Pigweed-specific configurations that *cannot* be expressed
# with a ``visibility`` specifier, and should never under any circumstance be
# available to downstream projects.
#
# When expanding this list, leave a clear justification.
#
# Downstream projects shouldn't ever have any reason to reference this.
visibility([
    # The host toolchain has Pigweed-only warnings that give us the
    # freedom to enforce new warnings without having to fix downstream projects
    # first.
    "//pw_toolchain/host_clang/...",
])

def pigweed_is_root():
    """A helper for conditions that express Pigweed-only behaviors.

    Returns:
        ``True`` if Pigweed is the root module.
    """
    return Label("@@//:__pkg__").repo_name == Label("@pigweed//:__pkg__").repo_name  # buildifier: disable=canonical-repository
