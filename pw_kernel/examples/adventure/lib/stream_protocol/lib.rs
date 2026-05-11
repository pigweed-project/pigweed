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

//! # stream_protocol
//!
//! A bidirectional streaming protocol over `pw_kernel` IPC used by the adventure
//! example for communications with the userspace UART driver.
//!
//! ## `pw_kernel` IPC
//!
//! A `pw_kernel` IPC channel is comprised of a pair of kernel objects: an
//! initiator (client) and a handler (server).  All IPC transactions are initiated
//! by the initiator side.  While the handler side can not initiate a transaction
//! it can raise the `USER` signal to inform the initiator side to take action.
//!
//! ## Design
//! `stream_protocol` maps onto an IPC channel:
//!
//! - [`StreamClient`] uses the initiator side and implements two blocking methods:
//!   - [`StreamClient::write()`]: Immediately sends a write request to the server.
//!   - [`StreamClient::read()`]: Waits for the USER signal on the initiator object
//!     to be active then sends a read request to the server.
//! - [`StreamServer`] uses the handler side, maintains a buffer of data,
//!   and implements two non-blocking methods:
//!   - [`StreamServer::push_data()`]: Pushes data into the data buffer and raises
//!     the USER signal on the initiator side.
//!   - [`StreamServer::handle_ipc()`]: Handles an outstanding IPC request if
//!     pending and clears the USER signal if the request fully drains the data
//!     buffer.
#![no_std]

mod client;
mod defs;
mod server;

pub use client::*;
pub use defs::*;
pub use server::*;
