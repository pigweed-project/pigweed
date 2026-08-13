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

load("@rules_rust//rust:defs.bzl", "rust_binary", "rust_library")

def pw_rust_binary(name, **kwargs):
    """Wrapper for rust_binary providing some defaults.

    Specifically, this wrapper adds deps on //pw_build:default_link_extra_lib,
    and defines a "<name>_lib" rust_library target.

    Args:
      name: The name of the target.
      **kwargs: Passed to rust_binary and rust_library.
    """

    # Define a library version of the binary so it can be linked into other
    # binaries (like Zephyr apps). Zephyr requires a two-pass link for interrupt
    # tables which is only supported for cc_binary targets (via zephyr_app).
    # To link Rust code into a cc_binary, it must be built as a library first.
    # alwayslink = 1 is required to prevent the C++ linker from discarding
    # the Rust entry point symbol before it is resolved.
    rust_library(
        name = name + "_lib",
        alwayslink = 1,
        **kwargs
    )

    kwargs["deps"] = kwargs.get("deps", []) + [str(Label("//pw_build:default_link_extra_lib"))]
    kwargs["rustc_flags"] = kwargs.get("rustc_flags", []) + [
        "-C",
        "link-arg=-lc",
    ]
    rust_binary(
        name = name,
        **kwargs
    )
