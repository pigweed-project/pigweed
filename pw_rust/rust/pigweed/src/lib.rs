// Copyright 2026 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.
#![no_std]

//! Pigweed is a collection of embedded libraries (modules) that work together
//! to enable faster and more robust development of embedded systems. This
//! rustdoc is our API reference and does not always cover general Pigweed
//! topics, which are found at the [Pigweed Homepage](https://pigweed.dev).
//!
//! Note: This crate exists solely to provide this landing page.
//!
//! ## Modules
//!
//! The following Pigweed modules are available in Rust:
//!
//! - [`pw_base64`](../pw_base64/index.html) - Base64 encoding and decoding.
//! - [`pw_bytes`](../pw_bytes/index.html) - Byte manipulation utilities.
//! - [`pw_format`](../pw_format/index.html) - Formatting support.
//! - [`pw_log`](../pw_log/index.html) - Logging facade.
//! - [`pw_status`](../pw_status/index.html) - Status codes and results.
//! - [`pw_stream`](../pw_stream/index.html) - Traits for streaming data.
//! - [`pw_tokenizer`](../pw_tokenizer/index.html) - String tokenization.
//! - [`pw_varint`](../pw_varint/index.html) - Variable-length integer encoding.
//!
//! Pigweed's Rust support is relatively new compared to our mature C++
//! codebase. We are actively working on expanding the set of modules available
//! in Rust, and expect to eventually cover almost all functionality from our
//! C++ codebase in Rust.
//!
//! ## Build
//!
//! Pigweed uses **Bazel** as its primary build system for Rust. We do not yet
//! support standard `cargo` builds for in-tree crates due to our complex
//! cross-compilation and dependency management requirements (including vendored
//! third-party crates).
//!
//! To build Pigweed's Rust targets, follow the standard Bazel instructions in
//! the [getting started guide](https://pigweed.dev/get_started/).
//!
//! ## Kernel
//!
//! Pigweed now includes an experimental **Kernel** written in Rust!
//! The kernel documentation is currently hosted separately in the main Sphinx docs.
//! See the [pw_kernel documentation](https://pigweed.dev/pw_kernel/) for details.
