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
use uart_driver_mock_codegen::handle;
use userspace::entry;
use userspace::syscall::{self, Signals};
use userspace::time::{Clock, Duration, SystemClock};

// This mock UART uses these strings to simulate the user typing and
// returns them as if they were received on the UART.
const MOCK_COMMANDS: &[&str] = &[
    "help\n",
    "look\n",
    "go north\n",
    "crash\n",
    "help\n",
    "exit\n",
];

const SERVER_BUFFER_SIZE: usize = 128;
const TYPING_DELAY: Duration = Duration::from_millis(100);

struct MockUart {
    server: StreamServer<SERVER_BUFFER_SIZE>,
    commands: &'static [&'static str],
    command_index: usize,
}

impl MockUart {
    fn new(ipc_handle: u32, commands: &'static [&'static str]) -> Self {
        Self {
            server: StreamServer::new(ipc_handle),
            commands,
            command_index: 0,
        }
    }

    /// Executes a single iteration of the driver loop.
    fn handle(&mut self) -> Result<()> {
        // Wait for incoming client transactions with a timeout to simulate typing delay
        let deadline = SystemClock::now() + TYPING_DELAY;
        let wait_result =
            syscall::object_wait(handle::GAME_IPC_HANDLER, Signals::READABLE, deadline);

        match wait_result {
            Err(pw_status::Error::DeadlineExceeded) => {
                self.simulate_typing()?;
            }
            Err(error) => return Err(error),
            Ok(_) => {
                self.server.handle_ipc(|_payload| {
                    // Mock UART ignores writes.
                })?;
            }
        }
        Ok(())
    }

    /// Simulates the user typing the next command sequence into the stream.
    fn simulate_typing(&mut self) -> Result<()> {
        let command = self.commands[self.command_index];
        self.command_index = (self.command_index + 1) % self.commands.len();

        let command_bytes = command.as_bytes();
        let pushed_bytes = self.server.push_data(command_bytes)?;
        if pushed_bytes < command_bytes.len() {
            pw_log::warn!(
                "UART receive buffer full! Pushed {}/{} bytes.",
                pushed_bytes as u32,
                command_bytes.len() as u32
            );
        }
        Ok(())
    }
}

#[entry]
fn main() {
    pw_log::info!("Mock UART Driver started");

    let mut uart = MockUart::new(handle::GAME_IPC_HANDLER, MOCK_COMMANDS);

    loop {
        if let Err(err) = uart.handle() {
            pw_log::error!("Driver iteration failed: {}", err as u32);
        }
    }
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    pw_log::error!("PANIC: Mock UART Driver panicked!");
    syscall::process_exit(Error::Internal as u32);
}
