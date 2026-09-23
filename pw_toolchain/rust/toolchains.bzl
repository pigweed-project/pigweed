# Copyright 2024 The Pigweed Authors
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
"""Rust toolchain configuration"""

HOSTS = [
    {
        "cipd_arch": "arm64",
        "cpu": "aarch64",
        "dylib_ext": ".so",
        "os": "linux",
        "triple": "aarch64-unknown-linux-gnu",
    },
    {
        "cipd_arch": "amd64",
        "cpu": "x86_64",
        "dylib_ext": ".so",
        "os": "linux",
        "triple": "x86_64-unknown-linux-gnu",
    },
    {
        "cipd_arch": "arm64",
        "cpu": "aarch64",
        "dylib_ext": ".dylib",
        "os": "macos",
        "triple": "aarch64-apple-darwin",
    },
    {
        "cipd_arch": "amd64",
        "cpu": "x86_64",
        "dylib_ext": ".dylib",
        "os": "macos",
        "triple": "x86_64-apple-darwin",
    },
]

EXTRA_TARGETS = [
    {
        "cpu": "armv6-m",
        "triple": "thumbv6m-none-eabi",
    },
    {
        "cpu": "armv7-m",
        "triple": "thumbv7m-none-eabi",
    },
    {
        "cpu": "armv7e-m",
        "triple": "thumbv7m-none-eabi",
    },
    {
        "build_std": True,
        "cpu": "armv8-m",
        "triple": "thumbv8m.main-none-eabihf",
    },
    {
        "build_std": True,
        "cpu": "aarch64",
        "triple": "aarch64-unknown-none",
    },
    {
        "build_std": True,
        "constraints": [
            "@pigweed//pw_build/constraints/riscv/extensions:I",
            "@pigweed//pw_build/constraints/riscv/extensions:M",
            "@pigweed//pw_build/constraints/riscv/extensions:C",
            "@pigweed//pw_build/constraints/riscv/extensions:A.not",
        ],
        "cpu": "riscv32",
        "triple": "riscv32imc-unknown-none-elf",
    },
    {
        "build_std": True,
        "constraints": [
            "@pigweed//pw_build/constraints/riscv/extensions:I",
            "@pigweed//pw_build/constraints/riscv/extensions:M",
            "@pigweed//pw_build/constraints/riscv/extensions:C",
            "@pigweed//pw_build/constraints/riscv/extensions:A",
        ],
        "cpu": "riscv32",
        "triple": "riscv32imac-unknown-none-elf",
    },
]

CHANNELS = [
    {
        "extra_rustc_flags": [
            "-Dlinker-messages",
            "-Dwarnings",
            "-Zemit-stack-sizes",
            "-Zmacro-backtrace",
        ],
        "name": "nightly",
        "target_settings": ["@rules_rust//rust/toolchain/channel:nightly"],
    },
    {
        # In order to approximate a stable toolchain with our nightly one, we
        # disable experimental features.  However some crate's build.rs file
        # autodetects the nightly compiler and automatically enables the
        # features.  For crates that exist in both `crates_std` and
        # `crates_no_std` we can use `crate.annotation()` in the `MODULE.bazel`.
        # For crates that do not exist in both places, this fails an we have
        # to allow these features globally.  These include:
        # `core_intrinsics`: `rustix` (a gen_rust_project dep) auto detects this
        #   feature.
        # `error_generic_member_access`: `anyhow` auto detects this feature
        # `proc_macro_span`: `proc-macro2` auto detects this features
        # `rustc_attrs`: `rustix` (a gen_rust_project dep) auto detects this
        #   feature.
        "extra_rustc_flags": [
            "-Dlinker-messages",
            "-Dwarnings",
            "-Zallow-features=core_intrinsics,error_generic_member_access,proc_macro_span,rustc_attrs",
            "-Zemit-stack-sizes",
            "-Zmacro-backtrace",
        ],
        "name": "stable",
        "target_settings": ["@rules_rust//rust/toolchain/channel:stable"],
    },
]
