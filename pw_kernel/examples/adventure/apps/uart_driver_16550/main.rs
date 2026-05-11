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
use stream_protocol::StreamServer;
use uart_16550_user::Uart;
use uart_driver_16550_codegen::{constants, handle, signals};
use userspace::syscall::Signals;
use userspace::time::Instant;
use userspace::{entry, syscall};

const SERVER_BUFFER_SIZE: usize = 128;

struct UartDriver {
    uart: Uart,
    server: StreamServer<SERVER_BUFFER_SIZE>,
    wait_group: u32,
    ipc_handle: u32,
    uart_interrupt_handle: u32,
}

impl UartDriver {
    /// Creates a new `UartDriver` instance, automatically registering its IPC
    /// and hardware interrupt handles to the statically configured WaitGroup.
    fn new(
        uart_base_address: usize,
        wait_group: u32,
        ipc_handle: u32,
        uart_interrupt_handle: u32,
    ) -> Result<Self> {
        // Register IPC client transaction channel handle, passing the handle as user_data
        syscall::wait_group_add(
            wait_group,
            ipc_handle,
            Signals::READABLE,
            ipc_handle as usize,
        )?;

        // Register hardware PLIC RX interrupt handle, passing the handle as user_data
        syscall::wait_group_add(
            wait_group,
            uart_interrupt_handle,
            signals::UART0,
            uart_interrupt_handle as usize,
        )?;

        Ok(Self {
            uart: Uart::new(uart_base_address),
            server: StreamServer::new(ipc_handle),
            wait_group,
            ipc_handle,
            uart_interrupt_handle,
        })
    }

    fn handle_ipc(&mut self) -> Result<()> {
        self.server.handle_ipc(|payload| {
            for &byte in payload {
                // Synchronously write data to the UART.
                //
                // TODO: https://pwbug.dev/511250393 - Support buffered UART writes.
                self.uart.write(byte);
            }
        })
    }

    fn handle_uart_interrupt(&mut self, pending_signals: Signals) -> Result<()> {
        let mut read_bytes = 0;
        let mut written_bytes = 0;
        while let Some(byte) = self.uart.read() {
            read_bytes += 1;
            written_bytes += self.server.push_data(&[byte])?;
        }
        if written_bytes < read_bytes {
            pw_log::warn!(
                "UART receive buffer full! Dropped {} byte.",
                (read_bytes - written_bytes) as usize
            );
        }

        // Acknowledge that the interrupt has been handled.  This lets the kernel
        // know it should unmask the interrupt and deliver any further interrupts.
        syscall::interrupt_ack(self.uart_interrupt_handle, pending_signals)
    }

    /// Runs the main wait group event loop indefinitely, propagating any fatal errors.
    fn run(&mut self) -> Result<()> {
        loop {
            let wait_return =
                syscall::object_wait(self.wait_group, Signals::READABLE, Instant::MAX)?;

            let triggered_handle = wait_return.user_data as u32;

            if triggered_handle == self.ipc_handle {
                self.handle_ipc()?;
            } else if triggered_handle == self.uart_interrupt_handle {
                self.handle_uart_interrupt(wait_return.pending_signals)?;
            } else {
                pw_log::warn!(
                    "Unexpected wait group wake on handle: {}",
                    triggered_handle as u32
                );
            }
        }
    }
}

#[entry]
fn main() -> Result<()> {
    pw_log::info!("UART 16550 Driver started!");

    run_driver().inspect_err(|&error| {
        pw_log::error!("Driver failed with error: {}", error as u32);
    })
}

fn run_driver() -> Result<()> {
    let mut driver = UartDriver::new(
        constants::UART_BASE_ADDR as usize,
        handle::MAIN_LOOP_WAIT_GROUP,
        handle::GAME_IPC_HANDLER,
        handle::UART_INTERRUPT,
    )?;
    driver.run()
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    pw_log::error!("PANIC: UART 16550 Driver panicked!");
    syscall::process_exit(Error::Internal as u32);
}
