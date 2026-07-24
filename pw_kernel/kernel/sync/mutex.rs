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
use core::marker::PhantomData;
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

pub struct RawMutex<K: Kernel> {
    // An future optimization can be made by keeping an atomic count outside of
    // the spinlock.  However, not all architectures support atomics so a pure
    // SchedLock based approach will always be needed.
    state: WaitQueueLock<K, MutexState>,
}

impl<K: Kernel> RawMutex<K> {
    pub const fn new(kernel: K) -> Self {
        Self {
            state: WaitQueueLock::new(
                kernel,
                MutexState {
                    count: 0,
                    holder_thread_id: Thread::<K>::null_id(),
                },
            ),
        }
    }

    pub fn lock(&self) {
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
    }

    pub fn try_lock(&self) -> bool {
        let mut state = self.state.lock();
        if state.count != 0 {
            return false;
        }
        state.count = 1;
        state.holder_thread_id = state.sched().current_thread_id();
        true
    }

    // TODO - konkers: Investigate combining with lock().
    pub fn lock_until(&self, deadline: Instant<K::Clock>) -> Result<()> {
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

        Ok(())
    }

    /// Unlocks the mutex.
    ///
    /// # Safety
    ///
    /// The caller must ensure that:
    /// - The mutex is currently locked.
    /// - The current thread is the holder of the lock.
    ///
    /// # Panics
    ///
    /// Panics if:
    /// - The mutex is not currently locked (`state.count == 0`).
    /// - The current thread is not the holder of the lock.
    pub unsafe fn unlock(&self) {
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

pub struct Mutex<K: Kernel, T> {
    raw: RawMutex<K>,
    data: UnsafeCell<T>,
}

// SAFETY: Sharing a `&Mutex<K, T>` across threads allows another thread to obtain
// exclusive `&mut T` access via `MutexGuard`, which is safe as long as `T: Send`
// as the Mutex's contract ensure that only one thread had access (mutable or
// or otherwise to the enclosed data.
//
// For more information see:
// https://doc.rust-lang.org/std/sync/struct.Mutex.html#impl-Sync-for-Mutex%3CT%3E
unsafe impl<K: Kernel, T: Send> Sync for Mutex<K, T> {}

// SAFETY: Moving a `Mutex<K, T>` to another thread transfers ownership of `T`,
// which is safe as long as `T: Send`.
//
// For more information see:
// https://doc.rust-lang.org/std/sync/struct.Mutex.html#impl-Send-for-Mutex%3CT%3E
unsafe impl<K: Kernel, T: Send> Send for Mutex<K, T> {}

pub struct MutexGuard<'a, K: Kernel, T> {
    mutex: &'a Mutex<K, T>,
    // Implicitly mark MutexGuard as !Send and !Sync.  !Sync is re-implemented
    // below.  This is a workaround for `negative_impls` being unstable.
    _marker: PhantomData<*const ()>,
}

impl<K: Kernel, T> Drop for MutexGuard<'_, K, T> {
    fn drop(&mut self) {
        // SAFETY: `MutexGuard` is only created when the current thread successfully
        // acquires the mutex, so it is safe to unlock the underlying `RawMutex`.
        unsafe {
            self.mutex.raw.unlock();
        }
    }
}

// SAFETY: Shared reference access (`&MutexGuard`) provides `&T` through `Deref`,
// which is safe across threads when `T: Sync`.
unsafe impl<K: Kernel, T: Sync> Sync for MutexGuard<'_, K, T> {}

impl<K: Kernel, T> Deref for MutexGuard<'_, K, T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        // SAFETY: The mutex is held for the lifetime of `MutexGuard`, guaranteeing
        // exclusive access to `data`.  Lifetime of underlying data is tied to
        // the lifetime of the guard.
        unsafe { &*self.mutex.data.get() }
    }
}

impl<K: Kernel, T> DerefMut for MutexGuard<'_, K, T> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        // SAFETY: The mutex is held for the lifetime of `MutexGuard`, guaranteeing
        // exclusive access to `data`.  Lifetime of underlying data is tied to
        // the lifetime of the guard.
        unsafe { &mut *self.mutex.data.get() }
    }
}

impl<K: Kernel, T> Mutex<K, T> {
    pub const fn new(kernel: K, initial_value: T) -> Self {
        Self {
            raw: RawMutex::new(kernel),
            data: UnsafeCell::new(initial_value),
        }
    }

    pub fn lock(&self) -> MutexGuard<'_, K, T> {
        self.raw.lock();
        MutexGuard {
            mutex: self,
            _marker: PhantomData,
        }
    }

    pub fn try_lock(&self) -> Option<MutexGuard<'_, K, T>> {
        self.raw.try_lock().then(|| MutexGuard {
            mutex: self,
            _marker: PhantomData,
        })
    }

    pub fn lock_until(&self, deadline: Instant<K::Clock>) -> Result<MutexGuard<'_, K, T>> {
        self.raw.lock_until(deadline).map(|()| MutexGuard {
            mutex: self,
            _marker: PhantomData,
        })
    }
}
