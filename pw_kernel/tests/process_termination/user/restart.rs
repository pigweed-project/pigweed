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

use control_commands::Command;
use pw_status::Error;
use restart_codegen::handle;
use userspace::syscall::Signals;
use userspace::time::{Clock, Duration, Instant, SystemClock, sleep_until};
use userspace::{process_entry, syscall};

const EXIT_DELAY: Duration = Duration::from_millis(500);

fn handle_modify_state() {
    test_logger::step_info!("ModifyState received: Adding IPC handles to WAIT_GROUP");
    if let Err(e) = syscall::wait_group_add(
        handle::WAIT_GROUP,
        handle::IPC_CONTROL_HANDLER,
        Signals::READABLE,
        42,
    ) {
        test_logger::step_failed!("Failed to add to wait group");
        let _ = syscall::debug_shutdown(Err(e));
        loop {}
    }
    if let Err(e) =
        syscall::wait_group_add(handle::WAIT_GROUP, handle::IPC_RESET, Signals::READABLE, 43)
    {
        test_logger::step_failed!("Failed to add initiator to wait group");
        let _ = syscall::debug_shutdown(Err(e));
        loop {}
    }
    let _ = syscall::channel_respond(handle::IPC_CONTROL_HANDLER, &[0]);
}

fn handle_verify_reset() {
    test_logger::step_info!("VerifyReset received: Verifying reset");
    match syscall::wait_group_remove(handle::WAIT_GROUP, handle::IPC_CONTROL_HANDLER) {
        Err(Error::NotFound) => {}
        Ok(()) => {
            test_logger::step_failed!("Object was NOT removed from wait group on reset!");
            let _ = syscall::debug_shutdown(Err(Error::Internal));
            loop {}
        }
        Err(e) => {
            test_logger::step_failed!("Failed to remove object from wait group");
            let _ = syscall::debug_shutdown(Err(e));
            loop {}
        }
    }
    match syscall::wait_group_remove(handle::WAIT_GROUP, handle::IPC_RESET) {
        Err(Error::NotFound) => {}
        Ok(()) => {
            test_logger::step_failed!("Initiator was NOT removed from wait group on reset!");
            let _ = syscall::debug_shutdown(Err(Error::Internal));
            loop {}
        }
        Err(e) => {
            test_logger::step_failed!("Failed to remove initiator from wait group");
            let _ = syscall::debug_shutdown(Err(e));
            loop {}
        }
    }
    let _ = syscall::channel_respond(handle::IPC_CONTROL_HANDLER, &[0]);
    test_logger::step_passed!("Reset verified");
}

fn handle_block_initiator() -> ! {
    test_logger::step_info!("BlockInitiator received: Doing nothing to block initiator");
    loop {
        if sleep_until(Instant::MAX).is_err() {
            panic!("canceled");
        }
    }
}

fn handle_async_transact() -> ! {
    test_logger::step_info!("AsyncTransact received: Initiating async transaction on IPC_RESET");
    let msg = [42u8];
    let mut reply = [0u8; 1];
    unsafe {
        let _ = syscall::channel_async_transact(
            handle::IPC_RESET,
            msg.as_ptr(),
            msg.len(),
            reply.as_mut_ptr(),
            reply.len(),
        );
    }
    let _ = syscall::channel_respond(handle::IPC_CONTROL_HANDLER, &[0]);
    let _ = sleep_until(SystemClock::now() + EXIT_DELAY);
    syscall::process_exit(0);
}

fn handle_sleep_and_exit() -> ! {
    test_logger::step_info!("SleepAndExit received: Sleeping and then exiting");
    let _ = sleep_until(SystemClock::now() + EXIT_DELAY);
    syscall::process_exit(0);
}

fn handle_set_control_user_signal() {
    test_logger::step_info!("SetControlUserSignal received: Setting peer USER signal to true");
    if let Err(e) = syscall::object_set_peer_user_signal(handle::IPC_CONTROL_HANDLER, true) {
        test_logger::step_failed!("Failed to set peer user signal on IPC_CONTROL_HANDLER");
        let _ = syscall::debug_shutdown(Err(e));
        loop {}
    }
    let _ = syscall::channel_respond(handle::IPC_CONTROL_HANDLER, &[0]);
}

fn handle_set_reset_user_signal() {
    test_logger::step_info!(
        "SetResetUserSignal received: Setting peer USER signal to true on IPC_RESET"
    );
    if let Err(e) = syscall::object_set_peer_user_signal(handle::IPC_RESET, true) {
        test_logger::step_failed!("Failed to set peer user signal on IPC_RESET");
        let _ = syscall::debug_shutdown(Err(e));
        loop {}
    }
    let _ = syscall::channel_respond(handle::IPC_CONTROL_HANDLER, &[0]);
}

fn handle_verify_user_signal_preserved() {
    test_logger::step_info!(
        "VerifyUserSignalPreserved received: Verifying Signals::USER is still set"
    );
    let wait_res = syscall::object_wait(
        handle::IPC_CONTROL_HANDLER,
        Signals::USER,
        SystemClock::now(),
    );
    if wait_res.is_err() {
        test_logger::step_failed!("Signals::USER was lost on restart!");
        let _ = syscall::debug_shutdown(Err(Error::Internal));
        loop {}
    }
    test_logger::step_passed!("Signals::USER correctly remained set on restart!");
    let _ = syscall::channel_respond(handle::IPC_CONTROL_HANDLER, &[0]);
}

#[process_entry("restart")]
fn main() {
    test_logger::step_info!("I am the object_reset process");

    loop {
        // Wait for a command from `main`
        if let Err(e) =
            syscall::object_wait(handle::IPC_CONTROL_HANDLER, Signals::READABLE, Instant::MAX)
        {
            test_logger::step_failed!("Failed to wait for command");
            let _ = syscall::debug_shutdown(Err(e));
            loop {}
        }

        // Read the command
        let mut cmd = [0u8; 1];
        if let Err(e) = syscall::channel_read_exact(handle::IPC_CONTROL_HANDLER, 0, &mut cmd) {
            test_logger::step_failed!("Failed to read command");
            let _ = syscall::debug_shutdown(Err(e));
            loop {}
        }

        let command = match Command::try_from(cmd[0]) {
            Ok(c) => c,
            Err(_) => {
                test_logger::step_failed!("Unknown command received");
                let _ = syscall::debug_shutdown(Err(Error::InvalidArgument));
                loop {}
            }
        };

        match command {
            Command::ModifyState => handle_modify_state(),
            Command::VerifyReset => handle_verify_reset(),
            Command::BlockInitiator => handle_block_initiator(),
            Command::AsyncTransact => handle_async_transact(),
            Command::SleepAndExit => handle_sleep_and_exit(),
            Command::SetControlUserSignal => handle_set_control_user_signal(),
            Command::SetResetUserSignal => handle_set_reset_user_signal(),
            Command::VerifyUserSignalPreserved => handle_verify_user_signal_preserved(),
        }
    }
}
