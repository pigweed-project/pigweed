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
use pw_status::Result;
use syscall_defs::{Signals, SysCallInterface, WaitReturn};

pub struct SysCall {}

impl SysCallInterface for SysCall {
    #[inline(always)]
    fn object_wait(_: u32, _: u32, _: u64) -> Result<WaitReturn> {
        Err(pw_status::Error::Unimplemented)
    }

    #[inline(always)]
    unsafe fn channel_transact(
        _handle: u32,
        _send_data: *const u8,
        _send_len: usize,
        _recv_data: *mut u8,
        _recv_len: usize,
        _deadline: u64,
    ) -> Result<u32> {
        Err(pw_status::Error::Unimplemented)
    }

    #[inline(always)]
    unsafe fn channel_read(
        _handle: u32,
        _offset: usize,
        _buffer: *mut u8,
        _buffer_len: usize,
    ) -> Result<u32> {
        Err(pw_status::Error::Unimplemented)
    }

    #[inline(always)]
    unsafe fn channel_respond(_handle: u32, _buffer: *const u8, _buffer_len: usize) -> Result<()> {
        Err(pw_status::Error::Unimplemented)
    }

    #[inline(always)]
    fn interrupt_ack(_handle: u32, _signal_mask: Signals) -> Result<()> {
        Err(pw_status::Error::Unimplemented)
    }

    #[inline(always)]
    fn debug_putc(_a: u32) -> Result<u32> {
        Err(pw_status::Error::Unimplemented)
    }

    #[inline(always)]
    fn debug_shutdown(_a: u32) -> Result<()> {
        Err(pw_status::Error::Unimplemented)
    }

    #[inline(always)]
    unsafe fn debug_log(_buffer: *const u8, _buffer_len: usize) -> Result<()> {
        Err(pw_status::Error::Unimplemented)
    }

    #[inline(always)]
    fn debug_nop() -> Result<()> {
        Err(pw_status::Error::Unimplemented)
    }

    #[inline(always)]
    fn debug_trigger_interrupt(_irq: u32) -> Result<()> {
        Err(pw_status::Error::Unimplemented)
    }
}
