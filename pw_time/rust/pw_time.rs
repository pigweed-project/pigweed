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

//! # pw_time
//!
//! `pw_time` provides time types and a system clock.
//!
//!  * [`Clock`] - A monotonically increasing clock with its own scale between
//!    internal "ticks" and human time.
//!  * [`Instant<Clock>`] - A sample of a specific instant on a given [`Clock`].
//!  * [`Duration<Clock>`] - A span of time whose internal representation is scaled to
//!    the given [`Clock`].
//!  * [`SystemClock`] - The clock used by the core system/RTOS.
//!
//! ## Examples
//!
//! ```
//! # #[unsafe(no_mangle)]
//! # unsafe extern "C-unwind" fn pw_assert_HandleFailure() -> ! { loop {} }
//! use pw_time::{SystemClock, Instant, Duration, Clock};
//!
//! let start = SystemClock::now();
//! let duration = Duration::from_millis(500);
//! let end = start + duration;
//! assert_eq!(end - start, duration);
//! ```
//!
//! ## Design
//!
//! #### Clocks
//!
//! `pw_time` is designed to allow type-safe handling of systems with multiple
//! clock domains.  Each clock domain is defined by a [`Clock`].  [`Instant`]
//! and [`Duration`] are generic of a [`Clock`] and can not be mixed.
//!
//! #### Representation
//!
//! `pw_time` uses 64 bit internal types to represent [`Instant`]s (unsigned)
//! and [`Duration`]s (unsigned).  The scale of this internal value is defined
//! by the [`Clock::TICKS_PER_SEC`].  Future work may move from this scalar
//! scale to a ratio based one similar to C++'s `std::chrono` types.
//!
//! #### Const conversion
//!
//! [`Duration`]'s `from_*` constructors are const and are designed to be
//! optimized into single "tick native" values by the compiler.
//!
//! #### Panic behavior
//!
//! `pw_time` is designed to never cause a Rust `panic` because this can lead
//! to code size bloat on size constrained systems.  Instead it relies on
//! [`pw_assert`] to allow system specific panic behavior.

/// The clock used by the core system/RTOS
///
/// This clock is by the system for time bound operations such as thread
/// sleeping, waiting on mutexes/semaphores, etc.  The clock's rate is system
/// dependent.
pub use pw_time_backend::SystemClock;
pub use pw_time_core::{Clock, Duration, Instant};
