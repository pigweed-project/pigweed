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
"""Pigweed's customized rust_binary wrapper."""

load("@rules_rust//rust:defs.bzl", "rust_binary")

def pw_rust_binary(**kwargs):
    """Wrapper for rust_binary providing some defaults.

    Specifically, this wrapper adds deps on //pw_build:default_link_extra_lib.

    Args:
      **kwargs: Passed to rust_binary.
    """
    kwargs["deps"] = kwargs.get("deps", []) + [str(Label("//pw_build:default_link_extra_lib"))]
    kwargs["rustc_flags"] = kwargs.get("rustc_flags", []) + [
        "-C",
        "link-arg=-lc",
    ]
    rust_binary(**kwargs)
