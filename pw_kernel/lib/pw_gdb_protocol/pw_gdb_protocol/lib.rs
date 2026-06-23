// Copyright 2025 The Pigweed Authors
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

//! A Rust implementation of the GDB remote protocol.
//!
//! This crate provides a `Client` for interacting with GDB servers (aka targets).
//! It uses the futures crate `AsyncRead` and `AsyncWrite` traits for I/O to
//! abstract the underlying transport.
//!
//! This crate is targeted at host level tooling and is not intended for use in
//! embedded/no_std scenarios.
//!
//! # Example
//!
//! ```
//! use pw_gdb_protocol::Client;
//! use futures::io::{AsyncRead, AsyncWrite};
//!
//! async fn example<S>(stream: S) -> Result<(), Box<dyn std::error::Error>>
//! where
//!     S: AsyncRead + AsyncWrite + Unpin,
//! {
//!     let mut client = Client::new(stream);
//!
//!     // Read memory
//!     let data = client.read_memory(0x1000, 4).await?;
//!     println!("Memory: {}", hex::encode(data));
//!
//!     Ok(())
//! }
//! ```

pub mod client;
mod packet;

pub use client::Client;
pub use packet::{BreakpointType, StopReply};
