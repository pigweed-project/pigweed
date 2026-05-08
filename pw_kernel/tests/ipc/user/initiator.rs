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

use initiator_codegen::handle;
use pw_status::{Error, Result};
use userspace::syscall::Signals;
use userspace::time::{Clock, Duration, Instant, SystemClock};
use userspace::{process_entry, syscall};

fn test_uppercase_ipcs() -> Result<()> {
    pw_log::info!("Ipc test starting");
    for c in 'a'..='z' {
        const SEND_BUF_LEN: usize = size_of::<char>();
        const RECV_BUF_LEN: usize = size_of::<char>() * 2;

        let mut send_buf = [0u8; SEND_BUF_LEN];
        let mut recv_buf = [0u8; RECV_BUF_LEN];

        // Encode the character into `send_buf` and send it over to the handler.
        c.encode_utf8(&mut send_buf);
        let len: usize =
            syscall::channel_transact(handle::IPC, &send_buf, &mut recv_buf, Instant::MAX)?;

        // The handler side always sends 8 bytes to make up two full Rust `char`s
        if len != RECV_BUF_LEN {
            pw_log::error!(
                "Received {} bytes, {} expected",
                len as usize,
                RECV_BUF_LEN as usize
            );
            return Err(Error::OutOfRange);
        }

        let (char0_bytes, char1_bytes) = recv_buf.split_at(size_of::<char>());

        // Decode first char.
        let Ok(char0) = u32::from_ne_bytes(char0_bytes.try_into().unwrap()).try_into() else {
            return Err(Error::InvalidArgument);
        };
        let char0: char = char0;

        // Decode second char.
        let Ok(char1) = u32::from_ne_bytes(char1_bytes.try_into().unwrap()).try_into() else {
            return Err(Error::InvalidArgument);
        };
        let char1: char = char1;

        // Log the response character
        pw_log::info!(
            "Sent {}, received ({},{})",
            c as char,
            char0 as char,
            char1 as char
        );

        // Verify that the remote side made the first character uppercase.
        if char0 != c.to_ascii_uppercase() {
            return Err(Error::Unknown);
        }

        // Verify that the remote side left the second character lowercase.
        if char1 != c {
            return Err(Error::Unknown);
        }
    }

    test_ipc_preserves_user_signal()?;

    // Test object_set_peer_user_signal: signal the handler and wait for the echo back.
    // Level-triggered model: the initiator raises USER on the handler, the handler
    // echoes USER back on the initiator and then lowers it.  The initiator only
    // observes; it does not clear the signal the handler raised.
    pw_log::info!("Testing object_set_peer_user_signal");
    syscall::object_set_peer_user_signal(handle::IPC, true).map_err(|_| Error::Internal)?;
    let wait_return = syscall::object_wait(handle::IPC, Signals::USER, Instant::MAX)
        .map_err(|_| Error::Internal)?;
    if !wait_return.pending_signals.contains(Signals::USER) {
        return Err(Error::Internal);
    }
    pw_log::info!("object_set_peer_user_signal round-trip OK");

    Ok(())
}

/// Tests that IPC transactions do not wipe out or reset active Signals::USER signals.
fn test_ipc_preserves_user_signal() -> Result<()> {
    pw_log::info!("📋 Testing that IPC transactions preserve Signals::USER");

    let mut send_buf = [0u8; size_of::<char>()];
    let mut recv_buf = [0u8; size_of::<char>() * 2];
    let deadline = SystemClock::now() + Duration::from_secs(2);

    // Tell the handler to set peer USER signal to true on our initiator
    '!'.encode_utf8(&mut send_buf);
    syscall::channel_transact(handle::IPC, &send_buf, &mut recv_buf, deadline)?;

    // Verify that Signals::USER is active on our initiator
    let wait_res = syscall::object_wait(handle::IPC, Signals::USER, SystemClock::now());
    if wait_res.is_err() {
        pw_log::error!("❌ Expected Signals::USER to be set on initiator");
        return Err(Error::Internal);
    }

    // Perform another normal IPC transaction (e.g., character 'a')
    'a'.encode_utf8(&mut send_buf);
    syscall::channel_transact(handle::IPC, &send_buf, &mut recv_buf, deadline)?;

    // Verify that Signals::USER remained active and was not wiped out by the transaction response!
    let wait_res = syscall::object_wait(handle::IPC, Signals::USER, SystemClock::now());
    if wait_res.is_err() {
        pw_log::error!("❌ Signals::USER was reset or wiped out by the IPC transaction response!");
        return Err(Error::Internal);
    }
    pw_log::info!("✅ Signals::USER correctly remained set across the IPC transaction!");

    // Tell the handler to lower the USER signal
    '#'.encode_utf8(&mut send_buf);
    syscall::channel_transact(handle::IPC, &send_buf, &mut recv_buf, deadline)?;

    Ok(())
}

#[process_entry("initiator")]
fn entry() -> Result<()> {
    pw_log::info!("🔄 RUNNING");

    let ret = test_uppercase_ipcs()
        .inspect(|_| pw_log::info!("✅ PASSED"))
        .inspect_err(|e| pw_log::error!("❌ FAILED: {}", *e as u32));

    // Since this is written as a test, shut down with the return status from `main()`.
    let _ = syscall::debug_shutdown(ret);
    ret
}
