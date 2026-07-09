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

use pw_time::{Clock, Duration, Instant, SystemClock};

pub fn sleep(sleep_duration: Duration<SystemClock>) {
    pw_assert::assert!(
        unsafe { freertos_sys::xPortIsInsideInterrupt() } == 0,
        "sleep cannot be called from an interrupt context"
    );

    const MAX_TIMEOUT_MINUS_ONE: u64 = (freertos_sys::TickType_t::MAX / 3 - 1) as u64;
    let mut ticks = sleep_duration.ticks() as u64;
    while ticks > MAX_TIMEOUT_MINUS_ONE {
        unsafe {
            freertos_sys::vTaskDelay(MAX_TIMEOUT_MINUS_ONE as freertos_sys::TickType_t);
        }
        ticks -= MAX_TIMEOUT_MINUS_ONE;
    }

    unsafe {
        freertos_sys::vTaskDelay((ticks + 1) as freertos_sys::TickType_t);
    }
}

pub fn sleep_until(wakeup_time: Instant<SystemClock>) {
    pw_assert::assert!(
        unsafe { freertos_sys::xPortIsInsideInterrupt() } == 0,
        "sleep_until cannot be called from an interrupt context"
    );

    let now = SystemClock::now();
    if wakeup_time <= now {
        return;
    }
    sleep(wakeup_time - now);
}

pub fn yield_now() {
    pw_assert::assert!(
        unsafe { freertos_sys::xPortIsInsideInterrupt() } == 0,
        "yield_now cannot be called from an interrupt context"
    );
    unsafe {
        freertos_sys::taskYIELD();
    }
}
