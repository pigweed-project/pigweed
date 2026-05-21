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

use main_codegen::handle;
use pw_status::Result;
use userspace::time::{Clock, Duration, SystemClock};
use userspace::{entry, syscall};

const PROCESS_JOIN_TIMEOUT: Duration = Duration::from_secs(5);

fn do_test() -> Result<()> {
    test_logger::start("User Process Termination Stress");

    let mut pass = 0;

    loop {
        test_logger::step_info!("Pass {}", pass as u32);

        if let Err(err) = syscall::task_terminate(handle::FORCED_EXIT_PROCESS) {
            test_logger::step_failed!("Failed to terminate extra process");
            return Err(err);
        }

        test_logger::step_info!("Waiting {}", pass as u32);
        let deadline = SystemClock::now() + PROCESS_JOIN_TIMEOUT;
        if let Err(err) = syscall::object_wait(
            handle::FORCED_EXIT_PROCESS,
            syscall::Signals::JOINABLE,
            deadline,
        ) {
            test_logger::step_failed!("Failed to wait for extra process to become joinable");
            return Err(err);
        }

        test_logger::step_info!("Joining {}", pass as u32);
        match syscall::task_join(handle::FORCED_EXIT_PROCESS) {
            Err(err) => return Err(err),
            Ok(syscall::ExitStatus::TerminatedBySyscall) => (),
            Ok(_) => {
                test_logger::step_failed!("Process joined with unexpected status");
                return Err(pw_status::Error::Internal);
            }
        }

        test_logger::step_info!("Starting {}", pass as u32);
        if let Err(err) = syscall::process_start(handle::FORCED_EXIT_PROCESS) {
            test_logger::step_failed!("Failed to restart extra process");
            return Err(err);
        }

        pass += 1;
    }
}

#[entry]
fn main_entry() -> Result<()> {
    let ret = do_test().inspect_err(|e| {
        test_logger::failed("User Process Termination Stress");
        test_logger::step_failed!("status code: {}", *e as u32);
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
