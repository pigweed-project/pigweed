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

use pw_sync_spinlock::InterruptSpinLock;
use pw_time_core::{Clock, Instant};

#[derive(Debug)]
pub struct SystemClock;

extern "C" {
    // TickType_t type enforced to be a u32 by static assertion in
    // pw_time_backend_freertos_helper.cc.
    fn xTaskGetTickCount() -> u32;
    fn xTaskGetTickCountFromISR() -> u32;
    fn xPortIsInsideInterrupt() -> i32;
}

#[inline(always)]
fn get_tick_count() -> u32 {
    // SAFETY: The correct `xTaskGetTickCount*` function is called depending on
    // the current CPU context as queried by `xPortIsInsideInterrupt()`.
    unsafe {
        if xPortIsInsideInterrupt() != 0 {
            xTaskGetTickCountFromISR()
        } else {
            xTaskGetTickCount()
        }
    }
}

struct TickState {
    overflow_tick_count: u64,
    native_tick_count: u32,
}

static TICK_STATE: InterruptSpinLock<TickState> = InterruptSpinLock::new(TickState {
    overflow_tick_count: 0,
    native_tick_count: 0,
});

impl Clock for SystemClock {
    // TODO: https://pwbug.dev/524327314 - Pull this value from FreeRTOSConfig.h.
    // Tick rate is enforced by to be matching FreeRTOSConfig.h by static assertion
    // in pw_time_backend_freertos_helper.cc.
    const TICKS_PER_SEC: u64 = 1000;

    fn now() -> Instant<Self> {
        let mut tick_state = TICK_STATE.lock();

        let tick_count = get_tick_count();

        // WARNING: This must be called more than once per overflow period!
        if tick_count < tick_state.native_tick_count {
            tick_state.overflow_tick_count = tick_state
                .overflow_tick_count
                .checked_add(u64::from(u32::MAX) + 1)
                .expect("clock tick count overflow");
        }
        tick_state.native_tick_count = tick_count;

        let ticks = tick_state.overflow_tick_count + tick_state.native_tick_count as u64;

        Instant::from_ticks(ticks)
    }
}
