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

use line_reader::LineReader;
use pw_status::{Error, Result};
use stream_protocol::StreamClient;
use userspace::time::{Clock, Duration, Instant, SystemClock};
use userspace::{entry, syscall};

const IO_TIMEOUT: Duration = Duration::from_secs(1);
const READER_BUFFER_SIZE: usize = 128;
const CHUNK_BUFFER_SIZE: usize = 64;

#[entry]
fn main() -> Result<()> {
    pw_log::info!("Adventure Game started!");

    run_game().inspect_err(|&error| {
        pw_log::error!("Adventure Game failed with error: {}", error as u32);
    })
}

/// Primary game execution loop that propagates all recoverable errors cleanly.
fn run_game() -> Result<()> {
    let client = StreamClient::new(adventure_game_codegen::handle::UART_IPC_INITIATOR);

    // Print welcome message with 1 second deadline
    let setup_deadline = SystemClock::now() + IO_TIMEOUT;
    client.write_str("Welcome to Text Adventure!\n", setup_deadline)?;
    client.write_str(
        "You are in a dark room. Type 'help' for commands.\n",
        setup_deadline,
    )?;

    let mut reader = LineReader::<READER_BUFFER_SIZE>::new();

    loop {
        pw_log::info!("Game waiting for input...");

        let mut chunk_buffer = [0u8; CHUNK_BUFFER_SIZE];
        let chunk_len = client.read(&mut chunk_buffer, Instant::MAX)?;

        reader.handle_input(&chunk_buffer[..chunk_len]);

        while let Some(input) = reader.next_line() {
            if !input.is_empty() {
                handle_command(&client, input, SystemClock::now() + IO_TIMEOUT)?;
            }
        }
    }
}

/// Decodes the trimmed command and routes it to the respective sub-handler.
fn handle_command(client: &StreamClient, command: &str, deadline: Instant) -> Result<()> {
    if command.is_empty() {
        return Ok(());
    }
    pw_log::info!("Game received command: {}", command as &str);
    match command {
        "help" => cmd_help(client, deadline)?,
        "look" => cmd_look(client, deadline)?,
        "go north" => cmd_go_north(client, deadline)?,
        "exit" => {
            cmd_exit(client, deadline)?;
        }
        "crash" => {
            cmd_crash(client, deadline)?;
        }
        _ => cmd_unknown(client, deadline)?,
    }
    Ok(())
}

fn cmd_help(client: &StreamClient, deadline: Instant) -> Result<()> {
    client.write_str(
        "Available commands: help, look, go north, exit, crash\n",
        deadline,
    )?;
    Ok(())
}

fn cmd_look(client: &StreamClient, deadline: Instant) -> Result<()> {
    client.write_str(
        "You see dusty walls and a single door to the north.\n",
        deadline,
    )?;
    Ok(())
}

fn cmd_go_north(client: &StreamClient, deadline: Instant) -> Result<()> {
    client.write_str("The door is locked. You need a key.\n", deadline)?;
    Ok(())
}

fn cmd_exit(client: &StreamClient, deadline: Instant) -> Result<()> {
    client.write_str("Goodbye!\n", deadline)?;
    syscall::process_exit(0);
}

fn cmd_crash(client: &StreamClient, deadline: Instant) -> Result<()> {
    client.write_str("Crashing...\n", deadline)?;
    // We use unsafe here only to intentionally trigger a crash
    unsafe {
        core::ptr::read_volatile(core::ptr::null::<u32>());
    }
    pw_assert::panic!("Null pointer dereference did not terminate thread!!!");
}

fn cmd_unknown(client: &StreamClient, deadline: Instant) -> Result<()> {
    client.write_str("Unknown command.\n", deadline)?;
    Ok(())
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    pw_log::error!("PANIC: Adventure Game panicked!");
    syscall::process_exit(Error::Internal as u32);
}
