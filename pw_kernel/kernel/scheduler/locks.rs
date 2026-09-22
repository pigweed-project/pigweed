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
use pw_time_core::Instant;

use crate::Kernel;
use crate::scheduler::thread::State;
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
    // The `'static` lifetime applies to `'sched`, which is sound because
    // `Kernel::get_scheduler()` returns a `&'static SpinLock`.
    pub unsafe fn lock(&self) -> SchedLockGuard<'_, 'static, K, T> {
        let guard = self.kernel.get_scheduler().lock(self.kernel);
        SchedLockGuard {
            guard,
            inner: unsafe { &mut *self.inner.as_ptr() },
            kernel: self.kernel,
        }
    }
}

pub struct SchedLockGuard<'lock, 'sched, K: Kernel, T> {
    guard: SpinLockGuard<'sched, K, SchedulerState<K>>,
    inner: &'lock mut T,
    pub(super) kernel: K,
}

impl<'lock, 'sched, K: Kernel, T> SchedLockGuard<'lock, 'sched, K, T> {
    #[must_use]
    pub fn sched(&self) -> &SpinLockGuard<'sched, K, SchedulerState<K>> {
        &self.guard
    }

    #[must_use]
    pub fn sched_mut(&mut self) -> &mut SpinLockGuard<'sched, K, SchedulerState<K>> {
        &mut self.guard
    }

    #[must_use]
    pub fn block(self, current_thread_id: usize, current_thead_state: State) -> Self {
        let inner = self.inner;
        let kernel = self.kernel;
        let guard = super::block(kernel, self.guard, current_thread_id, current_thead_state);
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

impl<K: Kernel, T> Deref for SchedLockGuard<'_, '_, K, T> {
    type Target = T;

    fn deref(&self) -> &T {
        self.inner
    }
}

impl<K: Kernel, T> DerefMut for SchedLockGuard<'_, '_, K, T> {
    fn deref_mut(&mut self) -> &mut T {
        self.inner
    }
}

/// An owning lock that shares the global scheduler lock.
///
/// A [`SchedLockGuard`] can be turned into a `SpinLockGuard<'sched, SchedulerState>`
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
    // The `'static` lifetime applies to `'sched`, which is sound because
    // `Kernel::get_scheduler()` returns a `&'static SpinLock`.
    #[allow(unused)]
    pub fn try_lock(&self) -> Option<SchedLockGuard<'_, 'static, K, T>> {
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

    // The `'static` lifetime applies to `'sched`, which is sound because
    // `Kernel::get_scheduler()` returns a `&'static SpinLock`.
    pub fn lock(&self) -> SchedLockGuard<'_, 'static, K, T> {
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

    // The `'static` lifetime applies to `'sched`, which is sound because
    // `SchedLock::lock()` locks the `&'static SpinLock` from `Kernel::get_scheduler()`.
    pub fn lock(&self) -> WaitQueueLockGuard<'_, 'static, K, T> {
        WaitQueueLockGuard {
            inner: self.state.lock(),
        }
    }

    pub(crate) fn inherit_sched_lock<'lock, 'sched>(
        &'lock self,
        guard: SpinLockGuard<'sched, K, SchedulerState<K>>,
    ) -> WaitQueueLockGuard<'lock, 'sched, K, T> {
        WaitQueueLockGuard {
            inner: SchedLockGuard {
                inner: unsafe { &mut *self.state.inner.get() },
                guard,
                kernel: self.state.kernel,
            },
        }
    }
}

pub struct WaitQueueLockGuard<'lock, 'sched, K: Kernel, T> {
    inner: SchedLockGuard<'lock, 'sched, K, WaitQueueLockState<K, T>>,
}

impl<'lock, 'sched, K: Kernel, T> WaitQueueLockGuard<'lock, 'sched, K, T> {
    pub fn sched(&self) -> &SpinLockGuard<'sched, K, SchedulerState<K>> {
        &self.inner.guard
    }

    pub fn into_sched(self) -> SpinLockGuard<'sched, K, SchedulerState<K>> {
        self.inner.guard
    }

    #[allow(dead_code)]
    pub fn sched_mut(&mut self) -> &mut SpinLockGuard<'sched, K, SchedulerState<K>> {
        &mut self.inner.guard
    }

    #[must_use]
    pub fn operate_on_wait_queue<F, R>(mut self, f: F) -> (Self, R)
    where
        F: FnOnce(
            SchedLockGuard<'lock, 'sched, K, WaitQueue<K>>,
        ) -> (SchedLockGuard<'lock, 'sched, K, WaitQueue<K>>, R),
    {
        let guard = SchedLockGuard::<'lock, 'sched, _, WaitQueue<K>> {
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

    /// Wakes all threads in `wake_list` and reschedules if needed.
    pub fn wake_list_and_reschedule(
        self,
        wake_list: super::WakeList<K>,
    ) -> SpinLockGuard<'sched, K, SchedulerState<K>> {
        let kernel = self.inner.kernel;
        let sched = self.into_sched();
        wake_list.wake_and_reschedule(kernel, sched)
    }

    /// Dequeues and wakes one waiter, rescheduling if needed.
    pub fn wake_one_and_reschedule(mut self) -> SpinLockGuard<'sched, K, SchedulerState<K>> {
        let mut wake_list = super::WakeList::new();
        let _ = self.dequeue_one(&mut wake_list);
        self.wake_list_and_reschedule(wake_list)
    }

    /// Dequeues and wakes all waiters, rescheduling if needed.
    pub fn wake_all_and_reschedule(mut self) -> SpinLockGuard<'sched, K, SchedulerState<K>> {
        let mut wake_list = super::WakeList::new();
        let _ = self.dequeue_all(&mut wake_list);
        self.wake_list_and_reschedule(wake_list)
    }

    /// Dequeues the head of the wait queue into `list` without waking it.
    pub fn dequeue_one(&mut self, list: &mut super::WakeList<K>) -> super::WakeResult {
        self.inner.inner.queue.dequeue_one(list)
    }

    /// Dequeues all threads from the wait queue into `list` without waking them.
    pub fn dequeue_all(&mut self, list: &mut super::WakeList<K>) -> super::WakeResult {
        self.inner.inner.queue.dequeue_all(list)
    }
}

impl<K: Kernel, T> Deref for WaitQueueLockGuard<'_, '_, K, T> {
    type Target = T;

    fn deref(&self) -> &T {
        &self.inner.inner.inner
    }
}

impl<K: Kernel, T> DerefMut for WaitQueueLockGuard<'_, '_, K, T> {
    fn deref_mut(&mut self) -> &mut T {
        &mut self.inner.inner.inner
    }
}
