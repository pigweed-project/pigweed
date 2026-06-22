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

/// A trait that defines the interface for low-level (raw) interrupt spinlocks.
///
/// Implementations of this trait provide synchronization primitives that prevent
/// concurrent access to shared resources across threads and interrupt handlers.
///
/// This trait is typically not used directly by application code. Instead, use
/// `InterruptSpinLock`, which wraps a backend implementation of this trait to
/// provide a safe, RAII-based API for protecting data.
///
/// # Contract
///
/// Implementations of this trait must ensure that:
/// - When the lock is held, no other thread or interrupt context on any CPU core
///   can acquire the lock.
/// - Local interrupts are disabled on the calling CPU core for the duration of
///   the lock.
/// - The lock is released when the returned [`Guard`][Self::Guard] is dropped.
pub trait RawInterruptSpinLock: Send + Sync {
    /// An associated RAII guard type that keeps the spinlock locked for its lifetime,
    /// releasing it when dropped.
    type Guard<'a>
    where
        Self: 'a;

    /// A constant initializer that returns a new, unlocked spinlock.
    const NEW: Self;

    /// Attempt to acquire the lock.
    ///
    /// Returns `Some(Guard)` if the lock was acquired successfully, or `None` if
    /// the lock was already held.
    fn try_lock(&self) -> Option<Self::Guard<'_>>;

    /// Acquire the lock
    ///
    /// Spins (busy-waits) the current thread or interrupt context until it succeeds.
    ///
    /// Returns a [`Guard`][Self::Guard] once the lock is acquired.
    fn lock(&self) -> Self::Guard<'_>;
}
