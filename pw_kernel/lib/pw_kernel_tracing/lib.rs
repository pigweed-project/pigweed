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

//! # pw_kernel_tracing
//!
//! `pw_kernel_tracing` is a `no_std` library that provides the core types and traits
//! for tracing within `pw_kernel`. It is designed to be efficient for embedded usage
//! and supports lock-free buffering of trace records.  Care is taken to ensure that
//! generating trace records and writing them to the buffer result in simple u32
//! copies.
//!
//! ## Overview
//!
//! This crate provides:
//! - **`Record`**: A fixed-size trace entry containing metadata (timestamp, event type) and payload.
//! - **`Buffer`**: A circular buffer implementation for storing trace records using atomic indices.
//!
//! ## Concurrency Model
//!
//! The [`Buffer`] uses atomic operations to reserve slots for writing records, allowing
//! multiple producers (e.g., interrupt handlers, threads) to write to the trace buffer
//! concurrently without lock contention.
//!
//! ## Example
//!
//! ```
//! use pw_kernel_tracing::{Buffer, EventType, Record, ContextSwitchEvent};
//! use core::sync::atomic::AtomicUsize;
//!
//! // Create a buffer with 4 entries.
//! // In a real kernel, you would likely use a larger size and a static instance.
//! let buffer = Buffer::<AtomicUsize, 4>::new();
//!
//! // Add a Context Switch record.
//! // Arguments: timestamp, event_type, event_data words.
//! let event = ContextSwitchEvent {
//!     old_thread_id: 1,
//!     new_thread_id: 2,
//!     old_thread_state: 3,
//! };
//!
//! buffer.add_record(
//!     0x1234,                     // Timestamp
//!     EventType::ContextSwitch,   // Event Type
//!     event.encode(),             // Event Payload
//! );
//! ```

#![cfg_attr(not(feature = "std"), no_std)]

mod buffer;
mod record;

pub use buffer::*;
pub use record::*;
