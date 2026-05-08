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

use pw_cast::CastInto;
use pw_status::{Error, Result, StatusCode};
use syscall_defs::SysCallInterface;
pub use syscall_defs::{ExitStatus, Signals, WaitReturn};
use syscall_user::SysCall;

use crate::time::Instant;

#[inline(always)]
pub fn object_wait(
    object_handle: u32,
    signal_mask: Signals,
    deadline: Instant,
) -> Result<WaitReturn> {
    SysCall::object_wait(object_handle, signal_mask.bits(), deadline.ticks())
}

#[inline(always)]
pub fn wait_group_add(
    wait_group: u32,
    object: u32,
    signal_mask: Signals,
    user_data: usize,
) -> Result<()> {
    SysCall::wait_group_add(wait_group, object, signal_mask, user_data)
}

#[inline(always)]
pub fn wait_group_remove(wait_group: u32, object: u32) -> Result<()> {
    SysCall::wait_group_remove(wait_group, object)
}

#[inline(always)]
pub fn channel_transact(
    object_handle: u32,
    send_data: &[u8],
    recv_data: &mut [u8],
    deadline: Instant,
) -> Result<usize> {
    unsafe {
        SysCall::channel_transact(
            object_handle,
            send_data.as_ptr(),
            send_data.len(),
            recv_data.as_mut_ptr(),
            recv_data.len(),
            deadline.ticks(),
        )
        .map(|ret| ret.cast_into())
    }
}

#[inline(always)]
/// Start an asynchronous transaction on a channel.
///
/// # Safety
///
/// This function effectively borrows the memory pointed to by `send_data` and `recv_data` for the
/// duration of the transaction. The caller must ensure that:
///
/// 1. The memory pointed to by `send_data` and `recv_data` remains valid and is not mutated until
///    the transaction is either completed via `channel_async_transact_complete` or cancelled via
///    `channel_async_cancel`.
/// 2. The `recv_data` buffer is large enough to hold the expected response.
///
/// Violating these requirements may result in undefined behavior.
pub unsafe fn channel_async_transact(
    object_handle: u32,
    send_data: *const u8,
    send_len: usize,
    recv_data: *mut u8,
    recv_len: usize,
) -> Result<()> {
    unsafe {
        SysCall::channel_async_transact(object_handle, send_data, send_len, recv_data, recv_len)
    }
}

#[inline(always)]
pub fn channel_async_transact_complete(object_handle: u32) -> Result<usize> {
    SysCall::channel_async_transact_complete(object_handle).map(|ret| ret.cast_into())
}

#[inline(always)]
pub fn channel_async_cancel(object_handle: u32) -> Result<()> {
    SysCall::channel_async_cancel(object_handle)
}

#[inline(always)]
pub fn channel_read(object_handle: u32, offset: usize, buffer: &mut [u8]) -> Result<usize> {
    unsafe {
        SysCall::channel_read(object_handle, offset, buffer.as_mut_ptr(), buffer.len())
            .map(|ret| ret.cast_into())
    }
}

#[inline(always)]
pub fn channel_read_exact(object_handle: u32, offset: usize, buffer: &mut [u8]) -> Result<()> {
    let read = channel_read(object_handle, offset, buffer)?;
    if read != buffer.len() {
        return Err(Error::OutOfRange);
    }
    Ok(())
}

#[inline(always)]
pub fn channel_respond(object_handle: u32, buffer: &[u8]) -> Result<()> {
    unsafe { SysCall::channel_respond(object_handle, buffer.as_ptr(), buffer.len()) }
}

#[inline(always)]
pub fn interrupt_ack(object_handle: u32, signal_mask: Signals) -> Result<()> {
    SysCall::interrupt_ack(object_handle, signal_mask)
}

/// Set (`set=true`) or clear (`set=false`) `Signals::USER` on the paired peer.
#[inline(always)]
pub fn object_set_peer_user_signal(object_handle: u32, set: bool) -> Result<()> {
    SysCall::object_set_peer_user_signal(object_handle, set)
}

#[inline(always)]
pub fn debug_putc(c: char) -> Result<u32> {
    SysCall::debug_putc(c.into())
}

#[inline(always)]
pub fn debug_shutdown(status: Result<()>) -> Result<()> {
    SysCall::debug_shutdown(status.status_code())
}

#[inline(always)]
pub fn debug_log(buffer: &[u8]) -> Result<()> {
    unsafe { SysCall::debug_log(buffer.as_ptr(), buffer.len()) }
}

#[inline(always)]
pub fn debug_nop() -> Result<()> {
    SysCall::debug_nop()
}

#[inline(always)]
pub fn debug_trigger_interrupt(irq: u32) -> Result<()> {
    SysCall::debug_trigger_interrupt(irq)
}

#[must_use]
#[inline(always)]
pub fn debug_clock_now() -> Instant {
    Instant::from_ticks(SysCall::debug_clock_now())
}

#[inline(always)]
pub fn thread_start(object_handle: u32, initial_pc: usize, initial_sp: usize) -> Result<()> {
    SysCall::thread_start(object_handle, initial_pc, initial_sp)
}

#[inline(always)]
pub fn thread_terminate(object_handle: u32) -> Result<()> {
    SysCall::thread_terminate(object_handle)
}

#[inline(always)]
pub fn thread_join(object_handle: u32) -> Result<ExitStatus> {
    SysCall::thread_join(object_handle)
}

#[inline(always)]
pub fn process_start(object_handle: u32) -> Result<()> {
    SysCall::process_start(object_handle)
}

#[inline(always)]
pub fn process_terminate(object_handle: u32) -> Result<()> {
    SysCall::process_terminate(object_handle)
}

#[inline(always)]
pub fn process_join(object_handle: u32) -> Result<ExitStatus> {
    SysCall::process_join(object_handle)
}

#[inline(always)]
pub fn process_exit(exit_code: u32) -> ! {
    SysCall::process_exit(exit_code)
}

#[inline(always)]
pub fn thread_exit(exit_code: u32) -> ! {
    SysCall::thread_exit(exit_code)
}
