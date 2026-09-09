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

use pw_sync_mutex_core::{RawMutex as RawMutexTrait, RawTimedMutex as RawTimedMutexTrait};
use pw_time::Clock;

/// Native FreeRTOS implementation of
/// [`RawMutex`][pw_sync_mutex_core::RawMutex] and
/// [`RawTimedMutex`][pw_sync_mutex_core::RawTimedMutex].
///
/// This backend uses dynamically allocated FreeRTOS mutexes
/// (`xSemaphoreCreateMutex`) rather than static semaphores
/// (`xSemaphoreCreateMutexStatic`) so that the resulting mutex can be safely
/// moved in memory without invalidating FreeRTOS internal address references.
pub struct RawTimedMutex {
    handle: freertos_sys::SemaphoreHandle_t,
}

// SAFETY: FreeRTOS dynamic semaphores are thread-safe primitives.
unsafe impl Sync for RawTimedMutex {}
// SAFETY: FreeRTOS dynamic semaphore handles can be transferred across thread
// boundaries.
unsafe impl Send for RawTimedMutex {}

impl Default for RawTimedMutex {
    fn default() -> Self {
        let handle = unsafe { freertos_sys::xSemaphoreCreateMutex() };
        pw_assert::assert!(!handle.is_null(), "Failed to allocate FreeRTOS mutex");
        Self { handle }
    }
}

impl Drop for RawTimedMutex {
    fn drop(&mut self) {
        unsafe {
            freertos_sys::vSemaphoreDelete(self.handle);
        }
    }
}

pub type RawMutex = RawTimedMutex;

unsafe impl RawMutexTrait for RawTimedMutex {
    fn try_lock(&self) -> bool {
        pw_assert::assert!(
            // SAFETY: `xPortIsInsideInterrupt` is a safe, read-only FreeRTOS
            // status check.
            unsafe { freertos_sys::xPortIsInsideInterrupt() } == 0,
            "Mutex cannot be locked from an interrupt context"
        );
        // SAFETY: `handle` points to a valid FreeRTOS mutex semaphore.
        unsafe { freertos_sys::xSemaphoreTake(self.handle, 0) != 0 }
    }

    fn lock(&self) {
        pw_assert::assert!(
            // SAFETY: `xPortIsInsideInterrupt` is a safe, read-only FreeRTOS
            // status check.
            unsafe { freertos_sys::xPortIsInsideInterrupt() } == 0,
            "Mutex cannot be locked from an interrupt context"
        );
        pw_assert::assert!(
            // SAFETY: `handle` points to a valid FreeRTOS mutex semaphore.
            unsafe { freertos_sys::xSemaphoreTake(self.handle, freertos_sys::TickType_t::MAX,) }
                != 0,
            "Failed to lock mutex"
        );
    }

    unsafe fn unlock(&self) {
        pw_assert::assert!(
            // SAFETY: `handle` points to a valid FreeRTOS mutex semaphore.
            unsafe { freertos_sys::xSemaphoreGive(self.handle) } != 0,
            "Failed to unlock mutex"
        );
    }
}

unsafe impl RawTimedMutexTrait for RawTimedMutex {
    type Duration = pw_time::Duration<pw_time::SystemClock>;
    type Instant = pw_time::Instant<pw_time::SystemClock>;

    fn try_lock_for(&self, timeout: Self::Duration) -> bool {
        pw_assert::assert!(
            // SAFETY: `xPortIsInsideInterrupt` is a safe, read-only FreeRTOS
            // status check.
            unsafe { freertos_sys::xPortIsInsideInterrupt() } == 0,
            "Mutex cannot be locked from an interrupt context"
        );
        if timeout.ticks() == 0 {
            return self.try_lock();
        }
        let ticks = if timeout.ticks() >= (freertos_sys::TickType_t::MAX as u64) {
            freertos_sys::TickType_t::MAX - 1
        } else {
            timeout.ticks() as freertos_sys::TickType_t
        };
        // SAFETY: `handle` points to a valid FreeRTOS mutex semaphore.
        unsafe { freertos_sys::xSemaphoreTake(self.handle, ticks) != 0 }
    }

    fn try_lock_until(&self, deadline: Self::Instant) -> bool {
        self.try_lock_for(deadline - pw_time::SystemClock::now())
    }
}
