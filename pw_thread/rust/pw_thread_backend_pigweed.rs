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
use target_arch::Arch;

pub fn sleep(sleep_duration: Duration<SystemClock>) {
    let wakeup_time = SystemClock::now() + sleep_duration;
    sleep_until(wakeup_time);
}

pub fn sleep_until(wakeup_time: Instant<SystemClock>) {
    let deadline = kernel::Instant::from_ticks(wakeup_time.ticks());
    let _ = kernel::sleep_until(Arch, deadline);
}

pub fn yield_now() {
    kernel::yield_timeslice(Arch);
}
