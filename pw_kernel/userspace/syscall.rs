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
use pw_status::{Result, StatusCode};
use syscall_defs::SysCallInterface;
pub use syscall_defs::{Signals, WaitReturn};
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
pub fn channel_read(object_handle: u32, offset: usize, buffer: &mut [u8]) -> Result<usize> {
    unsafe {
        SysCall::channel_read(object_handle, offset, buffer.as_mut_ptr(), buffer.len())
            .map(|ret| ret.cast_into())
    }
}

#[inline(always)]
pub fn channel_respond(object_handle: u32, buffer: &[u8]) -> Result<()> {
    unsafe { SysCall::channel_respond(object_handle, buffer.as_ptr(), buffer.len()) }
}

#[inline(always)]
pub fn interrupt_ack(object_handle: u32, signal_mask: Signals) -> Result<()> {
    SysCall::interrupt_ack(object_handle, signal_mask)
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
