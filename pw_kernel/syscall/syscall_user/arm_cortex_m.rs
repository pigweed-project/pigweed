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

macro_rules! syscall_asm {
    ($id:ident, 0) => {
        naked_asm!("
            push  {{r4-r5, r11}}
            mov   r11, {id}
            svc   0
            mov   r0, r4
            mov   r1, r5
            pop  {{r4-r5, r11}}
            bx lr
            ",
            id = const SysCallId::$id as u32
        )
    };

    ($id:ident, 1) => {
        naked_asm!("
            push  {{r4-r5, r11}}
            mov   r11, {id}
            mov   r4, r0
            svc   0
            mov   r0, r4
            mov   r1, r5
            pop  {{r4-r5, r11}}
            bx lr
            ",
            id = const SysCallId::$id as u32
        )
    };

    ($id:ident, 2) => {
        naked_asm!("
            push  {{r4-r5, r11}}
            mov   r11, {id}
            mov   r4, r0
            mov   r5, r1
            svc   0
            mov   r0, r4
            mov   r1, r5
            pop  {{r4-r5, r11}}
            bx lr
            ",
            id = const SysCallId::$id as u32
        )
    };

    ($id:ident, 3) => {
        naked_asm!("
            push  {{r4-r6, r11}}
            mov   r11, {id}
            mov   r4, r0
            mov   r5, r1
            mov   r6, r2
            svc   0
            mov   r0, r4
            mov   r1, r5
            pop  {{r4-r6, r11}}
            bx lr
            ",
            id = const SysCallId::$id as u32
        )
    };

    ($id:ident, 4) => {
        naked_asm!("
            push  {{r4-r7, r11}}
            mov   r11, {id}
            mov   r4, r0
            mov   r5, r1
            mov   r6, r2
            mov   r7, r3
            svc   0
            mov   r0, r4
            mov   r1, r5
            pop  {{r4-r7, r11}}
            bx lr
            ",
            id = const SysCallId::$id as u32
        )
    };

    ($id:ident, 2_u64) => {
        // The u64 arg here is naturally aligned so the 4 arg wrapper is used.
        syscall_asm!($id, 4)
    };

    ($id:ident, 5_u64) => {
        naked_asm!("
            push  {{r4-r11}}
            mov   r11, {id}
            mov   r4, r0
            mov   r5, r1
            mov   r6, r2
            mov   r7, r3
            ldr   r8, [sp, #(8 * 4)]
            // Stack is padded so that the u64 is 8 byte aligned
            ldr   r9, [sp, #(10 * 4)]
            ldr   r10, [sp, #(11 * 4)]
            svc   0
            mov   r0, r4
            mov   r1, r5
            pop  {{r4-r11}}
            bx lr
            ",
            id = const SysCallId::$id as u32
        )
    };
}

macro_rules! syscall_veneer {
    ($id:ident, $arg_slots:tt, $name:ident($($arg_name:ident: $arg_type:ty),*)) => {
        #[unsafe(naked)]
        unsafe extern "C" fn $name($($arg_name: $arg_type),*) -> i64 {
            syscall_asm!($id, $arg_slots)
        }
    };
}

syscall_veneer!(ObjectWait, 2_u64, object_wait(handle: u32, signals: u32, deadline: u64));
syscall_veneer!(ChannelTransact, 5_u64, channel_transact(
    object_handle: u32,
    send_data: *const u8,
    send_len: usize,
    recv_data: *mut u8,
    recv_len: usize,
    deadline: u64
));
syscall_veneer!(ChannelRead, 4, channel_read(
    handle: u32,
    offset: usize,
    buffer: *mut u8,
    buffer_len: usize
));
syscall_veneer!(ChannelRespond, 3, channel_respond(
    handle: u32,
    buffer: *const u8,
    buffer_len: usize
));
syscall_veneer!(InterruptAck, 2, interrupt_ack(handle: u32, signal_mask: Signals));
syscall_veneer!(DebugPutc, 1, putc(a: u32));
syscall_veneer!(DebugShutdown, 1, shutdown(a: u32));
syscall_veneer!(DebugLog, 2, log(buffer: *const u8, buffer_len: usize));
syscall_veneer!(DebugNop, 0, nop());
syscall_veneer!(DebugTriggerInterrupt, 1, debug_trigger_interrupt(irq: u32));

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
