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

#[entry]
fn entry() -> ! {
    pw_log::info!("Rust loop example");

    let mut count: usize = 0;
    loop {
        pw_log::info!("Loop: {}", count as i32);
        count += 1;
        // Convert to spinning on SystemClock::now() once pw_time lands.
        unsafe {
            vTaskDelay(1000);
        }
    }
}

extern "C" {
    fn vTaskDelay(x_ticks_to_delay: u32);
}
