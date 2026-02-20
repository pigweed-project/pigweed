// Copyright 2025 The Pigweed Authors
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

use kernel_config::{KernelConfig, KernelConfigInterface};

pub struct Clock;

impl time::Clock for Clock {
    const TICKS_PER_SEC: u64 = KernelConfig::SYSTEM_CLOCK_HZ;
    fn now() -> time::Instant<Self> {
        // Placeholder until a get_time() system call exists.
        Instant::from_ticks(0)
    }
}

pub type Instant = time::Instant<Clock>;
pub type Duration = time::Duration<Clock>;
