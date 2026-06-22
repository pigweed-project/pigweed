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

//! A spinlock that disables interrupts.
//!
//! This module provides [`InterruptSpinLock`], a synchronization primitive that protects
//! data from being simultaneously accessed by multiple threads and/or interrupts.
//!
//! While the lock is held, local interrupts are disabled on the calling CPU core.
//!
//! # Example
//!
//! ```
//! use pw_sync_spinlock::InterruptSpinLock;
//!
//! static LOCK: InterruptSpinLock<u32> = InterruptSpinLock::new(0);
//!
//! fn increment() {
//!     let mut data = LOCK.lock();
//!     *data += 1;
//! }
//! ```

use core::cell::UnsafeCell;
use core::marker::PhantomData;
use core::ops::{Deref, DerefMut};

pub use pw_sync_spinlock_core::RawInterruptSpinLock;

/// A synchronization primitive for protecting data from within threads and interrupts.
///
/// The `InterruptSpinLock` can be used to protect shared data from being
/// simultaneously accessed by multiple threads and/or interrupts.
/// Note: **While the lock is held, local interrupts are disabled on the calling CPU core.**
///
/// It offers exclusive, non-recursive ownership semantics.
///
pub struct InterruptSpinLock<T> {
    lock: pw_sync_spinlock_backend::RawInterruptSpinLock,
    data: UnsafeCell<T>,
}

// Safety: `InterruptSpinLock` is `Sync` because it provides mutual exclusion,
// ensuring only a single thread/interrupt context can access the underlying data
// at a time. The underlying data `T` only needs to be `Send` to be safely shared
// between contexts.
unsafe impl<T: Send> Sync for InterruptSpinLock<T> {}

// Safety: `InterruptSpinLock` is `Send` because it is safe to transfer ownership
// of the lock to another thread/context as long as the underlying data is `Send`.
unsafe impl<T: Send> Send for InterruptSpinLock<T> {}

impl<T> InterruptSpinLock<T> {
    /// Creates a new `InterruptSpinLock` wrapping the given initial value.
    pub const fn new(value: T) -> Self {
        Self {
            lock: pw_sync_spinlock_backend::RawInterruptSpinLock::NEW,
            data: UnsafeCell::new(value),
        }
    }

    /// Acquire the lock.
    ///
    /// Spins (busy-waits) the current thread or interrupt context until the lock is acquired.
    /// Note: **While the lock is held, local interrupts are disabled on the calling CPU core.**
    ///
    /// Returns an `InterruptSpinLockGuard` that gives access to the protected data
    /// and releases the lock when dropped.
    pub fn lock(&self) -> InterruptSpinLockGuard<'_, T> {
        let raw_guard = self.lock.lock();
        InterruptSpinLockGuard {
            _raw_guard: raw_guard,
            lock: self,
            _marker: PhantomData,
        }
    }

    /// Attempt to acquire the lock.
    ///
    /// Returns `Some(InterruptSpinLockGuard)` if the lock was successfully acquired,
    /// or `None` otherwise.
    /// Note: **While the lock is held, local interrupts are disabled on the calling CPU core.**
    pub fn try_lock(&self) -> Option<InterruptSpinLockGuard<'_, T>> {
        self.lock
            .try_lock()
            .map(|raw_guard| InterruptSpinLockGuard {
                _raw_guard: raw_guard,
                lock: self,
                _marker: PhantomData,
            })
    }
}

/// A guard that provides exclusive access to the data protected by an `InterruptSpinLock`.
/// The lock is released when this guard is dropped.
pub struct InterruptSpinLockGuard<'a, T> {
    _raw_guard: <pw_sync_spinlock_backend::RawInterruptSpinLock as RawInterruptSpinLock>::Guard<'a>,
    lock: &'a InterruptSpinLock<T>,

    // Mark this type as !Send and !Sync without using the unstable `negative_impls` feature.
    // See: https://github.com/rust-lang/rust/issues/68318
    //
    // This is necessary since the `InterruptSpinLock` has an implicit state
    // coupling to the interrupt state of this thread/CPU core. If this were
    // `Send`/`Sync`, it could be sent across a channel to another thread
    // concurrently executing on a different core and the implicit state would
    // be incoherent.
    _marker: PhantomData<*mut ()>,
}

impl<T> Deref for InterruptSpinLockGuard<'_, T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        // Safety: The existence of the guard guarantees exclusive access.
        unsafe { &*self.lock.data.get() }
    }
}

impl<T> DerefMut for InterruptSpinLockGuard<'_, T> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        // Safety: The existence of the guard guarantees exclusive access.
        unsafe { &mut *self.lock.data.get() }
    }
}
