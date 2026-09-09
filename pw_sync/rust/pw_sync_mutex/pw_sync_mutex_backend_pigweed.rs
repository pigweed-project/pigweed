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

//! Pigweed kernel backend for `pw_sync_mutex`.
//!
//! This crate implements [`RawMutexTrait`] and [`RawTimedMutexTrait`] using the
//! Pigweed kernel's native mutex primitive [`kernel::sync::mutex::RawMutex`].

use pw_sync_mutex_core::{RawMutex as RawMutexTrait, RawTimedMutex as RawTimedMutexTrait};
use pw_time::Clock;
use target_arch::Arch;

/// Pigweed-kernel-specific implementation of [`RawMutexTrait`] and
/// [`RawTimedMutexTrait`].
pub struct RawTimedMutex {
    mutex: kernel::sync::mutex::RawMutex<Arch>,
}

impl Default for RawTimedMutex {
    fn default() -> Self {
        Self {
            mutex: kernel::sync::mutex::RawMutex::new(Arch),
        }
    }
}

pub type RawMutex = RawTimedMutex;

// SAFETY: `RawTimedMutex` delegates to the kernel mutex, which provides mutual
// exclusion across threads.
unsafe impl RawMutexTrait for RawTimedMutex {
    fn try_lock(&self) -> bool {
        self.mutex.try_lock()
    }

    fn lock(&self) {
        self.mutex.lock();
    }

    unsafe fn unlock(&self) {
        // SAFETY: The caller guarantees that the lock is held by the calling
        // thread.
        unsafe {
            self.mutex.unlock();
        }
    }
}

// SAFETY: `RawTimedMutex` delegates to the kernel mutex, which correctly
// manages timed blocking and deadlines.
unsafe impl RawTimedMutexTrait for RawTimedMutex {
    type Duration = pw_time::Duration<pw_time::SystemClock>;
    type Instant = pw_time::Instant<pw_time::SystemClock>;

    fn try_lock_for(&self, timeout: Self::Duration) -> bool {
        let deadline = pw_time::SystemClock::now() + timeout;
        self.try_lock_until(deadline)
    }

    fn try_lock_until(&self, deadline: Self::Instant) -> bool {
        let kernel_deadline = kernel::Instant::from_ticks(deadline.ticks());
        self.mutex.lock_until(kernel_deadline).is_ok()
    }
}
