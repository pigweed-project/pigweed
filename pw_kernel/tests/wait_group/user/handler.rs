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
#![no_main]
#![no_std]

use core::sync::atomic::{AtomicBool, Ordering};

use handler_codegen::handle;
use pw_status::{Error, Result, StatusCode};
use userspace::syscall::{ExitStatus, Signals};
use userspace::time::{Clock, Duration, Instant, SystemClock, sleep_until};
use userspace::{entry, syscall};

const USER_DATA_IPC_A: usize = 11;
const USER_DATA_IPC_B: usize = 22;
const USER_DATA_UNUSED: usize = 3;
const USER_DATA_BLOCKED_WAITER: usize = 33;
const USER_DATA_SIGNALED_ADD: usize = 44;

fn wait_group() -> Result<()> {
    test_logger::info!("WaitGroup service starting");

    syscall::wait_group_add(
        handle::WAIT_GROUP_1,
        handle::IPC_A,
        Signals::READABLE,
        USER_DATA_IPC_A,
    )?;
    syscall::wait_group_add(
        handle::WAIT_GROUP_1,
        handle::IPC_B,
        Signals::READABLE,
        USER_DATA_IPC_B,
    )?;

    for i in 0..3 {
        test_logger::info!("waiting for objects iteration {}", i as usize);
        let wait_return =
            syscall::object_wait(handle::WAIT_GROUP_1, Signals::READABLE, Instant::MAX)?;

        let (ipc_handle, expected_char) = match i {
            0 => {
                if wait_return.user_data != USER_DATA_IPC_A
                    || !wait_return.pending_signals.contains(Signals::READABLE)
                {
                    return Err(Error::Internal);
                }
                (handle::IPC_A, 'a')
            }
            1 => {
                if wait_return.user_data != USER_DATA_IPC_B
                    || !wait_return.pending_signals.contains(Signals::READABLE)
                {
                    return Err(Error::Internal);
                }
                (handle::IPC_B, 'b')
            }
            2 => {
                if wait_return.user_data != USER_DATA_IPC_A
                    || !wait_return.pending_signals.contains(Signals::READABLE)
                {
                    return Err(Error::Internal);
                }
                (handle::IPC_A, 'c')
            }
            _ => {
                test_logger::error!("Unknown iteration: {}", i as usize);
                return Err(Error::Internal);
            }
        };
        test_logger::info!("done waiting on objects iteration {}", i as usize);

        // Read the payload.
        let mut buffer = [0u8; size_of::<char>()];
        let len = syscall::channel_read(ipc_handle, 0, &mut buffer)?;

        if len != buffer.len() {
            return Err(Error::OutOfRange);
        };

        // Convert the payload to a character.
        let Some(c) = char::from_u32(u32::from_ne_bytes(buffer)) else {
            return Err(Error::InvalidArgument);
        };

        if c != expected_char {
            test_logger::error!(
                "Received {} character, {} expected",
                c as char,
                expected_char as char
            );
            return Err(Error::InvalidArgument);
        }

        // Respond to the IPC with the iteration number.
        let mut response_buffer = [0u8; size_of::<usize>()];
        response_buffer.copy_from_slice(&(i as usize).to_ne_bytes());
        syscall::channel_respond(ipc_handle, &response_buffer)?;
    }

    test_logger::info!("Objects can only be in one wait group");
    let add_res = syscall::wait_group_add(
        handle::WAIT_GROUP_2,
        handle::IPC_A,
        Signals::READABLE,
        USER_DATA_UNUSED,
    );
    if let Err(e) = add_res {
        if e != Error::ResourceExhausted {
            return Err(Error::Internal);
        }
    } else {
        return Err(Error::Internal);
    }

    test_logger::info!("Object removed from incorrect wait group");
    let remove_res = syscall::wait_group_remove(handle::WAIT_GROUP_2, handle::IPC_A);
    if let Err(e) = remove_res {
        if e != Error::NotFound {
            return Err(Error::Internal);
        }
    } else {
        return Err(Error::Internal);
    }

    syscall::wait_group_remove(handle::WAIT_GROUP_1, handle::IPC_B)?;
    syscall::wait_group_remove(handle::WAIT_GROUP_1, handle::IPC_A)?;

    test_logger::info!("Object removed when not in any wait group");
    let remove_res = syscall::wait_group_remove(handle::WAIT_GROUP_1, handle::IPC_A);
    if let Err(e) = remove_res {
        if e != Error::NotFound {
            return Err(Error::Internal);
        }
    } else {
        return Err(Error::Internal);
    }

    test_logger::info!("Waiting on empty wait group");
    let wait_return = syscall::object_wait(handle::WAIT_GROUP_1, Signals::READABLE, Instant::MAX);
    if let Err(e) = wait_return {
        if e != Error::InvalidArgument {
            return Err(Error::Internal);
        }
    } else {
        return Err(Error::Internal);
    }

    test_logger::info!("Nested wait groups not supported");
    let add_res = syscall::wait_group_add(
        handle::WAIT_GROUP_1,
        handle::WAIT_GROUP_2,
        Signals::READABLE,
        USER_DATA_UNUSED,
    );
    // No objects in the wait group should return an error.
    if let Err(e) = add_res {
        if e != Error::InvalidArgument {
            return Err(Error::Internal);
        }
    } else {
        return Err(Error::Internal);
    }

    test_add_signaled_member_wakes_waiter()?;

    // Respond to the final IPC so the initiator can finish and shut down.
    let mut buffer = [0u8; size_of::<char>()];
    let len = syscall::channel_read(handle::IPC_A, 0, &mut buffer)?;
    if len != buffer.len() || char::from_u32(u32::from_ne_bytes(buffer)) != Some('d') {
        return Err(Error::InvalidArgument);
    }
    syscall::channel_respond(handle::IPC_A, &3usize.to_ne_bytes())?;

    Ok(())
}

const BLOCKED_WAIT_TIMEOUT: Duration = Duration::from_secs(2);
const HELPER_THREAD_STARTUP_DELAY: Duration = Duration::from_millis(30);
const POLL_DELAY: Duration = Duration::from_millis(10);

const WAITER_STACK_SIZE: usize = 1024;
#[repr(align(16))]
struct Stack([u8; WAITER_STACK_SIZE]);
static mut WAITER_STACK: Stack = Stack([0; WAITER_STACK_SIZE]);

static WAITER_READY: AtomicBool = AtomicBool::new(false);

extern "C" fn woken_by_add_waiter_entry(_arg: usize) -> ! {
    WAITER_READY.store(true, Ordering::Release);
    let deadline = SystemClock::now() + BLOCKED_WAIT_TIMEOUT;
    let res = match syscall::object_wait(handle::WAIT_GROUP_1, Signals::READABLE, deadline) {
        Ok(wait_return) => {
            if wait_return.user_data == USER_DATA_SIGNALED_ADD
                && wait_return.pending_signals.contains(Signals::READABLE)
            {
                Ok(())
            } else {
                test_logger::error!(
                    "Unexpected WaitReturn: user_data={}, signals={:#x}",
                    wait_return.user_data as u32,
                    wait_return.pending_signals.bits() as u32,
                );
                Err(Error::Internal)
            }
        }
        Err(e) => Err(e),
    };
    syscall::thread_exit(res.status_code());
}

// Verify that adding an already-signaled object to a wait group from one thread
// wakes a different thread already blocked in `object_wait()`.
fn test_add_signaled_member_wakes_waiter() -> Result<()> {
    test_logger::info!(
        "Adding already-signaled member while a thread is blocked on the wait group"
    );

    // Ensure `IPC_A` already has `READABLE` pending before adding it. Use a
    // bounded timeout rather than `Instant::MAX` so the test fails fast instead
    // of hanging if the initiator has not sent the message.
    syscall::object_wait(
        handle::IPC_A,
        Signals::READABLE,
        SystemClock::now() + BLOCKED_WAIT_TIMEOUT,
    )?;

    // Add an unsignaled member (`IPC_B`) so `WAIT_GROUP_1` is non-empty and a
    // waiter will block in `object_wait()`.
    syscall::wait_group_add(
        handle::WAIT_GROUP_1,
        handle::IPC_B,
        Signals::READABLE,
        USER_DATA_BLOCKED_WAITER,
    )?;

    WAITER_READY.store(false, Ordering::Release);
    let initial_pc = woken_by_add_waiter_entry as *const () as usize;
    // SAFETY: `add(WAITER_STACK_SIZE)` points one past the end of
    // `WAITER_STACK.0`. `WAITER_STACK` is used exclusively by `WAITER_THREAD`
    // for the lifetime of that thread's execution, and no references into
    // `WAITER_STACK` escape past thread termination.
    let initial_sp = unsafe {
        (&raw mut WAITER_STACK.0)
            .cast::<u8>()
            .add(WAITER_STACK_SIZE) as usize
    };
    syscall::thread_start(handle::WAITER_THREAD, initial_pc, initial_sp)?;

    while !WAITER_READY.load(Ordering::Acquire) {
        sleep_until(SystemClock::now() + POLL_DELAY)?;
    }
    // Give `WAITER_THREAD` a timeslice after storing `WAITER_READY` to enter
    // `object_wait()` in the kernel before adding the signaled object.
    sleep_until(SystemClock::now() + HELPER_THREAD_STARTUP_DELAY)?;
    syscall::wait_group_add(
        handle::WAIT_GROUP_1,
        handle::IPC_A,
        Signals::READABLE,
        USER_DATA_SIGNALED_ADD,
    )?;

    syscall::object_wait(handle::WAITER_THREAD, Signals::JOINABLE, Instant::MAX)?;
    if syscall::task_join(handle::WAITER_THREAD)? != ExitStatus::Success(0) {
        test_logger::error!("Waiter thread failed");
        return Err(Error::Internal);
    }

    syscall::wait_group_remove(handle::WAIT_GROUP_1, handle::IPC_A)?;
    syscall::wait_group_remove(handle::WAIT_GROUP_1, handle::IPC_B)?;

    Ok(())
}

#[entry]
fn entry() -> Result<()> {
    wait_group().inspect_err(|e| {
        // On error, log that it occurred and, since this is written as a test,
        // shut down the system with the error code.
        test_logger::error!("WaitGroup service error: {}", *e as u32);
        let _ = syscall::debug_shutdown(Err(*e));
    })
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}
