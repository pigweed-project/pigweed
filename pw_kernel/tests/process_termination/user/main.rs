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
use main_codegen::handle;
use pw_status::{Result, StatusCode};
use userspace::time::{Clock, Duration, Instant, SystemClock, sleep_until};
use userspace::{process_entry, syscall};

const PROCESS_JOIN_TIMEOUT: Duration = Duration::from_secs(5);
const TEST_THREAD_SPAWN_DELAY: Duration = Duration::from_millis(200);

#[unsafe(no_mangle)]
pub extern "C" fn test_thread_entry_initiator(_arg: usize) {
    let mut reply = [0u8; 1];
    test_logger::step_info!("Test thread: Initiating blocked transaction via Command 3");
    let res = syscall::channel_transact(
        handle::IPC_CONTROL_INITIATOR,
        &[Command::BlockInitiator as u8],
        &mut reply,
        Instant::MAX,
    );
    if res == Err(pw_status::Error::Unavailable) {
        test_logger::step_passed!("Initiator thread correctly received Unavailable!");
        syscall::thread_exit(0);
    } else {
        test_logger::step_failed!(
            "Initiator thread expected Unavailable, got {}",
            res.status_code() as u32
        );
        let _ = syscall::debug_shutdown(Err(pw_status::Error::Internal));
        loop {}
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn test_thread_entry_handler(_arg: usize) {
    test_logger::step_info!("Test thread: Waiting for transaction on IPC_RESET handler");
    let wait_res =
        syscall::object_wait(handle::IPC_RESET, syscall::Signals::READABLE, Instant::MAX);
    if wait_res == Err(pw_status::Error::Cancelled) {
        test_logger::step_passed!("Handler thread correctly received Cancelled!");
        syscall::thread_exit(0);
    } else {
        test_logger::step_failed!("Handler thread expected Cancelled");
        let _ = syscall::debug_shutdown(Err(pw_status::Error::Internal));
        loop {}
    }
}

const TEST_THREAD_STACK_SIZE: usize = 1024;

fn do_test() -> Result<()> {
    test_logger::start("User Process Termination");

    let mut test_thread_stack = [0u8; TEST_THREAD_STACK_SIZE];
    let mut reply = [0u8; 1];

    // Sleep to allow threads to start and log before the tests start.
    // Once threads can be manually started, this will become unnecessary.
    if let Err(err) = sleep_until(SystemClock::now() + TEST_THREAD_SPAWN_DELAY) {
        test_logger::step_failed!("Failed to sleep for 200ms");
        return Err(err);
    }

    test_invalid_handles()?;
    test_clean_and_forced_exit()?;
    test_object_reset_basic(&mut reply)?;
    test_own_user_signal_preserved_on_terminate(&mut reply)?;
    test_peer_user_signal_cleared_on_terminate(&mut reply)?;
    test_initiator_unavailable_on_terminate(&mut test_thread_stack)?;
    test_handler_error_on_terminate(&mut test_thread_stack)?;
    test_wait_group_error_on_initiator_terminate(&mut test_thread_stack, &mut reply)?;
    test_wait_group_error_on_handler_terminate(&mut test_thread_stack)?;

    test_logger::passed("User Process Termination");

    Ok(())
}

fn start_test_thread(entry: extern "C" fn(usize), stack: &mut [u8]) -> Result<()> {
    let initial_pc = entry as *const () as usize;
    let initial_sp = unsafe { stack.as_mut_ptr().add(stack.len()) as usize };
    syscall::thread_start(handle::TEST_THREAD, initial_pc, initial_sp)
}

fn cleanup_test_thread() -> Result<()> {
    let _ = syscall::task_terminate(handle::TEST_THREAD);
    syscall::object_wait(
        handle::TEST_THREAD,
        syscall::Signals::JOINABLE,
        Instant::MAX,
    )?;
    syscall::task_join(handle::TEST_THREAD)?;
    Ok(())
}

fn wait_and_join_task(handle: u32) -> Result<syscall::ExitStatus> {
    syscall::object_wait(handle, syscall::Signals::JOINABLE, Instant::MAX)?;
    syscall::task_join(handle)
}

fn send_control_command(cmd: u8, reply: &mut [u8]) -> Result<()> {
    syscall::channel_transact(handle::IPC_CONTROL_INITIATOR, &[cmd], reply, Instant::MAX)?;
    Ok(())
}

/// Tests that syscalls correctly reject invalid handles or handles of the wrong type.
fn test_invalid_handles() -> Result<()> {
    // We cannot test successful process_terminate on our own process because
    // it kills the main thread, resulting in a kernel panic (run queue empty),
    // and thus a failed test. Furthermore, userspace apps cannot spawn new processes
    // or hold handles to other processes.
    // So we test that process_terminate correctly rejects invalid handles.

    test_logger::step_start!("Testing task_terminate on an invalid handle");
    let result = syscall::task_terminate(0xdeadbeef);
    if result != Err(pw_status::Error::OutOfRange) {
        test_logger::step_failed!("Expected OutOfRange on an invalid handle");
        return Err(pw_status::Error::Internal);
    }

    test_logger::step_passed!("Received Error::OutOfRange response");

    Ok(())
}

/// Tests normal process termination (clean exit) and forced termination (kill).
fn test_clean_and_forced_exit() -> Result<()> {
    for pass in 0..2 {
        test_logger::step_start!("Testing clean exit iteration {}", pass as u32);
        test_logger::step_info!("Waiting for clean exit process to become joinable");
        if let Err(err) = syscall::object_wait(
            handle::CLEAN_EXIT_PROCESS,
            syscall::Signals::JOINABLE,
            SystemClock::now() + PROCESS_JOIN_TIMEOUT,
        ) {
            test_logger::step_failed!("Failed to wait for extra process to become joinable");
            return Err(err);
        }

        test_logger::step_info!("Joining clean exit process");
        match syscall::task_join(handle::CLEAN_EXIT_PROCESS) {
            Err(err) => return Err(err),
            Ok(syscall::ExitStatus::Success(42)) => (),
            Ok(_) => {
                test_logger::step_failed!("Clean exit process joined with unexpected status");
                return Err(pw_status::Error::Internal);
            }
        }
        test_logger::step_passed!("Clean exit process joined");

        test_logger::step_start!("Testing forced exit iteration {}", pass as u32);
        // The forced_exit process is started automatically by the kernel on boot.
        // On subsequent iterations, we start it manually below.
        test_logger::step_info!("Terminating forced exit process");
        syscall::task_terminate(handle::FORCED_EXIT_PROCESS)?;

        test_logger::step_info!("Waiting for forced exit process to become joinable");
        if let Err(err) = syscall::object_wait(
            handle::FORCED_EXIT_PROCESS,
            syscall::Signals::JOINABLE,
            SystemClock::now() + PROCESS_JOIN_TIMEOUT,
        ) {
            test_logger::step_failed!("Error waiting for process to be joinable");
            return Err(err);
        }

        test_logger::step_info!("Joining forced exit process");
        let status = syscall::task_join(handle::FORCED_EXIT_PROCESS)?;
        if status != syscall::ExitStatus::TerminatedBySyscall {
            test_logger::step_failed!(
                "Process joined with unexpected status (expected TerminatedBySyscall)"
            );
            return Err(pw_status::Error::Internal);
        }
        test_logger::step_passed!("Forced exit process joined");

        test_logger::step_start!("Testing exception exit iteration {}", pass as u32);
        test_logger::step_info!("Waiting for exception exit process to become joinable");
        if let Err(err) = syscall::object_wait(
            handle::EXCEPTION_EXIT_PROCESS,
            syscall::Signals::JOINABLE,
            SystemClock::now() + PROCESS_JOIN_TIMEOUT,
        ) {
            test_logger::step_failed!("Error waiting for exception process to be joinable");
            return Err(err);
        }

        test_logger::step_info!("Joining exception exit process");
        let status = syscall::task_join(handle::EXCEPTION_EXIT_PROCESS)?;
        if status != syscall::ExitStatus::UnhandledException(0) {
            test_logger::step_failed!(
                "Process joined with unexpected status (expected UnhandledException(0))"
            );
            return Err(pw_status::Error::Internal);
        }
        test_logger::step_passed!("Exception exit process joined");

        test_logger::info!("Restarting test processes iteration {}", pass as u32);
        syscall::process_start(handle::CLEAN_EXIT_PROCESS)?;
        syscall::process_start(handle::FORCED_EXIT_PROCESS)?;
        syscall::process_start(handle::EXCEPTION_EXIT_PROCESS)?;

        // Give the other processes a chance to start for the next iteration.
        //
        // TODO: https://pwbug.dev/505490714 - Query process state to ensure it's running
        if let Err(err) = sleep_until(SystemClock::now() + TEST_THREAD_SPAWN_DELAY) {
            test_logger::step_failed!("Failed to sleep for 200ms");
            return Err(err);
        }
    }
    Ok(())
}

/// Tests that objects are correctly reset when a process terminates.
fn test_object_reset_basic(reply: &mut [u8]) -> Result<()> {
    test_logger::step_start!("Testing Object Reset");

    test_logger::step_info!("Sending 'Modify State' command to restart process");
    syscall::channel_transact(
        handle::IPC_CONTROL_INITIATOR,
        &[Command::ModifyState as u8],
        reply,
        Instant::MAX,
    )?;

    test_logger::step_info!("Terminating restart_process");
    syscall::task_terminate(handle::RESTART_PROCESS)?;

    test_logger::step_info!("Waiting for restart_process to become joinable and joining");
    wait_and_join_task(handle::RESTART_PROCESS)?;

    test_logger::step_info!("Restarting restart_process");
    syscall::process_start(handle::RESTART_PROCESS)?;

    test_logger::step_info!("Sending 'Verify Reset' command to restart_process");
    syscall::channel_transact(
        handle::IPC_CONTROL_INITIATOR,
        &[Command::VerifyReset as u8],
        reply,
        Instant::MAX,
    )?;
    Ok(())
}

/// Tests that an object's own USER signal is preserved on process termination and
/// restart when it's peer remains alive.
fn test_own_user_signal_preserved_on_terminate(reply: &mut [u8]) -> Result<()> {
    test_logger::step_start!("Receiver termination preserves USER signal upon restart");

    test_logger::step_info!("Setting peer USER signal to true on RESTART_PROCESS handler");
    syscall::object_set_peer_user_signal(handle::IPC_CONTROL_INITIATOR, true)?;

    test_logger::step_info!("Terminating RESTART_PROCESS");
    syscall::task_terminate(handle::RESTART_PROCESS)?;

    test_logger::step_info!("Waiting for RESTART_PROCESS to become joinable and joining");
    wait_and_join_task(handle::RESTART_PROCESS)?;

    // Restart RESTART_PROCESS
    test_logger::step_info!("Restarting RESTART_PROCESS");
    syscall::process_start(handle::RESTART_PROCESS)?;

    // Request RESTART_PROCESS to verify USER signal remained active on restart
    test_logger::step_info!("Requesting RESTART_PROCESS to verify USER signal is preserved");
    syscall::channel_transact(
        handle::IPC_CONTROL_INITIATOR,
        &[Command::VerifyUserSignalPreserved as u8],
        reply,
        Instant::MAX,
    )?;

    // Reset our USER signal to false for subsequent tests
    syscall::object_set_peer_user_signal(handle::IPC_CONTROL_INITIATOR, false)?;
    Ok(())
}

/// Tests that an object's peer's USER signal is cleared on process termination and
/// restart.
fn test_peer_user_signal_cleared_on_terminate(reply: &mut [u8]) -> Result<()> {
    test_logger::step_start!("Process termination resets peer's USER signals to false");

    test_logger::step_info!("Requesting RESTART_PROCESS to set USER signal on IPC_RESET handler");
    syscall::channel_transact(
        handle::IPC_CONTROL_INITIATOR,
        &[Command::SetResetUserSignal as u8],
        reply,
        Instant::MAX,
    )?;

    // Verify USER signal is active on IPC_RESET handler before termination
    let wait_res = syscall::object_wait(
        handle::IPC_RESET,
        syscall::Signals::USER,
        SystemClock::now(),
    );
    if wait_res.is_err() {
        test_logger::step_failed!("Expected Signals::USER to be set on IPC_RESET handler");
        return Err(pw_status::Error::Internal);
    }

    test_logger::step_info!("Terminating RESTART_PROCESS");
    syscall::task_terminate(handle::RESTART_PROCESS)?;

    test_logger::step_info!("Waiting for RESTART_PROCESS to become joinable and joining");
    wait_and_join_task(handle::RESTART_PROCESS)?;

    // Verify USER signal is reset to false on IPC_RESET handler after termination!
    test_logger::step_info!("Checking if USER signal is de-asserted on IPC_RESET handler");
    let wait_res = syscall::object_wait(
        handle::IPC_RESET,
        syscall::Signals::USER,
        SystemClock::now(),
    );
    // If it was reset to false, waiting for USER signal should return Err(DeadlineExceeded) (4) immediately!
    if wait_res != Err(pw_status::Error::DeadlineExceeded) {
        test_logger::step_failed!("Signals::USER was NOT de-asserted on process termination!");
        return Err(pw_status::Error::Internal);
    }
    test_logger::step_passed!("Peer's USER signal correctly de-asserted after termination!");

    // Restart RESTART_PROCESS
    syscall::process_start(handle::RESTART_PROCESS)?;
    Ok(())
}

/// Tests that an initiator receives `Unavailable` when the handler terminates.
fn test_initiator_unavailable_on_terminate(stack: &mut [u8]) -> Result<()> {
    test_logger::step_start!("Initiator receives Unavailable when handler terminates");
    // test_thread_entry_initiator calls channel_transact and asserts that Unavailable is returned
    start_test_thread(test_thread_entry_initiator, stack)?;

    // Give the test thread a chance to print its log
    sleep_until(SystemClock::now() + TEST_THREAD_SPAWN_DELAY)?;

    test_logger::step_info!("Terminating restart_process");
    syscall::task_terminate(handle::RESTART_PROCESS)?;

    test_logger::step_info!("Waiting for restart_process to become joinable and joining");
    wait_and_join_task(handle::RESTART_PROCESS)?;

    // Give the test thread a chance to print its log
    sleep_until(SystemClock::now() + TEST_THREAD_SPAWN_DELAY)?;

    cleanup_test_thread()?;
    Ok(())
}

/// Tests that a handler receives `ERROR` signal when the initiator terminates.
fn test_handler_error_on_terminate(_stack: &mut [u8]) -> Result<()> {
    test_logger::step_start!("Handler receives ERROR signal when initiator terminates");
    syscall::process_start(handle::RESTART_PROCESS)?;

    // Tell OBJECT_RESET to initiate async transaction
    let mut reply = [0u8; 1];
    test_logger::step_info!("main thread: Sending command 4 to initiate async transaction");
    send_control_command(Command::AsyncTransact as u8, &mut reply)?;

    // Wait for the async transaction to arrive
    syscall::object_wait(handle::IPC_RESET, syscall::Signals::READABLE, Instant::MAX)?;

    // Wait for the restart process to exit (it will do so after 500ms via command 4)
    wait_and_join_task(handle::RESTART_PROCESS)?;

    // Now check if the handler object received the ERROR signal
    let wait_res = syscall::object_wait(
        handle::IPC_RESET,
        syscall::Signals::ERROR,
        SystemClock::now() + PROCESS_JOIN_TIMEOUT,
    );
    if wait_res.is_ok() {
        test_logger::step_passed!("Handler thread correctly received ERROR signal after join!");
    } else {
        test_logger::step_failed!("Handler thread expected ERROR signal");
        return Err(pw_status::Error::Internal);
    }
    Ok(())
}

/// Tests that the Wait Group receives `ERROR` when the initiator terminates.
fn test_wait_group_error_on_initiator_terminate(_stack: &mut [u8], reply: &mut [u8]) -> Result<()> {
    test_logger::step_start!("Wait Group receives ERROR when initiator terminates");
    syscall::process_start(handle::RESTART_PROCESS)?;

    // Tell OBJECT_RESET to initiate async transaction
    test_logger::step_info!("main thread: Sending command 4 to initiate async transaction");
    send_control_command(Command::AsyncTransact as u8, reply)?;

    // Add IPC_RESET to WAIT_GROUP waiting for ERROR
    syscall::wait_group_add(
        handle::WAIT_GROUP,
        handle::IPC_RESET,
        syscall::Signals::ERROR,
        99,
    )?;

    // Wait for the restart process to exit
    wait_and_join_task(handle::RESTART_PROCESS)?;

    // Now wait on the WAIT_GROUP (it should be ready immediately since cleanup happened)
    let wait_return = syscall::object_wait(
        handle::WAIT_GROUP,
        syscall::Signals::READABLE,
        SystemClock::now() + PROCESS_JOIN_TIMEOUT,
    )?;

    if wait_return.user_data == 99
        && wait_return
            .pending_signals
            .contains(syscall::Signals::ERROR)
    {
        test_logger::step_passed!("Wait Group correctly woke up on member ERROR signal!");
    } else {
        test_logger::step_failed!(
            "Wait Group woke up but unexpected result: user_data={}, signals={:#x}",
            wait_return.user_data as u32,
            wait_return.pending_signals.bits() as u32
        );
        return Err(pw_status::Error::Internal);
    }

    // Clean up: remove from wait group
    syscall::wait_group_remove(handle::WAIT_GROUP, handle::IPC_RESET)?;

    Ok(())
}

/// Tests that the Wait Group receives `ERROR` when the handler terminates.
fn test_wait_group_error_on_handler_terminate(_stack: &mut [u8]) -> Result<()> {
    test_logger::step_start!("Wait Group receives ERROR when handler terminates");
    syscall::process_start(handle::RESTART_PROCESS)?;

    // Initiate async transaction on IPC_CONTROL
    let msg = [Command::SleepAndExit as u8];
    let mut reply_buf = [0u8; 1];
    test_logger::step_info!("main thread: Initiating async transaction on IPC_CONTROL");
    // NOTE: Async channel syscalls are not meant to be used directly by normal
    // programs without wrappers handling buffer lifetimes.
    unsafe {
        syscall::channel_async_transact(
            handle::IPC_CONTROL_INITIATOR,
            msg.as_ptr(),
            msg.len(),
            reply_buf.as_mut_ptr(),
            reply_buf.len(),
        )?;
    }

    // Add IPC_CONTROL to WAIT_GROUP waiting for ERROR
    syscall::wait_group_add(
        handle::WAIT_GROUP,
        handle::IPC_CONTROL_INITIATOR,
        syscall::Signals::ERROR,
        99,
    )?;

    // Wait for the restart process to exit
    wait_and_join_task(handle::RESTART_PROCESS)?;

    // Now wait on the WAIT_GROUP
    let wait_return = syscall::object_wait(
        handle::WAIT_GROUP,
        syscall::Signals::READABLE,
        SystemClock::now() + PROCESS_JOIN_TIMEOUT,
    )?;

    if (wait_return.user_data == 89 || wait_return.user_data == 99)
        && wait_return
            .pending_signals
            .contains(syscall::Signals::ERROR)
    {
        test_logger::step_passed!(
            "Wait Group correctly woke up on member ERROR and READABLE (Initiator case)!"
        );
    } else {
        test_logger::step_failed!(
            "Wait Group woke up but unexpected result: user_data={}, signals={:#x}",
            wait_return.user_data as u32,
            wait_return.pending_signals.bits() as u32
        );
        return Err(pw_status::Error::Internal);
    }

    // Clean up: remove from wait group
    syscall::wait_group_remove(handle::WAIT_GROUP, handle::IPC_CONTROL_INITIATOR)?;

    // Cancel the async transaction to clean up channel state
    if let Err(e) = syscall::channel_async_cancel(handle::IPC_CONTROL_INITIATOR) {
        if e != pw_status::Error::Unavailable {
            return Err(e);
        }
    }

    Ok(())
}

#[process_entry("main")]
fn main() -> Result<()> {
    let ret = do_test().inspect_err(|e| {
        test_logger::failed("User Process Termination");
        test_logger::step_failed!("status code: {}", *e as u32);
    });

    let _ = syscall::debug_shutdown(ret);
    ret
}
