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

extern crate std;

use core::marker::PhantomData;
use core::sync::atomic::{AtomicBool, Ordering};
use std::thread;

/// std-specific implementation of the [`RawInterruptSpinLock`][pw_sync_spinlock_core::RawInterruptSpinLock] trait.
pub struct RawInterruptSpinLock {
    is_locked: AtomicBool,
}

/// RAII guard returned by [`RawInterruptSpinLock::lock`] or [`RawInterruptSpinLock::try_lock`].
pub struct RawInterruptSpinLockGuard<'a> {
    lock: &'a RawInterruptSpinLock,

    // Mark this type as !Send and !Sync without using the unstable `negative_impls`
    // feature.
    _marker: PhantomData<*mut ()>,
}

impl<'a> Drop for RawInterruptSpinLockGuard<'a> {
    fn drop(&mut self) {
        self.lock.is_locked.store(false, Ordering::Release);
    }
}

impl RawInterruptSpinLock {
    const fn new() -> Self {
        Self {
            is_locked: AtomicBool::new(false),
        }
    }
}

impl pw_sync_spinlock_core::RawInterruptSpinLock for RawInterruptSpinLock {
    type Guard<'a> = RawInterruptSpinLockGuard<'a>;

    const NEW: Self = Self::new();

    fn try_lock(&self) -> Option<Self::Guard<'_>> {
        if !self.is_locked.swap(true, Ordering::Acquire) {
            Some(RawInterruptSpinLockGuard {
                lock: self,
                _marker: PhantomData,
            })
        } else {
            None
        }
    }

    fn lock(&self) -> Self::Guard<'_> {
        loop {
            if let Some(guard) = self.try_lock() {
                return guard;
            }
            thread::yield_now();
        }
    }
}
