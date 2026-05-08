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
use userspace::time::Instant;
use userspace::{entry, syscall};

fn send_char(ipc_channel: u32, c: char, iteration: usize) -> Result<()> {
    pw_log::info!("Sending {} on channel {}", c as char, ipc_channel as u32);

    const SEND_BUF_LEN: usize = size_of::<char>();
    const RECV_BUF_LEN: usize = size_of::<usize>();

    let mut send_buf = [0u8; SEND_BUF_LEN];
    let mut recv_buf = [0u8; RECV_BUF_LEN];

    // Encode the character into `send_buf` and send it over to the handler.
    c.encode_utf8(&mut send_buf);
    let len: usize =
        syscall::channel_transact(ipc_channel, &send_buf, &mut recv_buf, Instant::MAX)?;

    if len != RECV_BUF_LEN {
        pw_log::error!(
            "Received {} bytes, {} expected",
            len as usize,
            RECV_BUF_LEN as usize
        );
        return Err(Error::OutOfRange);
    }

    let ret = usize::from_ne_bytes(recv_buf.try_into().unwrap());
    if ret != iteration {
        pw_log::error!(
            "Received {} return value, {} expected",
            ret as usize,
            iteration as usize
        );
        return Err(Error::InvalidArgument);
    };

    Ok(())
}

fn test_wait_group() -> Result<()> {
    pw_log::info!("Wait group test starting");
    send_char(handle::IPC_A, 'a', 0)?;
    send_char(handle::IPC_B, 'b', 1)?;
    send_char(handle::IPC_A, 'c', 2)?;

    Ok(())
}

#[entry]
fn entry() -> Result<()> {
    pw_log::info!("🔄 RUNNING");

    let ret = test_wait_group()
        .inspect(|_| pw_log::info!("✅ PASSED"))
        .inspect_err(|e| pw_log::error!("❌ FAILED: {}", *e as u32));

    // Since this is written as a test, shut down with the return status from `main()`.
    let _ = syscall::debug_shutdown(ret);
    ret
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}
