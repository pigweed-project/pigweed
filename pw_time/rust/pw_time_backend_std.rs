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

extern crate std;

#[derive(Debug)]
pub struct SystemClock;

impl pw_time_core::Clock for SystemClock {
    const TICKS_PER_SEC: u64 = 1_000_000_000; // Nanosecond resolution on host.
    fn now() -> pw_time_core::Instant<Self> {
        use std::sync::OnceLock;
        use std::time::Instant as StdInstant;

        // Because [`std::time::Instant`] has no declared epoch, we establish one
        // the first time `now()` is called. Though [`std::time::SystemTime`] does
        // provide a known epoch, it is non-monotonic and can't be used to fulfill
        // `SystemClock`'s contract.
        static EPOCH: OnceLock<StdInstant> = OnceLock::new();

        let start = EPOCH.get_or_init(StdInstant::now);
        let elapsed = start.elapsed();
        let Ok(ticks) = elapsed.as_nanos().try_into() else {
            pw_assert::panic!("Duration since EPOCH overflowed 64 bits");
        };

        pw_time_core::Instant::from_ticks(ticks)
    }
}
