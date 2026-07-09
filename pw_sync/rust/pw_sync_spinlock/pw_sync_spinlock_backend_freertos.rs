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

use core::cell::Cell;

const SCHEDULER_NOT_STARTED: freertos_sys::BaseType_t = 0;
const USE_SCHEDULER_LOCK: bool =
    pw_sync_spinlock_backend_freertos_config::kInterruptSpinLockUsesSchedulerLock;

/// FreeRTOS-specific implementation of the [`RawInterruptSpinLock`][pw_sync_spinlock_core::RawInterruptSpinLock] trait.
pub struct RawInterruptSpinLock {
    locked: Cell<bool>,
}

// Safety: `RawInterruptSpinLock` is safe to share across threads because the internal
// `Cell` fields are only accessed while a FreeRTOS critical section is active,
// which guarantees exclusive access.
unsafe impl Sync for RawInterruptSpinLock {}

/// RAII guard returned by [`RawInterruptSpinLock::lock`] or [`RawInterruptSpinLock::try_lock`].
pub struct RawInterruptSpinLockGuard<'a> {
    lock: &'a RawInterruptSpinLock,
    saved_interrupt_mask: u32,
}

impl<'a> Drop for RawInterruptSpinLockGuard<'a> {
    #[inline]
    fn drop(&mut self) {
        self.lock.locked.set(false);
        unsafe {
            if freertos_sys::xPortIsInsideInterrupt() != 0 {
                freertos_sys::taskEXIT_CRITICAL_FROM_ISR(self.saved_interrupt_mask);
            } else {
                freertos_sys::taskEXIT_CRITICAL();
                if USE_SCHEDULER_LOCK
                    && freertos_sys::xTaskGetSchedulerState() != SCHEDULER_NOT_STARTED
                {
                    freertos_sys::xTaskResumeAll();
                }
            }
        }
    }
}

impl RawInterruptSpinLock {
    const fn new() -> Self {
        Self {
            locked: Cell::new(false),
        }
    }
}

impl pw_sync_spinlock_core::RawInterruptSpinLock for RawInterruptSpinLock {
    type Guard<'a> = RawInterruptSpinLockGuard<'a>;

    const NEW: Self = Self::new();

    #[inline]
    fn try_lock(&self) -> Option<Self::Guard<'_>> {
        // This backend does not support SMP and on a uniprocessor we cannot actually
        // fail to acquire the lock. Recursive locking is already detected by lock().
        Some(self.lock())
    }

    #[inline]
    fn lock(&self) -> Self::Guard<'_> {
        let mask = unsafe {
            if freertos_sys::xPortIsInsideInterrupt() != 0 {
                freertos_sys::taskENTER_CRITICAL_FROM_ISR()
            } else {
                if USE_SCHEDULER_LOCK
                    && freertos_sys::xTaskGetSchedulerState() != SCHEDULER_NOT_STARTED
                {
                    // Suspending the scheduler ensures that kernel API calls that occur
                    // within the critical section will not preempt the current task
                    // (if called from a thread context).  Otherwise, kernel APIs called
                    // from within the critical section may preempt the running task if
                    // the port implements portYIELD synchronously.
                    // Note: calls to vTaskSuspendAll(), like taskENTER_CRITICAL() can
                    // be nested.
                    // Note: vTaskSuspendAll()/xTaskResumeAll() are not safe to call before the
                    // scheduler has been started.
                    freertos_sys::vTaskSuspendAll();
                }
                freertos_sys::taskENTER_CRITICAL();
                0
            }
        };
        // We can't deadlock here so crash instead.
        pw_assert::assert!(!self.locked.get());
        self.locked.set(true);
        RawInterruptSpinLockGuard {
            lock: self,
            saved_interrupt_mask: mask,
        }
    }
}
