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

use app_test_uart::{handle, mapping};
use pw_status::{Error, Result, StatusCode};
use uart_16550_user::Uart;
use userspace::syscall::Signals;
use userspace::time::Instant;
use userspace::{entry, syscall};

fn read_expected_value(expected_value: u8) -> Result<()> {
    // the UART listener responds on IPC with the value written to the UART.
    let wait_return = syscall::object_wait(handle::IPC, Signals::READABLE, Instant::MAX)?;

    if !wait_return.pending_signals.contains(Signals::READABLE) || wait_return.user_data != 0 {
        return Err(Error::Internal);
    }

    let mut buffer = [0u8; 1];
    let len = syscall::channel_read(handle::IPC, 0, &mut buffer)?;
    if len != 1 {
        return Err(Error::OutOfRange);
    };

    if buffer[0] != expected_value {
        pw_log::error!(
            "UART read() wrong value {} (expected {})",
            buffer[0] as u8,
            expected_value as u8
        );
        return Err(Error::Internal);
    }

    let response_buffer = [0u8; 0];
    syscall::channel_respond(handle::IPC, &response_buffer)?;

    Ok(())
}

fn test_uart_interrupts() -> Result<()> {
    let mut uart = Uart::new(mapping::UART0_START_ADDRESS);

    // enable lo to support writing and then reading back the result.
    uart.enable_loopback();

    // drain UART buffer
    while !uart.read().is_none() {}

    uart.write(7);
    read_expected_value(7)?;

    if !uart.read().is_none() {
        pw_log::error!("Buffer not empty after read");
        return Err(Error::FailedPrecondition);
    }

    for i in 0..3u8 {
        uart.write(i);
        read_expected_value(i)?;
    }

    if !uart.read().is_none() {
        pw_log::error!("Buffer not empty after multiple reads");
        return Err(Error::FailedPrecondition);
    }

    Ok(())
}

#[entry]
fn entry() -> ! {
    pw_log::info!("🔄 RUNNING");
    let ret = test_uart_interrupts();

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
