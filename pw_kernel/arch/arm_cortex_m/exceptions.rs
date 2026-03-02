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
#![allow(non_snake_case)]

use core::ptr::with_exposed_provenance;

#[cfg(not(feature = "user_space"))]
pub(crate) use arm_cortex_m_macro::kernel_only_exception as exception;
#[cfg(feature = "user_space")]
pub(crate) use arm_cortex_m_macro::user_space_exception as exception;
use pw_cast::CastInto as _;
use pw_log::info;
use regs::*;

#[cfg(feature = "user_space")]
use crate::regs::msr::ControlVal;

/// Combined Exception Return Program Status Register Value
#[repr(transparent)]
pub struct RetPsrVal(pub u32);
impl RetPsrVal {
    rw_int_field!(u32, exception, 0, 8, u16, "exception number");
    rw_bool_field!(u32, sprealign, 9, "stack pointer re-align");
    rw_int_field!(u32, branch_flags, 10, 19, u16, "combined branch flags");
    rw_bool_field!(u32, sfpa, 20, "secure floating point active");
    rw_bool_field!(u32, b, 21, "branch target idetification active");
    rw_bool_field!(u32, t, 24, "T32 state");
    rw_int_field!(u32, it, 25, 26, u8, "if-then flags");
    rw_bool_field!(u32, q, 27, "saturate flag");
    rw_bool_field!(u32, v, 28, "overflow flag");
    rw_bool_field!(u32, c, 29, "carry flag");
    rw_bool_field!(u32, z, 30, "zero flag");
    rw_bool_field!(u32, n, 31, "negative flag");
}

/// Exception Return Payload (EXC_RETURN)
///
/// Represents an Exception Return Payload (EXC_RETURN) as described in D1.2.26
/// of the Armv8-M Architecture Reference Manual.
#[repr(transparent)]
pub struct ExcReturn(usize);

impl ExcReturn {
    const S: usize = 1 << 6;
    const DCRS: usize = 1 << 5;
    const F_TYPE: usize = 1 << 4;
    const MODE: usize = 1 << 3;
    const SP_SEL: usize = 1 << 2;
    const ES: usize = 1 << 0;
    const RESERVED: usize = 0xffffff80;

    /// Create a new ExcReturn payload
    pub const fn new(
        stack_type: ExcReturnStack,
        register_stacking: ExcReturnRegisterStacking,
        frame_type: ExcReturnFrameType,
        mode: ExcReturnMode,
    ) -> Self {
        Self(
            Self::RESERVED
                | stack_type as usize
                | register_stacking as usize
                | frame_type as usize
                | mode as usize,
        )
    }

    /// Returns the bitwise representation of this return payload
    pub const fn bits(&self) -> usize {
        self.0
    }

    pub const fn is_main_stack(&self) -> bool {
        self.0 & Self::SP_SEL == 0
    }
}

/// Which Stack to use on exception return
///
/// This represents a combination of the `S` and `SPSEL` bits in the Exception
/// Return Payload.
#[derive(Clone, Copy)]
#[allow(dead_code)]
#[expect(clippy::enum_variant_names)]
#[repr(usize)]
pub enum ExcReturnStack {
    /// Main, Non-Secure stack
    MainNonSecure = 0,

    /// Thread, Non-Secure stack
    ThreadNonSecure = ExcReturn::SP_SEL,

    /// Main, Secure stack
    MainSecure = ExcReturn::S,

    /// Thread, Secure stack
    ThreadSecure = ExcReturn::S | ExcReturn::SP_SEL,
}

/// Default callee register stacking
///
/// This represents the `DCRS` bit in the Exception Return Payload.
#[derive(Clone, Copy)]
#[allow(dead_code)]
#[repr(usize)]
pub enum ExcReturnRegisterStacking {
    /// Skip stacking of additional state context registers.
    Skipped = 0,

    /// Default rules for stacking of additional state context registers.
    Default = ExcReturn::DCRS,
}

/// Stack frame type
///
/// This represents the `FType` bit in the Exception Return Payload.
#[derive(Clone, Copy)]
#[allow(dead_code)]
#[repr(usize)]
pub enum ExcReturnFrameType {
    /// Standard Stack Frame
    Standard = ExcReturn::F_TYPE,
    /// Extended Stack Frame
    Extended = 0,
}

/// Return Mode
///
/// This represents a combination of the `MODE` and `ES` bits in the Exception
/// Return Payload.
#[derive(Clone, Copy)]
#[allow(dead_code)]
#[expect(clippy::enum_variant_names)]
#[repr(usize)]
pub enum ExcReturnMode {
    /// Handler, Non-Secure mode
    HandlerNonSecure = 0,

    /// Thread, Non-Secure mode
    ThreadNonSecure = ExcReturn::MODE,

    /// Handler, Secure mode
    HandlerSecure = ExcReturn::ES,

    /// Thread, Secure mode
    ThreadSecure = ExcReturn::MODE | ExcReturn::ES,
}

/// Type of all exception handlers.
///
/// # Returns
/// Returns the frame representing the context in which to return.  This lets
/// exceptions perform context switches.
#[allow(dead_code)]
pub type ExceptionHandler = extern "C" fn(*mut KernelExceptionFrame) -> *mut KernelExceptionFrame;

/// Exception frame with only the registers pushed by the hardware.
///
/// Note: This is pushed to the active stack at the time of the exception which
/// may be either a user-mode or kernel stack.
#[repr(C)]
pub struct ExceptionFrame {
    pub r0: u32,
    pub r1: u32,
    pub r2: u32,
    pub r3: u32,
    pub r12: u32,
    pub lr: u32,
    pub pc: u32,
    pub psr: RetPsrVal,
}

impl ExceptionFrame {
    #[inline(never)]
    #[allow(dead_code)]
    pub fn dump(&self) {
        info!("Exception frame {:#08x}:", &raw const *self as usize);
        info!(
            "r0  {:#010x} r1 {:#010x} r2  {:#010x} r3  {:#010x}",
            self.r0 as u32, self.r1 as u32, self.r2 as u32, self.r3 as u32
        );
        info!(
            "r12 {:#010x} lr {:#010x} pc  {:#010x} psr {:#010x}",
            self.r12 as u32, self.lr as u32, self.pc as u32, self.psr.0 as u32
        );
    }
}

/// Exception frame with the registers that the kernel first level assembly
/// exception handler wrapper pushes.
///
/// Note: Always pushed to the kernel stack.
#[repr(C)]
pub struct KernelExceptionFrame {
    pub r4: u32,
    pub r5: u32,
    pub r6: u32,
    pub r7: u32,
    pub r8: u32,
    pub r9: u32,
    pub r10: u32,
    pub r11: u32,

    #[cfg(feature = "user_space")]
    pub psp: u32,
    #[cfg(feature = "user_space")]
    pub control: ControlVal,

    // return_address needs to be the last entry of the frame as it is popped
    // off the stack directly into `pc` causing the exception to return.
    pub return_address: u32,
}

impl KernelExceptionFrame {
    #[inline(never)]
    pub fn dump(&self) {
        info!("Kernel exception frame {:#08x}:", &raw const *self as usize);
        info!(
            "r4  {:#010x} r5 {:#010x} r6  {:#010x} r7  {:#010x}",
            self.r4 as u32, self.r5 as u32, self.r6 as u32, self.r7 as u32
        );
        info!(
            "r8  {:#010x} r9 {:#010x} r10 {:#010x} r11 {:#010x}",
            self.r8 as u32, self.r9 as u32, self.r10 as u32, self.r11 as u32
        );
        info!("return_address {:#010x}", self.return_address as u32,);

        #[cfg(feature = "user_space")]
        info!(
            "psp {:#010x} control {:#010x}",
            self.psp as u32, self.control.0 as u32,
        );

        #[cfg(feature = "user_space")]
        self.dump_user_frame();
    }

    #[cfg(feature = "user_space")]
    fn dump_user_frame(&self) {
        use pw_cast::CastFrom;
        let user_frame = if self.return_address & u32::cast_from(ExcReturn::SP_SEL) == 0 {
            // If we came from the Main stack, the user frame is directly above
            // the kernel frame on the stack.
            unsafe {
                (&raw const *self)
                    .byte_add(size_of::<Self>())
                    .cast::<ExceptionFrame>()
            }
        } else {
            // If we came from the Thread stack, the user frame is pointed to by
            // the psp field of the kernel frame.
            with_exposed_provenance::<ExceptionFrame>(self.psp.cast_into())
        };

        unsafe { &*user_frame }.dump();
    }
}

fn is_execption_from_kernel(frame: *const KernelExceptionFrame) -> bool {
    ExcReturn(unsafe { &*frame }.return_address.cast_into()).is_main_stack()
}

#[exception(exception = "HardFault")]
#[unsafe(no_mangle)]
extern "C" fn pw_kernel_hard_fault(frame: *mut KernelExceptionFrame) -> *mut KernelExceptionFrame {
    let hfsr = with_exposed_provenance::<u32>(0xe000ed2c);
    info!(
        "HardFault exception triggered: HFSR={:#010x}",
        unsafe { hfsr.read_volatile() } as u32
    );

    unsafe { &*frame }.dump();

    kernel::scheduler::handle_terminal_exception(super::Arch, is_execption_from_kernel(frame));
    frame
}

#[exception(exception = "DefaultHandler")]
#[unsafe(no_mangle)]
extern "C" fn pw_kernel_default(frame: *mut KernelExceptionFrame) -> *mut KernelExceptionFrame {
    info!("DefaultHandler exception triggered");
    unsafe { &*frame }.dump();
    kernel::scheduler::handle_terminal_exception(super::Arch, is_execption_from_kernel(frame));
    frame
}

#[exception(exception = "NonMaskableInt")]
#[unsafe(no_mangle)]
extern "C" fn pw_kernel_non_maskable_int(
    frame: *mut KernelExceptionFrame,
) -> *mut KernelExceptionFrame {
    info!("NonMaskableInt exception triggered");
    unsafe { &*frame }.dump();
    kernel::scheduler::handle_terminal_exception(super::Arch, is_execption_from_kernel(frame));
    frame
}

#[exception(exception = "MemoryManagement")]
#[unsafe(no_mangle)]
extern "C" fn pw_kernel_memory_management(
    frame: *mut KernelExceptionFrame,
) -> *mut KernelExceptionFrame {
    let mmfar = with_exposed_provenance::<u32>(0xe000ed34);
    info!(
        "MemoryManagement exception triggered: address={:#010x}",
        unsafe { mmfar.read_volatile() } as u32
    );
    unsafe { &*frame }.dump();

    kernel::scheduler::handle_terminal_exception(super::Arch, is_execption_from_kernel(frame));
    frame
}

#[exception(exception = "BusFault")]
#[unsafe(no_mangle)]
extern "C" fn pw_kernel_bus_fault(frame: *mut KernelExceptionFrame) -> *mut KernelExceptionFrame {
    let bfar = with_exposed_provenance::<u32>(0xe000ed38);
    info!(
        "BusFault exception triggered: address={:#010x}",
        unsafe { bfar.read_volatile() } as u32
    );
    unsafe { &*frame }.dump();
    kernel::scheduler::handle_terminal_exception(super::Arch, is_execption_from_kernel(frame));
    frame
}

#[exception(exception = "UsageFault")]
#[unsafe(no_mangle)]
extern "C" fn pw_kernel_usage_fault(frame: *mut KernelExceptionFrame) -> *mut KernelExceptionFrame {
    info!("UsageFault exception triggered");
    unsafe { &*frame }.dump();
    kernel::scheduler::handle_terminal_exception(super::Arch, is_execption_from_kernel(frame));
    frame
}

// PendSV is defined in thread.rs
// SVCall is defined in syscall.rs

#[exception(exception = "DebugMonitor")]
#[unsafe(no_mangle)]
extern "C" fn pw_kernel_debug_monitor(
    frame: *mut KernelExceptionFrame,
) -> *mut KernelExceptionFrame {
    info!("DebugMonitor exception triggered");
    unsafe { &*frame }.dump();
    kernel::scheduler::handle_terminal_exception(super::Arch, is_execption_from_kernel(frame));
    frame
}
