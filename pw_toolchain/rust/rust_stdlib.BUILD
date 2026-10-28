# Copyright 2023 The Pigweed Authors
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

load("@rules_rust//rust:toolchain.bzl", "rust_stdlib_filegroup")

exports_files(
    glob(["**"]),
    visibility = ["//visibility:public"],
)

filegroup(
    name = "all",
    srcs = glob(["**"]),
    visibility = ["//visibility:public"],
)

rust_stdlib_filegroup(
    name = "rust_std",
    # This is globbing over the target triple. Ideally, only the relevant target
    # tripple is part of this filegroup.
    srcs = glob(["lib/rustlib/*/lib/*"]),
    visibility = ["//visibility:public"],
)
