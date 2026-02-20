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

use pw_status::{Error, Result, StatusCode};
use stress_initiator::handle;
use userspace::time::Instant;
use userspace::{entry, syscall};

fn test_uppercase_ipcs() -> Result<()> {
    pw_log::info!("IPC test starting");
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

    Ok(())
}

#[entry]
fn entry() -> ! {
    pw_log::info!("🔄 RUNNING");

    let mut pass = 0;
    loop {
        let ret = test_uppercase_ipcs();

        // Log that an error occurred so that the app that caused the shutdown is logged.
        if ret.is_err() {
            pw_log::error!("❌ FAILED: {}", ret.status_code() as u32);
        } else {
            pw_log::info!("✅ PASSED {}", pass as u32);
        }
        pass += 1;
    }
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}
