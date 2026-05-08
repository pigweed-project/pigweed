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

use handler_codegen::handle;
use pw_status::{Error, Result};
use userspace::syscall::Signals;
use userspace::time::Instant;
use userspace::{entry, syscall};

fn wait_group() -> Result<()> {
    pw_log::info!("🔄 WaitGroup service starting");

    syscall::wait_group_add(handle::WAIT_GROUP_1, handle::IPC_A, Signals::READABLE, 11)?;
    syscall::wait_group_add(handle::WAIT_GROUP_1, handle::IPC_B, Signals::READABLE, 22)?;

    for i in 0..3 {
        pw_log::info!("waiting for objects iteration {}", i as usize);
        let wait_return =
            syscall::object_wait(handle::WAIT_GROUP_1, Signals::READABLE, Instant::MAX)?;

        let (ipc_handle, expected_char) = match i {
            0 => {
                if wait_return.user_data != 11
                    || !wait_return.pending_signals.contains(Signals::READABLE)
                {
                    return Err(Error::Internal);
                }
                (handle::IPC_A, 'a')
            }
            1 => {
                if wait_return.user_data != 22
                    || !wait_return.pending_signals.contains(Signals::READABLE)
                {
                    return Err(Error::Internal);
                }
                (handle::IPC_B, 'b')
            }
            2 => {
                if wait_return.user_data != 11
                    || !wait_return.pending_signals.contains(Signals::READABLE)
                {
                    return Err(Error::Internal);
                }
                (handle::IPC_A, 'c')
            }
            _ => {
                pw_log::info!("Unknown iteration: {}", i as usize);
                return Err(Error::Internal);
            }
        };
        pw_log::info!("done waiting on objects iteration {}", i as usize);

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
            pw_log::error!(
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

    pw_log::info!("🔄 Objects can only be in one wait group");
    let add_res =
        syscall::wait_group_add(handle::WAIT_GROUP_2, handle::IPC_A, Signals::READABLE, 3);
    if let Err(e) = add_res {
        if e != Error::ResourceExhausted {
            return Err(Error::Internal);
        }
    } else {
        return Err(Error::Internal);
    }

    pw_log::info!("🔄 Object removed from incorrect wait group");
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

    pw_log::info!("🔄 Object removed when not in any wait group");
    let remove_res = syscall::wait_group_remove(handle::WAIT_GROUP_1, handle::IPC_A);
    if let Err(e) = remove_res {
        if e != Error::NotFound {
            return Err(Error::Internal);
        }
    } else {
        return Err(Error::Internal);
    }

    pw_log::info!("🔄 Waiting on empty wait group");
    let wait_return = syscall::object_wait(handle::WAIT_GROUP_1, Signals::READABLE, Instant::MAX);
    if let Err(e) = wait_return {
        if e != Error::InvalidArgument {
            return Err(Error::Internal);
        }
    } else {
        return Err(Error::Internal);
    }

    pw_log::info!("🔄 Nested wait groups not supported");
    let add_res = syscall::wait_group_add(
        handle::WAIT_GROUP_1,
        handle::WAIT_GROUP_2,
        Signals::READABLE,
        3,
    );
    // No objects in the wait group should return an error.
    if let Err(e) = add_res {
        if e != Error::InvalidArgument {
            return Err(Error::Internal);
        }
    } else {
        return Err(Error::Internal);
    }

    Ok(())
}

#[entry]
fn entry() -> Result<()> {
    wait_group().inspect_err(|e| {
        // On error, log that it occurred and, since this is written as a test,
        // shut down the system with the error code.
        pw_log::error!("WaitGroup service error: {}", *e as u32);
        let _ = syscall::debug_shutdown(Err(*e));
    })
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}
