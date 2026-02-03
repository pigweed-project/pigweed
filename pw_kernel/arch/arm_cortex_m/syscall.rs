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

use kernel::SyscallArgs;
use kernel::syscall::handle_syscall;
use pw_cast::CastInto as _;
use pw_status::{Error, Result};

use crate::exceptions::{
    ExcReturn, ExcReturnFrameType, ExcReturnMode, ExcReturnRegisterStacking, ExcReturnStack,
    KernelExceptionFrame,
};

pub struct CortexMSyscallArgs<'a> {
    frame: &'a KernelExceptionFrame,
    cur_index: usize,
}

impl<'a> CortexMSyscallArgs<'a> {
    fn new(frame: &'a KernelExceptionFrame) -> Self {
        Self {
            frame,
            cur_index: 0,
        }
    }
}

impl<'a> SyscallArgs<'a> for CortexMSyscallArgs<'a> {
    fn next_usize(&mut self) -> Result<usize> {
        if self.cur_index > 6 {
            return Err(Error::InvalidArgument);
        }
        // Pointer math is used here instead of a match statement as it results
        // in significantly smaller code.
        //
        // SAFETY: Index is range checked above and the r* fields are in consecutive
        // positions in the `KernelExceptionFrame` struct.
        let usize0 = &raw const self.frame.r4;
        let value = unsafe { *usize0.byte_add(self.cur_index * 4) };
        self.cur_index += 1;
        Ok(value.cast_into())
    }

    #[inline(always)]
    fn next_u64(&mut self) -> Result<u64> {
        let low: u64 = self.next_usize()?.cast_into();
        let high: u64 = self.next_usize()?.cast_into();
        Ok(low | high << 32)
    }
}

// Handle a SVCall (syscall) exception.
//
// Enabling preemptable syscalls on M-profile ARM architectures is tricky because
// of the exception handling behavior baked into the hardware.  Exceptions are
// handled in a separate `handler` mode (as opposed to the normal `thread` mode).
// Exceptions can also stack.  This presents two tricky cases for handling
// blocking and preemptable syscalls in handler mode.
//
// 1) Thread A calls a syscall which blocks, causing the scheduler to run thread B.
//    Thread B wakes up and itself call a syscall.  This will cause the core to
//    Fault due to an SVCall exception being taken while an SVCall exception is
//    active.  This can be worked around by clearing the SVCALLACT bit in the
//    SHCSR register.  However this does not work with case 2.
//
// 2) Thread A calls a syscall.  While that syscall is actively processing (not
//    blocked), an interrupt occurs.  If this happens and the above SVCALLACT
//    workaround is used, Thread A will fault on returning from its syscall.  It
//    is believed this is due to internal core state that handles exception
//    stacking.
//
// Instead of trying to handle the syscall in handler mode, the SVCall handler
// pushed a new exception frame to the kernel stack.  This frame is set up to
// return from exception directly as a call to `handle_svc`.  The SVCall is
// immediately returned and handle_svc is executed.
//
// On syscall return, there is no facility to atomically drop privileged
// execution and return to userspace.  Instead a trampoline `svc_return` is used.
// This trampoline is required to be mapped ReadOnlyExecutable in every process
// since half of it is executed in kernel mode and half in user mode.
#[unsafe(no_mangle)]
#[unsafe(naked)]
// The following clippy attributes are required because values passed into
// inline assembly must be cast to i32.
#[allow(clippy::cast_possible_wrap)]
#[allow(clippy::cast_possible_truncation)]
pub unsafe extern "C" fn SVCall() -> ! {
    core::arch::naked_asm!(
        "
            // Disable interrupts while manipulating the exception stack state.
            cpsid i

            // Save the registers (exception frame) not saved by the hardware
            // exception handling logic.
            // see `exceptions::KernelExceptionFrame`
            mrs     r2, control
            mrs     r1, psp
            push    {{ r1 - r2, lr }}

            push    {{ r4 - r11 }}

            // r0 holds the address of the KernelExceptionFrame
            mov     r0, sp

            // Arm exception frames need to be aligned to 8 bytes
            ands    r3, r0, #0x4
            it ne
            subne     sp, 4

            // Elevate to privilege mode (aka clear the nPriv bit) so that our
            // return from exception happens in kernel mode.  The syscall exit
            // trampoline will drop back to non-privileged mode.
            bfc     r2, #0, #1
            msr     control, r2


            // Push a fake exception frame to return from handler mode to
            // thread mode for the bulk of syscall processing.

            // Copy the RETPSR from the initial exception frame.
            //
            // By virtue of the fact this frame is pushed to the user stack
            // within the user process context, either the process has access to
            // this memory or the exception handling will fault and we won't get
            // here.
            //
            // r1 is set to the user stack pointer (PSP) as part of saving the
            // kernel exception frame above.
            //
            // The SPREALIGN from the initial exception is cleared as the stack
            // has been manually aligned above.
            ldr     r3, [r1, #0x1c]
            bfc     r3, #9, #1
            push    {{r3}} // retpsr

            mov     r4, #0x0
            ldr     r5, =svc_return
            ldr     r6, =handle_svc
            push    {{ r4-r6 }}  // r12, lr, pc

            // r0 points to the kernel exception frame above
            // r1 points to the user stack (with the exception frame)
            // r2 contains the current CONTROL register value
            // r3 contains the RETPSR value
            //
            // Of these, only r0 (the kernel exception frame) is used as an
            // argument to `handle_svc`.
            push    {{ r0-r3 }}  // r0, r1, r2, r3

            // Reenable interrupts now that the exception stack state is coherent.
            cpsie i

            // Return from exception into the syscall handler in kernel mode
            // using the ExcReturn value below.
            ldr lr, ={exc_return}
            bx lr
        ",
        exc_return = const ExcReturn::new(
                ExcReturnStack::MainSecure,
                ExcReturnRegisterStacking::Default,
                ExcReturnFrameType::Standard,
                ExcReturnMode::ThreadSecure,
            ).bits() as i32,
    )
}

// Handle syscall return
//
// `svc_return` handles return from a syscall into userspace.  Because the
// SVCall exception has already been returned as part of syscall entry, a
// manual switch into userspace is needed.  This requires this code to be
// executable by both the kernel and userspace.
//
// TODO: https://pwbug.dev/465500606 - Isolate `svc_return` into its own section
// to allow selectively mapping it into userspace instead of the whole kernel.
#[unsafe(no_mangle)]
#[unsafe(naked)]
pub unsafe extern "C" fn svc_return() -> ! {
    core::arch::naked_asm!(
        "
            // Restore the exception framed returned from `handle_svc`.
            mov     sp, r0
            pop     {{ r4 - r11 }}

            // r0 contains the PSP which points at the original exception frame.
            // r1 contains the original CONTROL value at the time of the syscall.
            // lr is the original exception ExcReturn value and is discarded.
            pop     {{ r0 - r1, lr }}

            // Restore the user stack pointer (PSP).
            msr     psp, r0

            // Switch to non-privileged mode and the process stack.
            orr     r1, r1, 0x3
            msr     control, r1

            // Per ARM's recommendations, an instruction barrier ensures that
            // instructions executed after this point respect the dropping of
            // the privilege level.
            //
            // See https://developer.arm.com/documentation/107656/0101/Registers/Special-purpose-registers/CONTROL-register/Changing-privilege-level-using-the-CONTROL-register

            isb


            // Restore the standard exception frame pushed by the hardware while
            // handling the initial SVCall from userspace.
            pop {{r0-r3, r12, lr}}

            // Per the syscall ABI, r0-r3 are not saved allowing their use as
            // temporaries here.  Care is taken not to store any non-user
            // accessible data in these registers.

            // Store the PC for later return to user space.
            // r1 is not cleared before return to userspace but the userspace's
            // PC is not a privileged value.
            pop {{r1}}

            // Pop RETPSR from the stack.
            pop {{r0}}

            // If the SPREALIGN bit is set, the stack was aligned on exception
            // entry and needs another 4 bytes added to it to undo that alignment.
            ands r0, #(1<<9)
            it ne
            addne sp, 4

            mov r0, #0x0

            // Branch to the original syscall from userspace, forcing thumb mode.
            orr r1, r1, #0x1
            bx r1
    "
    )
}

#[unsafe(no_mangle)]
extern "C" fn handle_svc(frame_ptr: *mut KernelExceptionFrame) -> *mut KernelExceptionFrame {
    let frame = unsafe { &mut *frame_ptr };

    // Explicitly truncate the syscall ID to 16 bits per syscall ABI.
    #[expect(clippy::cast_possible_truncation)]
    let id = frame.r11 as u16;

    let args = CortexMSyscallArgs::new(frame);
    let ret_val = handle_syscall(super::Arch, id, args);
    // Intentionally truncate ret_val to marshal return value into r4 and r5
    #[expect(clippy::cast_possible_truncation)]
    {
        frame.r4 = ret_val.value[0] as u32;
        frame.r5 = ret_val.value[1] as u32;
    }

    frame_ptr
}
