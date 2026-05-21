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

use pw_status::Result;
use time::Clock;
use userspace::time::{Duration, sleep_until};
use userspace::{entry, syscall};

fn clock_test() -> Result<()> {
    test_logger::step_start!("Testing SystemClock::now() advances");
    let start = userspace::time::SystemClock::now();
    let mut end = start;
    let mut count = 0;
    while end == start && count < 1000000 {
        end = userspace::time::SystemClock::now();
        count += 1;
    }
    if end > start {
        test_logger::step_passed!("Clock advanced");
        Ok(())
    } else {
        test_logger::step_failed!("Clock did not advance");
        Err(pw_status::Error::Internal)
    }
}

fn sleep_test() -> Result<()> {
    test_logger::step_start!("Testing sleep_until");
    let start = userspace::time::SystemClock::now();
    let delay = Duration::from_millis(100);
    let deadline = start + delay;

    if let Err(err) = sleep_until(deadline) {
        test_logger::step_failed!("sleep_until failed");
        return Err(err);
    }

    let end = userspace::time::SystemClock::now();
    if end >= deadline {
        test_logger::step_passed!("sleep_until slept for at least the requested time");
        Ok(())
    } else {
        test_logger::step_failed!("sleep_until returned before deadline");
        Err(pw_status::Error::Internal)
    }
}

fn do_test() -> Result<()> {
    test_logger::start("User Clock Test");
    clock_test()?;
    sleep_test()?;
    test_logger::passed("User Clock Test");
    Ok(())
}

#[entry]
fn main_entry() -> Result<()> {
    let ret = do_test().inspect_err(|_| {
        test_logger::failed("User Clock Test");
    });

    let _ = syscall::debug_shutdown(ret);
    ret
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    test_logger::step_failed!("PANIC");
    let _ = syscall::debug_shutdown(Err(pw_status::Error::Internal));
    loop {}
}
