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

use core::mem::size_of;

use app_test_interrupts::{constants, handle};
use pw_status::{Error, Result, StatusCode};
use userspace::syscall::Signals;
use userspace::time::Instant;
use userspace::{entry, syscall};

fn read_expected_value(expected_value: u32) -> Result<()> {
    // the interrupt listener responds on IPC with the interrupt count.
    let wait_return = syscall::object_wait(handle::IPC, Signals::READABLE, Instant::MAX)?;

    if !wait_return.pending_signals.contains(Signals::READABLE) || wait_return.user_data != 0 {
        return Err(Error::Internal);
    }

    let mut buffer = [0u8; size_of::<u32>()];
    let len = syscall::channel_read(handle::IPC, 0, &mut buffer)?;
    if len != buffer.len() {
        return Err(Error::OutOfRange);
    };

    let received_value = u32::from_le_bytes(buffer);
    if received_value != expected_value {
        pw_log::error!(
            "Interrupt count wrong value {} (expected {})",
            received_value as u32,
            expected_value as u32
        );
        return Err(Error::Internal);
    }

    let response_buffer = [0u8; 0];
    syscall::channel_respond(handle::IPC, &response_buffer)?;

    Ok(())
}

fn test_interrupts() -> Result<()> {
    syscall::debug_trigger_interrupt(constants::TEST_IRQ)?;
    read_expected_value(1)?;

    syscall::debug_trigger_interrupt(constants::TEST_IRQ)?;
    read_expected_value(2)?;

    syscall::debug_trigger_interrupt(constants::TEST_IRQ)?;
    read_expected_value(3)?;

    Ok(())
}

#[entry]
fn entry() -> ! {
    pw_log::info!("🔄 RUNNING");
    let ret = test_interrupts();

    // Log that an error occurred so that the app that caused the shutdown is logged.
    if ret.is_err() {
        pw_log::error!("❌ FAILED: {}", ret.status_code() as u32);
    } else {
        pw_log::info!("✅ PASSED");
    }

    // Since this is written as a test, shut down with the return status from `main()`.
    let _ = syscall::debug_shutdown(ret);
    loop {}
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}
