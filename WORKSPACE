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

workspace(
    name = "pigweed",
)

load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")

# Setup Fuchsia SDK.
# TODO: https://pwbug.dev/308624902 - This piece lacks bzlmod support.
git_repository(
    name = "fuchsia_infra",
    # ROLL: Warning: this entry is automatically updated by
    # ROLL: https://cr-buildbucket.appspot.com/builder/pigweed/tools.roll/fuchsia-infra-bazel-rules-roller
    commit = "235ee6dcbc6bc27c9ff8bb2ad21195b1e9866165",
    remote = "https://fuchsia.googlesource.com/fuchsia-infra-bazel-rules",
)

# fuchsia_infra_workspace is a macro, not a repository rule, so we can't call
# it from MODULE.bazel.
load("@fuchsia_infra//:workspace.bzl", "fuchsia_infra_workspace")

fuchsia_infra_workspace()
