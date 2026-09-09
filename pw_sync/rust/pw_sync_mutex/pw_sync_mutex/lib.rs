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

//! Mutual exclusion synchronization primitives.
//!
//! This crate provides thread-safe, RAII-guarded mutual exclusion
//! synchronization primitives that protect shared data from simultaneous
//! access by multiple threads or execution contexts.
//!
//! See the [Design & Architecture](`_design`) docs for in-depth design goals
//! and architectural details.
//!
//! # Example
//!
//! ```
//! # #[unsafe(no_mangle)]
//! # unsafe extern "C-unwind" fn pw_assert_HandleFailure() -> ! { loop {} }
//! use pw_sync_mutex::SystemMutex;
//!
//! let lock = SystemMutex::new(0);
//! {
//!     let mut data = lock.lock();
//!     *data += 1;
//! }
//! assert_eq!(*lock.lock(), 1);
//! ```
//!
//! ## Backends
//!
//! The system backend (`pw_sync_mutex_backend`) is selected at compile time
//! (e.g. via Bazel label flags), enabling portable application code across
//! diverse execution environments.  Examples include:
//!
//! - **`std`**: Implemented using Rust standard library synchronization
//!   primitives for host development and testing.
//! - **`freertos`**: Native FreeRTOS implementation utilizing FreeRTOS
//!   mutex semaphores with priority inheritance.
//! - **`pigweed`**: Native Pigweed kernel (`pw_kernel`) backend integrated
//!   directly with kernel synchronization primitives.
//!
//! ## Crate Structure
//!
//! The mutex implementation is divided across three layers of crates:
//!
//! - **`pw_sync_mutex_core`**: The foundational, dependency-minimal crate. It
//!   defines the [`RawMutex`] and [`RawTimedMutex`] traits, the generic
//!   [`Mutex<T, Lock>`] wrapper, the RAII [`MutexGuard`], and
//!   [`SingleThreadMutex`]. It has no platform or OS dependencies and can be
//!   depended on directly by libraries that wish to remain generic over
//!   synchronization backends.
//! - **`pw_sync_mutex_backend_*`**: Platform-specific backend crates that
//!   implement [`RawMutex`] and [`RawTimedMutex`] for target environments
//!   (e.g., standard library, FreeRTOS, or Pigweed kernel). A backend is
//!   selected at build time via the `pw_sync_mutex_backend` build
//!   configuration.
//! - **`pw_sync_mutex` (this crate)**: The primary user-facing facade crate.
//!   It re-exports all items from `pw_sync_mutex_core` and links to the
//!   build-configured backend to provide concrete system lock aliases
//!   [`SystemMutex`] and [`SystemTimedMutex`].

pub use pw_sync_mutex_core::*;

pub mod _design;

/// A mutual exclusion synchronization primitive using the default backend lock.
pub type SystemMutex<T> = Mutex<T, pw_sync_mutex_backend::RawMutex>;

/// A mutual exclusion primitive extending [`SystemMutex`] with timed blocking
/// acquisitions using the default backend lock.
pub type SystemTimedMutex<T> = Mutex<T, pw_sync_mutex_backend::RawTimedMutex>;

/// A single-threaded mutual exclusion synchronization primitive for single-threaded contexts.
pub type SystemSingleThreadMutex<T> = SingleThreadMutex<T>;

/// A single-threaded mutual exclusion synchronization primitive with timed locking
/// using [`pw_time::SystemClock`].
pub type SystemSingleThreadTimedMutex<T> = SingleThreadTimedMutex<T, pw_time::SystemClock>;

#[cfg(test)]
extern crate std;

#[cfg(test)]
mod tests;

// Compile-time type assertion ensuring the backend raw mutex types implement
// Send and Sync.
#[allow(dead_code)]
fn _assert_backend_send_sync() {
    fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<pw_sync_mutex_backend::RawMutex>();
    assert_send_sync::<pw_sync_mutex_backend::RawTimedMutex>();
}

// Compile-time type assertion ensuring the backend's associated Duration and
// Instant types are backed by pw_time::SystemClock.
#[allow(dead_code)]
fn _assert_backend_clock_types() {
    let _ = |x: pw_time::Duration<pw_time::SystemClock>| -> <pw_sync_mutex_backend::RawTimedMutex as RawTimedMutex>::Duration {
        x
    };
    let _ = |x: pw_time::Instant<pw_time::SystemClock>| -> <pw_sync_mutex_backend::RawTimedMutex as RawTimedMutex>::Instant {
        x
    };
}
