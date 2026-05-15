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
use pw_log::info;
use pw_status::Result;
use userspace::time::{Clock, Duration, SystemClock};
use userspace::{entry, syscall};

const PROCESS_JOIN_TIMEOUT: Duration = Duration::from_secs(5);

fn do_test() -> Result<()> {
    info!("🔄 [User Process Termination Stress] RUNNING");

    let mut pass = 0;

    loop {
        info!("🔄 ├─ Pass {}", pass as u32);

        if let Err(err) = syscall::task_terminate(handle::FORCED_EXIT_PROCESS) {
            pw_log::error!("Failed to terminate extra process");
            return Err(err);
        }

        info!("🔄 ├─ Waiting", pass as u32);
        let deadline = SystemClock::now() + PROCESS_JOIN_TIMEOUT;
        if let Err(err) = syscall::object_wait(
            handle::FORCED_EXIT_PROCESS,
            syscall::Signals::JOINABLE,
            deadline,
        ) {
            pw_log::error!("Failed to wait for extra process to become joinable");
            return Err(err);
        }

        info!("🔄 ├─ Joining", pass as u32);
        match syscall::task_join(handle::FORCED_EXIT_PROCESS) {
            Err(err) => return Err(err),
            Ok(syscall::ExitStatus::TerminatedBySyscall) => (),
            Ok(_) => {
                pw_log::error!("❌ ├─ Process joined with unexpected status");
                return Err(pw_status::Error::Internal);
            }
        }

        info!("🔄 ├─ Starting", pass as u32);
        if let Err(err) = syscall::process_start(handle::FORCED_EXIT_PROCESS) {
            pw_log::error!("Failed to restart extra process");
            return Err(err);
        }

        pass += 1;
    }
}

#[entry]
fn main_entry() -> Result<()> {
    let ret = do_test().inspect_err(|e| {
        pw_log::error!("❌ FAILED");
        pw_log::error!("❌ status code: {}", *e as u32);
    });

    let _ = syscall::debug_shutdown(ret);
    ret
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    pw_log::error!("❌ PANIC");
    let _ = syscall::debug_shutdown(Err(pw_status::Error::Internal));
    loop {}
}
