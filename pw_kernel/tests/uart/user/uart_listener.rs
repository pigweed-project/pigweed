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

use app_test_uart_listener::{handle, mapping, signals};
use pw_status::{Error, Result};
// TODO: once multiple target's are supported, feature flag the UART.
use uart_16550_user::Uart;
use userspace::syscall::Signals;
use userspace::time::Instant;
use userspace::{entry, syscall};

fn handle_interrupt(uart: &mut Uart, interrupts: Signals) -> Result<()> {
    if !interrupts.contains(signals::UART0) {
        pw_log::error!(
            "Interrupt on wrong signal. {} not in {}",
            signals::UART0.bits() as u32,
            interrupts.bits() as u32
        );
        return Err(Error::FailedPrecondition);
    }

    let value = uart.read();
    if value.is_none() {
        pw_log::error!("No data to read");
        return Err(Error::FailedPrecondition);
    }

    let _ = syscall::interrupt_ack(handle::UART_INTERRUPTS, interrupts);

    const SEND_BUF_LEN: usize = 1;
    const RECV_BUF_LEN: usize = 0;
    let mut send_buf = [0u8; SEND_BUF_LEN];
    let mut recv_buf = [0u8; RECV_BUF_LEN];

    send_buf[0] = value.unwrap();
    let len = match syscall::channel_transact(handle::IPC, &send_buf, &mut recv_buf, Instant::MAX) {
        Ok(val) => val,
        Err(err) => {
            return Err(err);
        }
    };

    if len != RECV_BUF_LEN {
        pw_log::error!(
            "Received {} bytes, {} expected",
            len as usize,
            RECV_BUF_LEN as usize
        );
        return Err(Error::OutOfRange);
    }

    Ok(())
}

#[entry]
fn entry() -> ! {
    let mut uart = Uart::new(mapping::UART0_START_ADDRESS);

    loop {
        let res = match syscall::object_wait(handle::UART_INTERRUPTS, signals::UART0, Instant::MAX)
        {
            Ok(wait_return) => {
                if !wait_return.pending_signals.contains(signals::UART0)
                    || wait_return.user_data != 0
                {
                    pw_log::error!("Incorrect WaitReturn values");
                    Err(Error::Internal)
                } else {
                    handle_interrupt(&mut uart, wait_return.pending_signals)
                }
            }
            Err(err) => {
                pw_log::error!("Failed to wait on interrupt");
                Err(err)
            }
        };

        if res.is_err() {
            let _ = syscall::debug_shutdown(res);
        }
    }
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}
