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
use core::ptr::NonNull;

use pw_status::Result;
use time::Instant;

use crate::Kernel;
use crate::scheduler::algorithm::RescheduleReason;
use crate::scheduler::{SchedulerState, WaitQueue, WaitType};
use crate::sync::spinlock::SpinLockGuard;

pub struct SmuggledSchedLock<K, T> {
    inner: NonNull<T>,
    kernel: K,
}

impl<K: Kernel, T> SmuggledSchedLock<K, T> {
    /// # Safety
    /// The caller must guarantee that the underlying lock and it's enclosed data
    /// is still valid.
    pub unsafe fn lock(&self) -> SchedLockGuard<'_, K, T> {
        let guard = self.kernel.get_scheduler().lock(self.kernel);
        SchedLockGuard {
            guard,
            inner: unsafe { &mut *self.inner.as_ptr() },
            kernel: self.kernel,
        }
    }
}

pub struct SchedLockGuard<'lock, K: Kernel, T> {
    guard: SpinLockGuard<'lock, K, SchedulerState<K>>,
    inner: &'lock mut T,
    pub(super) kernel: K,
}

impl<'lock, K: Kernel, T> SchedLockGuard<'lock, K, T> {
    #[must_use]
    pub fn sched(&self) -> &SpinLockGuard<'lock, K, SchedulerState<K>> {
        &self.guard
    }

    #[must_use]
    pub fn sched_mut(&mut self) -> &mut SpinLockGuard<'lock, K, SchedulerState<K>> {
        &mut self.guard
    }

    #[must_use]
    pub fn block(self, current_thread_id: usize) -> Self {
        let inner = self.inner;
        let kernel = self.kernel;
        let guard = super::block(kernel, self.guard, current_thread_id);
        Self {
            guard,
            inner,
            kernel,
        }
    }

    #[allow(clippy::return_self_not_must_use, clippy::must_use_candidate)]
    pub fn try_reschedule(self, reason: RescheduleReason) -> Self {
        let inner = self.inner;
        let kernel = self.kernel;
        let guard = self.guard.try_reschedule(kernel, reason);
        Self {
            guard,
            inner,
            kernel,
        }
    }

    /// # Safety
    /// The caller must guarantee that the underlying lock remains valid and
    /// un-moved for the live the smuggled lock.
    #[must_use]
    pub unsafe fn smuggle(&self) -> SmuggledSchedLock<K, T> {
        let inner: *const T = self.inner;
        SmuggledSchedLock {
            inner: unsafe { NonNull::new_unchecked(inner.cast_mut()) },
            kernel: self.kernel,
        }
    }
}

impl<K: Kernel, T> Deref for SchedLockGuard<'_, K, T> {
    type Target = T;

    fn deref(&self) -> &T {
        self.inner
    }
}

impl<K: Kernel, T> DerefMut for SchedLockGuard<'_, K, T> {
    fn deref_mut(&mut self) -> &mut T {
        self.inner
    }
}

/// An owning lock that shares the global scheduler lock.
///
/// A [`SchedLockGuard`] can be turned into a `SpinLockGuard<'lock, SchedulerState>`
/// so that it can be passed to `reschedule()`
///
/// # Safety
/// Taking two different `SchedLock`s at the same time will deadlock as they
/// share the same underlying lock.
pub struct SchedLock<K, T> {
    inner: UnsafeCell<T>,
    kernel: K,
}
unsafe impl<K: Sync, T> Sync for SchedLock<K, T> {}
unsafe impl<K: Send, T> Send for SchedLock<K, T> {}

impl<K, T> SchedLock<K, T> {
    pub const fn new(kernel: K, initial_value: T) -> Self {
        Self {
            inner: UnsafeCell::new(initial_value),
            kernel,
        }
    }
}

impl<K: Kernel, T> SchedLock<K, T> {
    #[allow(unused)]
    pub fn try_lock(&self) -> Option<SchedLockGuard<'_, K, T>> {
        // Safety: The lock guarantees
        self.kernel
            .get_scheduler()
            .try_lock(self.kernel)
            .map(|guard| SchedLockGuard {
                inner: unsafe { &mut *self.inner.get() },
                guard,
                kernel: self.kernel,
            })
    }

    pub fn lock(&self) -> SchedLockGuard<'_, K, T> {
        let guard = self.kernel.get_scheduler().lock(self.kernel);
        SchedLockGuard {
            inner: unsafe { &mut *self.inner.get() },
            guard,
            kernel: self.kernel,
        }
    }
}

pub struct WaitQueueLockState<K: Kernel, T> {
    queue: WaitQueue<K>,
    inner: T,
}

pub struct WaitQueueLock<K: Kernel, T> {
    state: SchedLock<K, WaitQueueLockState<K, T>>,
}

impl<K: Kernel, T> WaitQueueLock<K, T> {
    pub const fn new(kernel: K, initial_value: T) -> Self {
        Self {
            state: SchedLock::new(
                kernel,
                WaitQueueLockState {
                    queue: WaitQueue::new(),
                    inner: initial_value,
                },
            ),
        }
    }

    pub fn lock(&self) -> WaitQueueLockGuard<'_, K, T> {
        WaitQueueLockGuard {
            inner: self.state.lock(),
        }
    }
}

pub struct WaitQueueLockGuard<'lock, K: Kernel, T> {
    inner: SchedLockGuard<'lock, K, WaitQueueLockState<K, T>>,
}

impl<'lock, K: Kernel, T> WaitQueueLockGuard<'lock, K, T> {
    pub fn sched(&self) -> &SpinLockGuard<'lock, K, SchedulerState<K>> {
        &self.inner.guard
    }

    #[allow(dead_code)]
    pub fn sched_mut(&mut self) -> &mut SpinLockGuard<'lock, K, SchedulerState<K>> {
        &mut self.inner.guard
    }

    pub fn operate_on_wait_queue<F, R>(mut self, f: F) -> (Self, R)
    where
        F: FnOnce(
            SchedLockGuard<'lock, K, WaitQueue<K>>,
        ) -> (SchedLockGuard<'lock, K, WaitQueue<K>>, R),
    {
        let guard = SchedLockGuard::<'lock, _, WaitQueue<K>> {
            guard: self.inner.guard,
            // Safety: Mutable reference only lives as long as the call into f()
            #[allow(clippy::deref_addrof)]
            inner: unsafe { &mut *&raw mut self.inner.inner.queue },
            kernel: self.inner.kernel,
        };
        let (guard, result) = f(guard);
        self.inner.guard = guard.guard;
        (self, result)
    }

    pub fn wait_until(
        self,
        wait_type: WaitType,
        deadline: Instant<K::Clock>,
    ) -> (Self, Result<()>) {
        self.operate_on_wait_queue(|guard| guard.wait_until(wait_type, deadline))
    }

    pub fn wait(self, wait_type: WaitType) -> (Self, Result<()>) {
        self.operate_on_wait_queue(|guard| guard.wait(wait_type))
    }

    pub fn wake_one(self) -> (Self, super::WakeResult) {
        self.operate_on_wait_queue(|guard| guard.wake_one())
    }

    pub fn wake_all(self) -> Self {
        self.operate_on_wait_queue(|guard| (guard.wake_all(), ())).0
    }
}

impl<K: Kernel, T> Deref for WaitQueueLockGuard<'_, K, T> {
    type Target = T;

    fn deref(&self) -> &T {
        &self.inner.inner.inner
    }
}

impl<K: Kernel, T> DerefMut for WaitQueueLockGuard<'_, K, T> {
    fn deref_mut(&mut self) -> &mut T {
        &mut self.inner.inner.inner
    }
}
