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
#![no_std]
#![no_main]

use app_initiator::handle;
use pw_status::{Error, Result, StatusCode};
use userspace::entry;
use userspace::syscall::{
    Signals, channel_async_cancel, channel_async_transact, channel_async_transact_complete,
    object_wait, wait_group_add,
};
use userspace::time::{Duration, Instant};

const NUM_HANDLERS: u32 = 3;
const ITERATIONS: usize = 100;

fn test_async_cancel() -> Result<()> {
    pw_log::info!("Async Cancel test starting");

    let send_buf = [0x42];
    let mut recv_buf = [0u8; 1];

    // 1. Start a transaction
    // Safety: The send and receive buffers are stack-allocated, will remain valid
    // and will not be mutated until the transaction is cancelled or completed.
    if let Err(e) = unsafe {
        channel_async_transact(
            handle::IPC_1,
            send_buf.as_ptr(),
            send_buf.len(),
            recv_buf.as_mut_ptr(),
            recv_buf.len(),
        )
    } {
        pw_log::error!("channel_async_transact failed (1): {}", e as u32);
        return Err(e);
    }

    // 2. Cancel it
    if let Err(e) = channel_async_cancel(handle::IPC_1) {
        pw_log::error!("channel_async_cancel failed: {}", e as u32);
        return Err(e);
    }

    // 3. Verify that finishing a cancelled transaction fails with Unavailable
    let Err(err) = channel_async_transact_complete(handle::IPC_1) else {
        pw_log::error!("channel_async_transact_complete succeeded on cancelled transaction");
        return Err(Error::Internal);
    };

    if err != Error::Unavailable {
        pw_log::error!(
            "channel_async_transact_complete returned unexpected error: {}",
            err as u32
        );
        return Err(err);
    }

    // 4. Verify that waiting on the cancelled transaction times out
    let Err(err) = object_wait(
        handle::IPC_1,
        Signals::READABLE,
        Instant::from_ticks(0) + Duration::from_millis(10),
    ) else {
        pw_log::error!("object_wait succeeded on cancelled transaction");
        return Err(Error::Internal);
    };

    if err != Error::DeadlineExceeded {
        pw_log::error!("object_wait returned unexpected error: {}", err as u32);
        return Err(err);
    }

    // 5. Start another transaction to verify channel is free.
    // Safety: The send and receive buffers are stack-allocated, will remain valid
    // and will not be mutated until the transaction is cancelled or completed.
    if let Err(e) = unsafe {
        channel_async_transact(
            handle::IPC_1,
            send_buf.as_ptr(),
            send_buf.len(),
            recv_buf.as_mut_ptr(),
            recv_buf.len(),
        )
    } {
        pw_log::error!("channel_async_transact failed (2): {}", e as u32);
        return Err(e);
    }

    // 6. Wait for it to complete.
    let ret = match object_wait(handle::IPC_1, Signals::READABLE, Instant::MAX) {
        Ok(res) => res,
        Err(e) => {
            pw_log::error!("object_wait failed: {}", e as u32);
            return Err(e);
        }
    };
    if !ret.pending_signals.contains(Signals::READABLE) {
        pw_log::error!("object_wait returned success but READABLE is missing");
        return Err(Error::Internal);
    }

    // 7. Complete
    let len = match channel_async_transact_complete(handle::IPC_1) {
        Ok(l) => l,
        Err(e) => {
            pw_log::error!("channel_async_transact_complete failed: {}", e as u32);
            return Err(e);
        }
    };

    if len != 1 {
        pw_log::error!("Length mismatch: got {}", len as u32);
        return Err(Error::Internal);
    }

    Ok(())
}

fn test_increment_async_ipcs() -> Result<()> {
    pw_log::info!("Async Ipc test starting");

    let handles = [handle::IPC_1, handle::IPC_2, handle::IPC_3];
    let increments = [1u8, 2u8, 3u8];

    // Add channels to wait group.
    for (i, &h) in handles.iter().enumerate() {
        if let Err(e) = wait_group_add(handle::WAIT_GROUP, h, Signals::READABLE, i) {
            pw_log::error!(
                "wait_group_add failed for handle {}: {}",
                i as u32,
                e as u32
            );
            return Err(e);
        }
    }

    for val in 0..ITERATIONS {
        let val_u8 = val as u8;
        let send_buf = [val_u8];
        // 3 buffers, 1 byte each
        let mut recv_buffers = [[0u8; 1]; 3];

        // Start async transactions on all channels.
        for (i, &h) in handles.iter().enumerate() {
            // Safety: The send and receive buffers are stack-allocated, will remain valid
            // and will not be mutated until the transaction is cancelled or completed.
            if let Err(e) = unsafe {
                channel_async_transact(
                    h,
                    send_buf.as_ptr(),
                    send_buf.len(),
                    recv_buffers[i].as_mut_ptr(),
                    recv_buffers[i].len(),
                )
            } {
                pw_log::error!(
                    "channel_async_transact failed for handle {}: {}",
                    i as u32,
                    e as u32
                );
                return Err(e);
            }
        }

        // Wait for all transactions to complete.
        let mut completed_count = 0;
        while completed_count < NUM_HANDLERS {
            // Wait for ANY channel to be readable
            let result = match object_wait(handle::WAIT_GROUP, Signals::READABLE, Instant::MAX) {
                Ok(res) => res,
                Err(e) => {
                    pw_log::error!("object_wait failed: {}", e as u32);
                    return Err(e);
                }
            };

            let idx = result.user_data as usize;
            if idx >= 3 {
                return Err(Error::OutOfRange);
            }

            let signals = result.pending_signals;
            if !signals.contains(Signals::READABLE) {
                return Err(Error::Internal);
            }

            // Complete the transaction for this channel
            let recv_len = match channel_async_transact_complete(handles[idx]) {
                Ok(len) => len,
                Err(e) => {
                    pw_log::error!(
                        "channel_async_transact_complete failed for handle {}: {}",
                        idx as u32,
                        e as u32
                    );
                    return Err(e);
                }
            };

            if recv_len != 1 {
                pw_log::error!(
                    "Length mismatch for handler {}: got {}",
                    (idx + 1) as u32,
                    recv_len as u32
                );
                return Err(Error::OutOfRange);
            }

            let received = recv_buffers[idx][0];
            let expected = val_u8.wrapping_add(increments[idx]);

            if received != expected {
                pw_log::error!(
                    "Value mismatch handler {}: sent {}, got {}, want {}",
                    (idx + 1) as u32,
                    val as u32,
                    received as u32,
                    expected as u32
                );
                return Err(Error::Internal);
            }

            completed_count += 1;
        }

        pw_log::info!("Iteration {} passed", val as u32);
    }

    Ok(())
}

#[entry]
fn entry() -> ! {
    pw_log::info!("🔄 RUNNING");

    let ret = test_async_cancel().and_then(|_| test_increment_async_ipcs());

    // Log that an error occurred so that the app that caused the shutdown is logged.
    if ret.is_err() {
        pw_log::error!("❌ FAILED: {}", ret.status_code() as u32);
    } else {
        pw_log::info!("✅ PASSED");
    }

    // Since this is written as a test, shut down with the return status from `main()`.
    let _ = userspace::syscall::debug_shutdown(ret);
    loop {}
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    // Attempt to log panic (might fail if lock held?)
    // pw_log::error!("Panic: {:?}", _info);
    use userspace::syscall::debug_shutdown;
    let _ = debug_shutdown(Err(pw_status::Error::Internal));
    loop {}
}
