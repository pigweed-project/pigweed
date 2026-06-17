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

#![no_main]
#![no_std]

use pw_status::{Error, Result};
use supervisor_codegen::handle;
use userspace::entry;
use userspace::syscall::{self, ExitStatus, Signals};
use userspace::time::{self, Clock, Duration, SystemClock};

const SHUTDOWN_DRAIN_TIMEOUT: Duration = Duration::from_millis(100);
const SUPERVISOR_TIMEOUT: Duration = Duration::from_secs(15);

#[entry]
fn main() -> Result<()> {
    pw_log::info!("Supervisor started");

    let result = run_supervisor().inspect_err(|&err| {
        pw_log::error!("Supervisor failed with error: {}", err as u32);
    });

    // Allow any IO buffers (console/UART) to be drained before shutting down.
    let _ = time::sleep_until(SystemClock::now() + SHUTDOWN_DRAIN_TIMEOUT);

    syscall::debug_shutdown(result)
}

// DOCSTAG: [tour]
/// Encapsulates the supervisor setup and event loop execution,
/// returning Ok(()) on successful game exit or a pw_status::Error on failures.
fn run_supervisor() -> Result<()> {
    // Add both child processes to the WaitGroup, passing their handles as user_data
    syscall::wait_group_add(
        handle::MAIN_LOOP_WAIT_GROUP,
        handle::UART_DRIVER_PROCESS,
        Signals::JOINABLE,
        handle::UART_DRIVER_PROCESS as usize,
    )?;
    syscall::wait_group_add(
        handle::MAIN_LOOP_WAIT_GROUP,
        handle::ADVENTURE_GAME_PROCESS,
        Signals::JOINABLE,
        handle::ADVENTURE_GAME_PROCESS as usize,
    )?;

    loop {
        // Wait for a process to become joinable, retrying on timeout.  This
        // timeout could be used to pet a watchdog or the like.
        let deadline = SystemClock::now() + SUPERVISOR_TIMEOUT;
        let wait_result =
            match syscall::object_wait(handle::MAIN_LOOP_WAIT_GROUP, Signals::READABLE, deadline) {
                Err(Error::DeadlineExceeded) => continue,
                res => res?,
            };
        let triggered_handle = wait_result.user_data as u32;

        // Join the process and get it's `ExitStatus`.
        let status = syscall::task_join(triggered_handle)?;

        log_exit_status(triggered_handle, status);

        // If the game process completed successfully, trigger clean system shutdown.
        if triggered_handle == handle::ADVENTURE_GAME_PROCESS
            && matches!(status, ExitStatus::Success(_))
        {
            pw_log::info!("Supervisor shutting down system due to successful game exit.");
            return Ok(());
        }

        // Always restart any exited process
        pw_log::warn!(
            "Process '{}' exited, restarting...",
            process_name(triggered_handle) as &str
        );

        syscall::process_start(triggered_handle).inspect_err(|err| {
            pw_log::error!(
                "Failed to restart process '{}': {}",
                process_name(triggered_handle) as &str,
                *err as u32
            );
        })?;
    }
}
// DOCSTAG: [tour]

/// Helper function to log the exact exit status/reason for a terminated process.
fn log_exit_status(process_handle: u32, status: ExitStatus) {
    match status {
        ExitStatus::Success(code) => {
            pw_log::info!(
                "Process '{}' exited cleanly with code {}",
                process_name(process_handle) as &str,
                code as u32
            );
        }
        ExitStatus::UnhandledException(exception) => {
            pw_log::error!(
                "Process '{}' crashed due to unhandled exception {}",
                process_name(process_handle) as &str,
                exception as usize
            );
        }
        ExitStatus::TerminatedBySyscall => {
            pw_log::error!(
                "Process '{}' was terminated by syscall",
                process_name(process_handle) as &str
            );
        }
        ExitStatus::TerminatedByKernel => {
            pw_log::error!(
                "Process '{}' was terminated by kernel",
                process_name(process_handle) as &str
            );
        }
        ExitStatus::ProcessTerminated => {
            // ExitStatus::ProcessTerminated is only returned when joining a thread.
            pw_log::error!(
                "Process '{}' terminated with unexpected ExitStatus::ProcessTerminated",
                process_name(process_handle) as &str
            );
        }
        _ => {
            pw_log::error!(
                "Process '{}' exited with unknown/non-exhaustive status",
                process_name(process_handle) as &str
            );
        }
    }
}

/// Helper function to resolve a dynamic process handle to its human-readable name.
fn process_name(process_handle: u32) -> &'static str {
    match process_handle {
        handle::UART_DRIVER_PROCESS => "uart_driver",
        handle::ADVENTURE_GAME_PROCESS => "adventure_game",
        _ => "unknown",
    }
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    pw_log::error!("PANIC: Supervisor panicked!");
    syscall::process_exit(Error::Internal as u32);
}
