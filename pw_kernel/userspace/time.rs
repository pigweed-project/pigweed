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

use kernel_config::{KernelConfig, KernelConfigInterface};
use pw_status::{Error, Result};
pub use pw_time_core::Clock;
use syscall_user::{SysCall, SysCallInterface};

pub type Instant = pw_time_core::Instant<SystemClock>;
pub type Duration = pw_time_core::Duration<SystemClock>;

#[derive(Debug)]
pub struct SystemClock;

impl pw_time_core::Clock for SystemClock {
    const TICKS_PER_SEC: u64 = KernelConfig::SYSTEM_CLOCK_HZ;
    fn now() -> pw_time_core::Instant<Self> {
        let ticks = SysCall::debug_clock_now();
        pw_time_core::Instant::from_ticks(ticks)
    }
}

pub fn sleep_until(deadline: Instant) -> Result<()> {
    // Object handle 0 is always the local process and can be waited on with
    // a blank signal mask to wait until the deadline has passed.
    match SysCall::object_wait(
        0,
        syscall_defs::Signals::no_active().bits(),
        deadline.ticks(),
    ) {
        // object_wait returned early.
        Ok(_) => Err(Error::Cancelled),

        // Deadline exceeded is the expected return.
        Err(Error::DeadlineExceeded) => Ok(()),

        // Other errors are passed through.
        Err(err) => Err(err),
    }
}
