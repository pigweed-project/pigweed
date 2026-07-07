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
use pw_thread::{sleep, sleep_until};
use pw_time::{Clock, Duration, SystemClock};

#[entry]
fn entry() -> ! {
    pw_log::info!("Rust sleep example");

    let mut cycles = 0;
    loop {
        cycles += 1;
        pw_log::info!("Loop (sleep): {}", cycles);

        // Sleep for 500 ms using sleep()
        sleep(Duration::from_millis(500));

        // Sleep until 500 ms from now using sleep_until()
        let now = SystemClock::now();
        pw_log::info!("Loop (sleep_until): {}", cycles);
        sleep_until(now + Duration::from_millis(500));
    }
}
