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
#![no_main]

use boot as _;
use pw_boot::entry;
use pw_sync_spinlock::InterruptSpinLock;
use pw_time::{Clock, Duration, SystemClock};

// Put loop cycle count behind a spinlock to demonstrate them working.
static CYCLES: InterruptSpinLock<usize> = InterruptSpinLock::new(0);

#[entry]
fn entry() -> ! {
    pw_log::info!("Rust system clock example");

    loop {
        let start = SystemClock::now();
        {
            let mut cycles = CYCLES.lock();
            *cycles += 1;
            pw_log::info!("Loop: {}", *cycles as usize);
        }

        while SystemClock::now() - start < Duration::from_millis(1000) {
            core::hint::spin_loop();
        }
    }
}
