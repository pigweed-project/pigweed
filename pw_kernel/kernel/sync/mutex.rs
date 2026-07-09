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

use pw_status::Result;
use pw_time_core::Instant;

use crate::Kernel;
use crate::scheduler::{Thread, WaitQueueLock, WaitType};

const MUTEX_DEBUG: bool = false;
macro_rules! mutex_debug {
  ($($args:expr),*) => {{
    log_if::debug_if!(MUTEX_DEBUG, $($args),*)
  }}
}

struct MutexState {
    count: usize,
    holder_thread_id: usize,
}

pub struct Mutex<K: Kernel, T> {
    // An future optimization can be made by keeping an atomic count outside of
    // the spinlock.  However, not all architectures support atomics so a pure
    // SchedLock based approach will always be needed.
    state: WaitQueueLock<K, MutexState>,

    inner: UnsafeCell<T>,
}
unsafe impl<K: Kernel, T> Sync for Mutex<K, T> {}
unsafe impl<K: Kernel, T> Send for Mutex<K, T> {}

pub struct MutexGuard<'lock, K: Kernel, T> {
    lock: &'lock Mutex<K, T>,
}

impl<K: Kernel, T> Deref for MutexGuard<'_, K, T> {
    type Target = T;

    fn deref(&self) -> &T {
        unsafe { &*self.lock.inner.get() }
    }
}

impl<K: Kernel, T> DerefMut for MutexGuard<'_, K, T> {
    fn deref_mut(&mut self) -> &mut T {
        unsafe { &mut *self.lock.inner.get() }
    }
}

impl<K: Kernel, T> Drop for MutexGuard<'_, K, T> {
    fn drop(&mut self) {
        self.lock.unlock();
    }
}

impl<K: Kernel, T> Mutex<K, T> {
    pub const fn new(kernel: K, initial_value: T) -> Self {
        Self {
            state: WaitQueueLock::new(
                kernel,
                MutexState {
                    count: 0,
                    holder_thread_id: Thread::<K>::null_id(),
                },
            ),
            inner: UnsafeCell::new(initial_value),
        }
    }

    pub fn lock(&self) -> MutexGuard<'_, K, T> {
        let mut state = self.state.lock();
        pw_assert::ne!(
            state.holder_thread_id as usize,
            state.sched().current_thread_id() as usize,
            "Mutex attempted to be locked by holding thread"
        );

        #[allow(clippy::needless_else)]
        if let Some(val) = state.count.checked_add(1) {
            state.count = val;
        } else {
            pw_assert::debug_assert!(false);
        }

        // TODO - konkers: investigate using core::intrinsics::unlikely() or
        //                 core::hint::unlikely()
        if state.count > 1 {
            mutex_debug!(
                "Mutex {:#010x}: lock wait by thread '{}' ({:#010x})",
                &raw const *self as usize,
                state.sched().current_thread_name() as &str,
                state.sched().current_thread_id() as usize
            );
            // Mutexes use uninterruptible waits because cleaning up a terminating
            // thread may involve aquisiation of mutex protected resources.
            let res;
            (state, res) = state.wait(WaitType::NonInterruptible);
            pw_assert::debug_assert!(res.is_ok());
        }
        mutex_debug!(
            "Mutex {:#010x}: lock acquired by thread '{}' ({:#010x})",
            &raw const *self as usize,
            state.sched().current_thread_name() as &str,
            state.sched().current_thread_id() as usize
        );

        state.holder_thread_id = state.sched().current_thread_id();

        // At this point we have exclusive access to `self.inner`.

        MutexGuard { lock: self }
    }

    // TODO - konkers: Investigate combining with lock().
    pub fn lock_until(&self, deadline: Instant<K::Clock>) -> Result<MutexGuard<'_, K, T>> {
        let mut state = self.state.lock();

        #[allow(clippy::needless_else)]
        if let Some(val) = state.count.checked_add(1) {
            state.count = val;
        } else {
            pw_assert::debug_assert!(false);
        }

        // TODO - konkers: investigate using core::intrinsics::unlikely() or
        //                 core::hint::unlikely()
        if state.count > 1 {
            let result;
            mutex_debug!(
                "Mutex {:#010x}: lock_until({}) wait by thread '{}' ({:#010x})",
                &raw const *self as usize,
                deadline.ticks() as u64,
                state.sched().current_thread_name() as &str,
                state.sched().current_thread_id() as usize
            );

            // Mutexes use uninterruptible waits because cleaning up a terminating
            // thread may involve aquisiation of mutex protected resources.
            (state, result) = state.wait_until(WaitType::NonInterruptible, deadline);

            if let Err(e) = result {
                mutex_debug!(
                    "Mutex {:#010x}: lock_until error: {} for thread '{}' ({:#010x})",
                    &raw const *self as usize,
                    e as u32,
                    state.sched().current_thread_name() as &str,
                    state.sched().current_thread_id() as usize
                );

                if let Some(val) = state.count.checked_sub(1) {
                    state.count = val;
                } else {
                    // use assert not debug_assert, as it's possible a
                    // real bug could trigger this assert,
                    pw_assert::assert!(false)
                }
                return Err(e);
            }
        }
        mutex_debug!(
            "Mutex {:#010x}: lock_until acquired by thread '{}' ({:#010x})",
            &raw const *self as usize,
            state.sched().current_thread_name() as &str,
            state.sched().current_thread_id() as usize
        );

        state.holder_thread_id = state.sched().current_thread_id();

        // At this point we have exclusive access to `self.inner`.

        Ok(MutexGuard { lock: self })
    }

    fn unlock(&self) {
        let mut state = self.state.lock();

        pw_assert::assert!(state.count > 0);
        pw_assert::eq!(
            state.holder_thread_id as usize,
            state.sched().current_thread_id() as usize
        );
        state.holder_thread_id = Thread::<K>::null_id();

        state.count -= 1;

        // TODO - konkers: investigate using core::intrinsics::unlikely() or
        //                 core::hint::unlikely()
        if state.count > 0 {
            let _ = state.wake_one();
        }
    }
}
