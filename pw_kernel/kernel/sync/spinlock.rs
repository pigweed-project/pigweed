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
use core::ops::{Deref, DerefMut};

use crate::scheduler::PreemptDisableGuard;
use crate::{Arch, Kernel};

/// Controls whether dropping a guard reconstructed via
/// [`SpinLock::adopt_locked`] re-enables interrupts or keeps them disabled.
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum InterruptsOnDrop {
    /// Re-enable interrupts after unlocking when the guard is dropped.
    Restore,
    /// Leave interrupts disabled after unlocking when the guard is dropped.
    KeepDisabled,
}

pub trait BareSpinLock: Send + Sync {
    type Guard<'a>
    where
        Self: 'a;

    const NEW: Self;

    fn try_lock(&self) -> Option<Self::Guard<'_>>;

    // This default implementation of `lock()` will usually be overridden by
    // architecture-specific implementations.
    #[inline(always)]
    fn lock(&self) -> Self::Guard<'_> {
        loop {
            if let Some(sentinel) = self.try_lock() {
                return sentinel;
            }
        }
    }

    /// Reconstructs a guard for a lock that is already held by the current core
    /// (for example, when lock ownership is transferred across a context switch
    /// into a newly started thread's entry trampoline).
    ///
    /// Do not call directly.
    ///
    /// `interrupts_on_drop` controls whether dropping the returned guard will
    /// re-enable interrupts after unlocking ([`InterruptsOnDrop::Restore`]) or
    /// leave interrupts disabled ([`InterruptsOnDrop::KeepDisabled`]).
    ///
    /// # Safety
    /// The caller must guarantee that the lock is currently held, interrupts are
    /// disabled, and no other active `Guard` will unlock it. See
    /// [`SpinLock::adopt_locked()`] for use and safety information.
    unsafe fn adopt_locked(&self, interrupts_on_drop: InterruptsOnDrop) -> Self::Guard<'_>;

    // TODO - konkers: Add optimized path for functions that know they are in
    // atomic context (i.e. interrupt handlers).
}

pub struct SpinLockGuard<'lock, K: Kernel, T> {
    lock: &'lock SpinLock<K, T>,
    _preempt_guard: PreemptDisableGuard<K>,
    _inner_guard: <<K as Arch>::BareSpinLock as BareSpinLock>::Guard<'lock>,
}

impl<K: Kernel, T> Deref for SpinLockGuard<'_, K, T> {
    type Target = T;

    fn deref(&self) -> &T {
        unsafe { &*self.lock.data.get() }
    }
}

impl<K: Kernel, T> DerefMut for SpinLockGuard<'_, K, T> {
    fn deref_mut(&mut self) -> &mut T {
        unsafe { &mut *self.lock.data.get() }
    }
}

pub struct SpinLock<K: Kernel, T> {
    data: UnsafeCell<T>,
    inner: K::BareSpinLock,
}
// As long as the inner type is `Send` and the bare spinlock is `Sync`, the lock
// can be shared between threads.
unsafe impl<K: Sync + Kernel, T: Send> Sync for SpinLock<K, T> {}

impl<K: Kernel, T> SpinLock<K, T> {
    pub const fn new(initial_value: T) -> Self {
        Self {
            data: UnsafeCell::new(initial_value),
            inner: K::BareSpinLock::NEW,
        }
    }

    pub fn try_lock(&self, kernel: K) -> Option<SpinLockGuard<'_, K, T>> {
        self.inner.try_lock().map(|guard| SpinLockGuard {
            lock: self,
            _preempt_guard: PreemptDisableGuard::new(kernel),
            _inner_guard: guard,
        })
    }

    pub fn lock(&self, kernel: K) -> SpinLockGuard<'_, K, T> {
        let inner_guard = self.inner.lock();
        SpinLockGuard {
            lock: self,
            _preempt_guard: PreemptDisableGuard::new(kernel),
            _inner_guard: inner_guard,
        }
    }

    /// Reconstructs a [`SpinLockGuard`] for a lock that is already held and whose
    /// ownership was transferred to the calling context (for example, across a
    /// context switch into a newly started thread).
    ///
    /// `interrupts_on_drop` controls whether dropping the returned guard will
    /// re-enable interrupts after unlocking ([`InterruptsOnDrop::Restore`]) or
    /// leave interrupts disabled ([`InterruptsOnDrop::KeepDisabled`]).
    ///
    /// # Safety
    /// The caller must guarantee that the underlying lock is currently held,
    /// interrupts and preemption are disabled (`preempt_disable_count >= 1`),
    /// and no other active guard exists for this lock.
    #[inline]
    pub unsafe fn adopt_locked(
        &self,
        kernel: K,
        interrupts_on_drop: InterruptsOnDrop,
    ) -> SpinLockGuard<'_, K, T> {
        // SAFETY: Caller guarantees the underlying lock is currently held,
        // interrupts are disabled, and no other active guard exists.
        let inner_guard = unsafe { self.inner.adopt_locked(interrupts_on_drop) };
        SpinLockGuard {
            lock: self,
            // SAFETY: Caller guarantees `preempt_disable_count >= 1` for an
            // active guard whose ownership was transferred to this context.
            _preempt_guard: unsafe { PreemptDisableGuard::adopt_locked(kernel) },
            _inner_guard: inner_guard,
        }
    }
}
