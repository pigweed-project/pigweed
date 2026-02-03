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

use core::arch::naked_asm;

use pw_status::Result;
use syscall_defs::{Signals, SysCallId, SysCallInterface, SysCallReturnValue, WaitReturn};

pub struct SysCall {}

macro_rules! syscall_veneer {
    ($id:ident, $name:ident($($arg_name:ident: $arg_type:ty),* $(,)?)) => {
        #[unsafe(naked)]
        unsafe extern "C" fn $name($($arg_name: $arg_type),*) -> i64 {
            naked_asm!("
                li   t0, {id}
                ecall
                ret
                ",
                id = const SysCallId::$id as u32
            );
        }
    };
}

syscall_veneer!(ObjectWait, object_wait(handle: u32, signals: u32, deadline: u64));
syscall_veneer!(ChannelTransact, channel_transact(
    object_handle: u32, // a0
    send_data: *const u8, // a1
    send_len: usize,    // a2
    recv_data: *mut u8, // a3
    recv_len: usize,    // a4
    deadline: u64,      // a6-a7
));
syscall_veneer!(ChannelRead, channel_read(
     object_handle: u32,
     offset: usize,
     buffer: *mut u8,
     buffer_len: usize,
));
syscall_veneer!(ChannelRespond, channel_respond(object_handle: u32, buffer: *const u8, buffer_len: usize));
syscall_veneer!(InterruptAck, interrupt_ack(object_handle: u32, signal_mask: Signals));
syscall_veneer!(DebugPutc, putc(a: u32));
syscall_veneer!(DebugShutdown, shutdown(a: u32));
syscall_veneer!(DebugLog, log(buffer: *const u8, buffer_len: usize));
syscall_veneer!(DebugNop, nop());
syscall_veneer!(DebugTriggerInterrupt, debug_trigger_interrupt(irq: u32));

impl SysCallInterface for SysCall {
    #[inline(always)]
    fn object_wait(handle: u32, signals: u32, deadline: u64) -> Result<WaitReturn> {
        SysCallReturnValue::from(unsafe { object_wait(handle, signals, deadline) }).into()
    }

    #[inline(always)]
    unsafe fn channel_transact(
        handle: u32,
        send_data: *const u8,
        send_len: usize,
        recv_data: *mut u8,
        recv_len: usize,
        deadline: u64,
    ) -> Result<u32> {
        SysCallReturnValue::from(unsafe {
            channel_transact(handle, send_data, send_len, recv_data, recv_len, deadline)
        })
        .into()
    }

    #[inline(always)]
    unsafe fn channel_read(
        handle: u32,
        offset: usize,
        buffer: *mut u8,
        buffer_len: usize,
    ) -> Result<u32> {
        SysCallReturnValue::from(unsafe { channel_read(handle, offset, buffer, buffer_len) }).into()
    }

    #[inline(always)]
    unsafe fn channel_respond(handle: u32, buffer: *const u8, buffer_len: usize) -> Result<()> {
        SysCallReturnValue::from(unsafe { channel_respond(handle, buffer, buffer_len) }).into()
    }

    #[inline(always)]
    fn interrupt_ack(handle: u32, signal_mask: Signals) -> Result<()> {
        SysCallReturnValue::from(unsafe { interrupt_ack(handle, signal_mask) }).into()
    }

    #[inline(always)]
    fn debug_putc(a: u32) -> Result<u32> {
        SysCallReturnValue::from(unsafe { putc(a) }).into()
    }

    #[inline(always)]
    fn debug_shutdown(a: u32) -> Result<()> {
        SysCallReturnValue::from(unsafe { shutdown(a) }).into()
    }

    #[inline(always)]
    unsafe fn debug_log(buffer: *const u8, buffer_len: usize) -> Result<()> {
        SysCallReturnValue::from(unsafe { log(buffer, buffer_len) }).into()
    }

    #[inline(always)]
    fn debug_nop() -> Result<()> {
        SysCallReturnValue::from(unsafe { nop() }).into()
    }

    #[inline(always)]
    fn debug_trigger_interrupt(irq: u32) -> Result<()> {
        SysCallReturnValue::from(unsafe { debug_trigger_interrupt(irq) }).into()
    }
}
