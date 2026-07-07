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

use pw_time_core::{Clock, Instant};

pub struct SystemClock;

extern "C" {
    fn sys_clock_tick_get() -> i64;
}

impl Clock for SystemClock {
    // TODO: https://pwbug.dev/527992421 - Pull this value from Kconfig.
    // Tick rate is enforced to be matching Kconfig by static assertion
    // in pw_time_backend_zephyr_helper.cc.
    const TICKS_PER_SEC: u64 = 10000;

    fn now() -> Instant<Self> {
        // SAFETY: `sys_clock_tick_get` is a read-only kernel query with no parameters
        // and is always safe to invoke from any context.
        let ticks = unsafe { sys_clock_tick_get() };
        Instant::from_ticks(ticks as u64)
    }
}
