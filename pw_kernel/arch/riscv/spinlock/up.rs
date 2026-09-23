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

use core::cell::UnsafeCell;

use kernel::sync::spinlock::BareSpinLock as BareSpinLockTrait;

use super::common::{InterruptGuard, RiscVSpinLockGuard};

/// Non-SMP bare spinlock
pub struct BareSpinLock {
    // Lock state is needed to support `try_lock()` semantics.  An `UnsafeCell`
    // is used to hold the lock state as exclusive access is guaranteed by
    // enabling and disabling interrupts.
    is_locked: UnsafeCell<bool>,
}

// Safety: Access to `is_locked` is protected by disabling interrupts and
// proper barriers.
unsafe impl Send for BareSpinLock {}
unsafe impl Sync for BareSpinLock {}

impl BareSpinLock {
    #[must_use]
    pub const fn new() -> Self {
        Self {
            is_locked: UnsafeCell::new(false),
        }
    }

    // Must be called with interrupts disabled.
    #[inline]
    pub(super) unsafe fn unlock(&self) {
        unsafe {
            self.is_locked.get().write_volatile(false);
        }
    }
}

impl Default for BareSpinLock {
    fn default() -> Self {
        Self::new()
    }
}

impl BareSpinLockTrait for BareSpinLock {
    type Guard<'a> = RiscVSpinLockGuard<'a>;
    const NEW: BareSpinLock = Self::new();

    #[inline(always)]
    fn try_lock(&self) -> Option<Self::Guard<'_>> {
        let guard = InterruptGuard::new();
        // Safety: exclusive access to `is_locked` guaranteed because interrupts
        // are off.
        if unsafe { self.is_locked.get().read_volatile() } {
            return None;
        }

        unsafe {
            self.is_locked.get().write_volatile(true);
        }

        Some(RiscVSpinLockGuard::new(guard, self))
    }

    #[inline(always)]
    fn lock(&self) -> Self::Guard<'_> {
        let guard = InterruptGuard::new();
        // Safety: exclusive access to `is_locked` guaranteed because interrupts
        // are off.
        if unsafe { self.is_locked.get().read_volatile() } {
            pw_assert::panic!("recursively locked spinlock");
        }

        unsafe {
            self.is_locked.get().write_volatile(true);
        }

        RiscVSpinLockGuard::new(guard, self)
    }

    /// Unconditionally break the lock.
    ///
    /// Do not call directly.
    ///
    /// # Safety
    /// See [`kernel::sync::spinlock::SpinLock::break_lock()`] for use and
    /// safety information.
    unsafe fn break_lock(&self) {
        unsafe { self.unlock() };
    }
}
