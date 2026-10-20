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
//! <!-- categories-start -->
//! <!-- +----------------------------------------------------------+ -->
//! <!-- | WARNING: AUTO-GENERATED SECTION. DO NOT MANUALLY EDIT!   | -->
//! <!-- |                                                          | -->
//! <!-- | To regenerate this section, run:                         | -->
//! <!-- |   bazelisk run //docs/common/metadata:sync               | -->
//! <!-- |                                                          | -->
//! <!-- | Docs: https://pigweed.dev/contributing/docs/website.html | -->
//! <!-- +----------------------------------------------------------+ -->
//! The following Pigweed modules are available in Rust, organized by category:
//!
//! ### Comms
//!
//! Remote procedure calls (RPC), channels, and communication streams.
//!
//! - [`pw_stream`](../pw_stream/index.html) - A foundational interface for streaming data.
//!
//! ### Observability
//!
//! Structured logging, tokenization, metrics, crash handling, and tracing.
//!
//! - [`pw_log`](../pw_log/index.html) - Robust and portable logging for embedded projects.
//! - [`pw_tokenizer`](../pw_tokenizer/index.html) - Compress strings to shrink logs by +75%.
//! - [`pw_assert`](../pw_assert/index.html) - Flexible crash triggering and handling.
//! - [`pw_format`](../pw_format/index.html) - String formatting.
//!
//! ### Encoding & serialization
//!
//! Protocol Buffers, JSON, Varint, Base64, and wire format serialization.
//!
//! - [`pw_base64`](../pw_base64/index.html) - Base64 encoding, decoding, and validating.
//! - [`pw_bytes`](../pw_bytes/index.html) - Utilities for manipulating binary data.
//! - [`pw_varint`](../pw_varint/index.html) - Functions for encoding and decoding variable length integers.
//!
//! ### OS abstraction layers
//!
//! Synchronization, threading, and portable RTOS abstractions.
//!
//! - [`pw_thread`](../pw_thread/index.html) - Thread creation and execution.
//!
//! ### HALs
//!
//! Hardware abstraction layer interfaces for timers, I2C, SPI, UART, and GPIO.
//!
//! - [`pw_time`](../pw_time/index.html) - Robust time, instants, and durations for multiple clock domains.
//!
//! ### Data structures & utilities
//!
//! Embedded-safe collections, spans, strings, status, and error handling.
//!
//! - [`pw_status`](../pw_status/index.html) - Exception-free error propagation for embedded.
//! <!-- +----------------------------------------------------------+ -->
//! <!-- | END OF AUTO-GENERATED SECTION                            | -->
//! <!-- +----------------------------------------------------------+ -->
//! <!-- categories-end -->
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
