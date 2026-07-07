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

//! `pw_thread` provides thread execution control utilities.
//!
//!  * [`sleep`] - Blocks the execution of the current thread for at least the specified duration.
//!  * [`sleep_until`] - Blocks the execution of the current thread until the specified instant.
//!  * [`yield_now`] - Cooperatively gives up a timeslice to the OS scheduler.

use pw_time::{Duration, Instant, SystemClock};

/// Blocks the execution of the current thread for at least the specified
/// duration. This function may block for longer due to scheduling or resource
/// contention delays.
pub fn sleep(sleep_duration: Duration<SystemClock>) {
    pw_thread_backend::sleep(sleep_duration);
}

/// Blocks the execution of the current thread until at least the specified
/// time has been reached. This function may block for longer due to scheduling
/// or resource contention delays.
pub fn sleep_until(wakeup_time: Instant<SystemClock>) {
    pw_thread_backend::sleep_until(wakeup_time);
}

/// Cooperatively gives up a timeslice to the OS scheduler, allowing other
/// threads to run.
pub fn yield_now() {
    pw_thread_backend::yield_now();
}
