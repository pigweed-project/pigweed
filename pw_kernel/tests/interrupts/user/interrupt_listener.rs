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

use core::sync::atomic::{AtomicU32, Ordering};

use app_test_interrupt_listener::{handle, signals};
use pw_status::{Error, Result};
use userspace::syscall::Signals;
use userspace::time::Instant;
use userspace::{entry, syscall};

static INTERRUPT_COUNT: AtomicU32 = AtomicU32::new(1);

fn handle_interrupt(interrupts: Signals) -> Result<()> {
    if !interrupts.contains(signals::TEST_IRQ) {
        pw_log::error!(
            "Interrupt on wrong signal. {} not in {}",
            signals::TEST_IRQ.bits() as u32,
            interrupts.bits() as u32
        );
        return Err(Error::FailedPrecondition);
    }

    const SEND_BUF_LEN: usize = size_of::<u32>();
    const RECV_BUF_LEN: usize = 0;
    let mut send_buf = [0u8; SEND_BUF_LEN];
    let mut recv_buf = [0u8; RECV_BUF_LEN];

    let count = INTERRUPT_COUNT.fetch_add(1, Ordering::SeqCst);
    send_buf.copy_from_slice(&count.to_le_bytes());
    let _ = syscall::interrupt_ack(handle::TEST_INTERRUPTS, interrupts);

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
    loop {
        let res =
            match syscall::object_wait(handle::TEST_INTERRUPTS, signals::TEST_IRQ, Instant::MAX) {
                Ok(wait_return) => {
                    if !wait_return.pending_signals.contains(signals::TEST_IRQ)
                        || wait_return.user_data != 0
                    {
                        pw_log::error!("Incorrect WaitReturn values");
                        Err(Error::Internal)
                    } else {
                        handle_interrupt(wait_return.pending_signals)
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
