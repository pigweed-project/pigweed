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

use kernel::Arch as _;
use kernel_config::{KernelConfig, KernelConfigInterface};
use pw_time_core::{Clock, Instant};
use target_arch::Arch;

pub struct SystemClock;

impl Clock for SystemClock {
    const TICKS_PER_SEC: u64 = KernelConfig::SYSTEM_CLOCK_HZ;

    fn now() -> Instant<Self> {
        let ticks = Arch.now().ticks();
        Instant::from_ticks(ticks)
    }
}
