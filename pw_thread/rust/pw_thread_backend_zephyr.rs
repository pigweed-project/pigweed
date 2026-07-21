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

extern "C" {
    fn pw_thread_zephyr_Yield();
    fn pw_thread_zephyr_Sleep(ticks: u64) -> i32;
    fn pw_thread_zephyr_SleepUntil(ticks: u64) -> i32;
    fn k_is_in_isr() -> bool;
}

pub fn sleep(sleep_duration: Duration<SystemClock>) {
    pw_assert::assert!(
        unsafe { !k_is_in_isr() },
        "sleep cannot be called from an interrupt context"
    );

    let ticks = sleep_duration.ticks();
    let remaining = unsafe { pw_thread_zephyr_Sleep(ticks) };
    pw_assert::assert!(
        remaining == 0,
        "Zephyr sleep failed or was interrupted early"
    );
}

pub fn sleep_until(wakeup_time: Instant<SystemClock>) {
    pw_assert::assert!(
        unsafe { !k_is_in_isr() },
        "sleep_until cannot be called from an interrupt context"
    );

    let now = SystemClock::now();
    if wakeup_time <= now {
        return;
    }
    let ticks = wakeup_time.ticks();
    let remaining = unsafe { pw_thread_zephyr_SleepUntil(ticks) };
    pw_assert::assert!(
        remaining == 0,
        "Zephyr sleep_until failed or was interrupted early"
    );
}

pub fn yield_now() {
    pw_assert::assert!(
        unsafe { !k_is_in_isr() },
        "yield_now cannot be called from an interrupt context"
    );
    unsafe {
        pw_thread_zephyr_Yield();
    }
}
