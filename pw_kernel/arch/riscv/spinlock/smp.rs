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

#[cfg(not(target_has_atomic = "8"))]
compile_error!(
    "SMP spinlock requires hardware atomic compare-and-swap support (RISC-V 'A' extension)"
);

use core::hint;
use core::sync::atomic::{AtomicBool, Ordering};

use kernel::sync::spinlock::BareSpinLock as BareSpinLockTrait;

use super::common::{InterruptGuard, RiscVSpinLockGuard};

/// SMP bare spinlock using hardware atomic operations.
///
/// Unlike `up::BareSpinLock`, recursive locking on the same core will deadlock
/// rather than panic until per-CPU owner tracking is available.
// TODO: https://pwbug.dev/565443213 - Detect recursive locking on the same core
// once per-CPU owner tracking is available.
pub struct BareSpinLock {
    // Uses `Ordering::Acquire` on successful `lock()`/`try_lock()` and
    // `Ordering::Release` on `unlock()` so that all reads and writes within the
    // critical section are ordered after lock acquisition and visible to the
    // next core that acquires the lock. Failed compare-exchange attempts use
    // `Ordering::Relaxed` because no protected state is accessed on failure.
    is_locked: AtomicBool,
}

impl BareSpinLock {
    #[must_use]
    pub const fn new() -> Self {
        Self {
            is_locked: AtomicBool::new(false),
        }
    }

    // Must be called with interrupts disabled.
    #[inline]
    pub(super) unsafe fn unlock(&self) {
        self.is_locked.store(false, Ordering::Release);
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
        if self
            .is_locked
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_ok()
        {
            Some(RiscVSpinLockGuard::new(guard, self))
        } else {
            None
        }
    }

    #[inline(always)]
    fn lock(&self) -> Self::Guard<'_> {
        let guard = InterruptGuard::new();
        // `compare_exchange_weak` is used here because we are already in a
        // retry loop: any spurious LL/SC (`lr.w`/`sc.w`) failure is retried by
        // the outer `while` loop without emitting a nested loop.
        while self
            .is_locked
            .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            while self.is_locked.load(Ordering::Relaxed) {
                hint::spin_loop();
            }
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
