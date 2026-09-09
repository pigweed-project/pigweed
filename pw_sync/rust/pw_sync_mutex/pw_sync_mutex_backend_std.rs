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

//! Standard library backend for `pw_sync_mutex`.
//!
//! This crate implements [`RawMutex`][RawMutexTrait] and
//! [`RawTimedMutex`][RawTimedMutexTrait] using Rust's standard library thread
//! synchronization primitives.
//!
//! ## Design
//!
//! Rust's standard library [`std::sync::Mutex`] wraps data and provides mutual
//! exclusion, but it does not support timed acquisition operations (such as
//! `try_lock_for` or `try_lock_until`), nor does it support direct manual
//! unlocking without dropping an RAII guard.
//!
//! To satisfy these interfaces, this backend provides two implementations:
//!
//! - **[`RawMutex`]**: Uses [`std::sync::Mutex<()>`] and stores the active
//!   [`std::sync::MutexGuard`] inside an
//!   [`UnsafeCell`][core::cell::UnsafeCell], dropping it on
//!   [`unlock`](RawMutexTrait::unlock) to demonstrate wrapping an underlying
//!   primitive that lacks an explicit manual unlock API.
//! - **[`RawTimedMutex`]**: Uses a combination of [`std::sync::Mutex<bool>`]
//!   (to track lock state) and [`std::sync::Condvar`] to support non-blocking,
//!   blocking, and timed acquisition operations
//!   ([`try_lock_for`](RawTimedMutexTrait::try_lock_for) and
//!   [`try_lock_until`](RawTimedMutexTrait::try_lock_until)). When released in
//!   [`unlock`](RawMutexTrait::unlock), `condvar.notify_one()` wakes a waiting
//!   thread.

extern crate std;

use core::cell::UnsafeCell;

use pw_sync_mutex_core::{RawMutex as RawMutexTrait, RawTimedMutex as RawTimedMutexTrait};
use pw_time::Clock;

/// std-specific implementation of [`RawMutex`][RawMutexTrait] using
/// [`std::sync::Mutex`].
///
/// This serves as an example of how to implement [`RawMutex`][RawMutexTrait]
/// on an underlying primitive that does not support explicit unlocking. It
/// holds the underlying [`std::sync::MutexGuard`] in an `Option` inside the
/// mutex while locked, dropping it on [`unlock`](RawMutexTrait::unlock).
pub struct RawMutex {
    // NOTE: `guard` must be declared before `mutex` so that if the mutex is
    // dropped while locked (e.g. during panic unwinding), `guard` is dropped
    // and unlocked before `mutex` is destroyed. Some platforms (such as macOS)
    // will fault if a mutex is destroyed while it is locked.
    guard: UnsafeCell<Option<std::sync::MutexGuard<'static, ()>>>,
    mutex: std::sync::Mutex<()>,
}

// SAFETY: `RawMutex` is `Send` because moving ownership of the mutex to
// another thread is safe when the mutex is not locked. Although
// `std::sync::MutexGuard` is `!Send`, the `RawMutex::unlock` contract requires
// that `unlock` must be called from the same thread that acquired the lock.
// Consequently, the stored `MutexGuard` is only ever accessed and dropped on
// the acquiring thread, and transferring ownership of an unlocked `RawMutex`
// across threads is safe.
unsafe impl Send for RawMutex {}

// SAFETY: `RawMutex` is `Sync` because `self.mutex` synchronizes and guards all
// access to the inner `UnsafeCell` (`self.guard`):
// - In `lock()` and `try_lock()`, `self.guard` is only written to after
//   `self.mutex` has been successfully acquired, guaranteeing exclusive access.
// - In `unlock()`, `self.guard` is only read and taken by the thread holding
//   the lock (per the contract that `unlock` must be called by the thread that
//   acquired it). Taking and dropping the stored `MutexGuard` then releases
//   `self.mutex`.
// Thus, concurrent unsynchronized access to `self.guard` is impossible across
// threads.
unsafe impl Sync for RawMutex {}

impl Default for RawMutex {
    fn default() -> Self {
        Self {
            guard: UnsafeCell::new(None),
            mutex: std::sync::Mutex::new(()),
        }
    }
}

unsafe impl RawMutexTrait for RawMutex {
    fn try_lock(&self) -> bool {
        match self.mutex.try_lock() {
            Ok(guard) => {
                // SAFETY: We hold the lock on `self.mutex`, giving exclusive
                // access to `self.guard`. The guard's lifetime is transmuted to
                // `'static` and will be released when `unlock()` is called.
                unsafe {
                    *self.guard.get() = Some(core::mem::transmute::<
                        std::sync::MutexGuard<'_, ()>,
                        std::sync::MutexGuard<'static, ()>,
                    >(guard));
                }
                true
            }
            Err(std::sync::TryLockError::WouldBlock) => false,
            Err(std::sync::TryLockError::Poisoned(_)) => {
                pw_assert::panic!("Internal std::sync::Mutex was poisoned");
            }
        }
    }

    fn lock(&self) {
        let Ok(guard) = self.mutex.lock() else {
            pw_assert::panic!("Failed to lock internal std::sync::Mutex");
        };
        // SAFETY: We hold the lock on `self.mutex`, giving exclusive access to
        // `self.guard`. The guard's lifetime is transmuted to `'static` and
        // will be released when `unlock()` is called.
        unsafe {
            *self.guard.get() = Some(core::mem::transmute::<
                std::sync::MutexGuard<'_, ()>,
                std::sync::MutexGuard<'static, ()>,
            >(guard));
        }
    }

    unsafe fn unlock(&self) {
        // SAFETY: `unlock` is called by the thread that acquired the lock via
        // `MutexGuard::drop` (which is `!Send`). Taking the guard drops it,
        // which releases `self.mutex`.
        let guard = unsafe { (*self.guard.get()).take() };
        if let Some(guard) = guard {
            drop(guard);
        } else {
            pw_assert::panic!("unlock called on unlocked RawMutex");
        }
    }
}

/// std-specific implementation of [`RawTimedMutex`][RawTimedMutexTrait].
///
/// Uses an internal [`std::sync::Mutex<bool>`] and [`std::sync::Condvar`] to
/// provide non-blocking, blocking, and timed mutex locking.
pub struct RawTimedMutex {
    lock_state: std::sync::Mutex<bool>,
    condvar: std::sync::Condvar,
}

impl Default for RawTimedMutex {
    fn default() -> Self {
        Self {
            lock_state: std::sync::Mutex::new(false),
            condvar: std::sync::Condvar::new(),
        }
    }
}

unsafe impl RawMutexTrait for RawTimedMutex {
    fn try_lock(&self) -> bool {
        let Ok(mut state) = self.lock_state.lock() else {
            pw_assert::panic!("Failed to lock internal std::sync::Mutex");
        };
        if *state {
            false
        } else {
            *state = true;
            true
        }
    }

    fn lock(&self) {
        let Ok(mut state) = self.lock_state.lock() else {
            pw_assert::panic!("Failed to lock internal std::sync::Mutex");
        };
        while *state {
            state = match self.condvar.wait(state) {
                Ok(guard) => guard,
                Err(_) => pw_assert::panic!("Failed to wait on std::sync::Condvar"),
            };
        }
        *state = true;
    }

    unsafe fn unlock(&self) {
        let Ok(mut state) = self.lock_state.lock() else {
            pw_assert::panic!("Failed to lock internal std::sync::Mutex");
        };
        *state = false;
        self.condvar.notify_one();
    }
}

unsafe impl RawTimedMutexTrait for RawTimedMutex {
    type Duration = pw_time::Duration<pw_time::SystemClock>;
    type Instant = pw_time::Instant<pw_time::SystemClock>;

    fn try_lock_for(&self, timeout: Self::Duration) -> bool {
        if timeout.ticks() == 0 {
            return self.try_lock();
        }
        let nanos = u64::try_from(timeout.as_nanos()).unwrap_or(u64::MAX);
        let std_dur = core::time::Duration::from_nanos(nanos);
        let Ok(state) = self.lock_state.lock() else {
            pw_assert::panic!("Failed to lock internal std::sync::Mutex");
        };
        let Ok((mut state, _result)) =
            self.condvar
                .wait_timeout_while(state, std_dur, |&mut is_locked| is_locked)
        else {
            pw_assert::panic!("Failed to wait_timeout_while on std::sync::Condvar");
        };
        if *state {
            false
        } else {
            *state = true;
            true
        }
    }

    fn try_lock_until(&self, deadline: Self::Instant) -> bool {
        self.try_lock_for(deadline - pw_time::SystemClock::now())
    }
}
