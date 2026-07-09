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

use freertos_sys::TickType_t;
use pw_sync_spinlock::InterruptSpinLock;
use pw_time_core::{Clock, Instant};

#[derive(Debug)]
pub struct SystemClock;

#[inline(always)]
fn get_tick_count() -> TickType_t {
    // SAFETY: The correct `xTaskGetTickCount*` function is called depending on
    // the current CPU context as queried by `xPortIsInsideInterrupt()`.
    unsafe {
        if freertos_sys::xPortIsInsideInterrupt() != 0 {
            freertos_sys::xTaskGetTickCountFromISR()
        } else {
            freertos_sys::xTaskGetTickCount()
        }
    }
}

struct TickState {
    overflow_tick_count: u64,
    native_tick_count: TickType_t,
}

static TICK_STATE: InterruptSpinLock<TickState> = InterruptSpinLock::new(TickState {
    overflow_tick_count: 0,
    native_tick_count: 0,
});

impl Clock for SystemClock {
    const TICKS_PER_SEC: u64 = freertos_sys::configTICK_RATE_HZ as u64;

    fn now() -> Instant<Self> {
        let mut tick_state = TICK_STATE.lock();

        let tick_count = get_tick_count();

        // WARNING: This must be called more than once per overflow period!
        if tick_count < tick_state.native_tick_count {
            tick_state.overflow_tick_count = tick_state
                .overflow_tick_count
                .checked_add(u64::from(TickType_t::MAX) + 1)
                .expect("clock tick count overflow");
        }
        tick_state.native_tick_count = tick_count;

        let ticks = tick_state.overflow_tick_count + tick_state.native_tick_count as u64;

        Instant::from_ticks(ticks)
    }
}
