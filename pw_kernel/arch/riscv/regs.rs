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

use regs::*;

#[cfg(feature = "epmp")]
pub mod epmp;
pub mod pmp;
pub mod veer;

#[allow(unused_macros)]
macro_rules! ro_csr_reg {
    ($name:ident, $val_type:ident, $reg_name:literal, $doc:literal) => {
        #[doc=$doc]
        pub struct $name;
        impl $name {
            #[inline]
            pub fn read() -> $val_type {
              mut val: usize;
              core::arch::asm!(concat!("csrr {0}, ", $reg_name), out(reg) val);
              $val_type(val)
            }
        }
    };
}

#[macro_export]
macro_rules! rw_csr_reg {
    ($name:ident, $val_type:ident, $reg_name:literal, $doc:literal) => {
        #[doc=$doc]
        pub struct $name;
        impl $name {
            #[allow(dead_code)]
            #[inline]
            pub fn read() -> $val_type {
              let mut val: usize;
              unsafe {core::arch::asm!(concat!("csrr {0}, ", $reg_name), out(reg) val)};
              $val_type(val)
            }

            #[allow(dead_code)]
            #[inline]
            pub fn write(val: $val_type) {
              unsafe {core::arch::asm!(concat!("csrw ", $reg_name, ", {0}"), in(reg) val.0)};
            }
        }
    };
}

#[derive(Clone, Copy)]
#[repr(usize)]
#[non_exhaustive]
#[allow(dead_code)]
pub enum Exception {
    InstructionAddressMisaligned = 0,
    InstructionAddressFault = 1,
    IllegalInstruction = 2,
    Breakpoint = 3,
    LoadAddressMisaligned = 4,
    LoadAccessFault = 5,
    StoreAddressMisaligned = 6,
    StoreAccessFault = 7,
    EnvironmentCallFromUMode = 8,
    EnvironmentCallFromSMode = 9,
    EnvironmentCallFromMMode = 11,
    InstructionPageFault = 12,
    LoadPageFault = 13,
    StorePageFault = 15,
    SoftwareCheck = 18,
    HardwareError = 19,
}

#[derive(Clone, Copy)]
#[repr(usize)]
#[non_exhaustive]
#[allow(dead_code)]
pub enum Interrupt {
    SupervisorSoftware = 1,
    MachineSoftware = 3,
    SupervisorTimer = 5,
    MachineTimer = 7,
    SupervisorExternal = 9,
    MachineExternal = 11,
    CounterOverflow = 13,
}

#[derive(Clone, Copy)]
pub enum Cause {
    Exception(Exception),
    Interrupt(Interrupt),
}

#[derive(Copy, Clone, Default)]
#[repr(transparent)]
pub struct MCauseVal(pub usize);
impl MCauseVal {
    rw_masked_field!(raw_cause, usize::MAX >> 1, usize, "raw cause");

    /// Extract the trap cause field.
    #[inline]
    #[must_use]
    pub fn cause(&self) -> Cause {
        if self.is_interrupt() {
            // Safety: Interrupt is non-exhaustive
            Cause::Interrupt(unsafe { core::mem::transmute::<usize, Interrupt>(self.raw_cause()) })
        } else {
            // Safety: Interrupt is non-exhaustive
            Cause::Exception(unsafe { core::mem::transmute::<usize, Exception>(self.raw_cause()) })
        }
    }

    /// Returns `true` if the cause is an interrupt
    #[inline]
    #[must_use]
    pub fn is_interrupt(&self) -> bool {
        // is_negative() is used to test the high bit to be word size independent.
        self.0.cast_signed().is_negative()
    }
}

rw_csr_reg!(MCause, MCauseVal, "mcause", "Machine Cause Register");

#[derive(Copy, Clone, Default)]
#[repr(transparent)]
pub struct MtValVal(pub usize);

rw_csr_reg!(MtVal, MtValVal, "mtval", "Machine Trap Value Register");

/// Execution Privilege Level
///
/// Only Machine mode is guaranteed to be implemented.
#[allow(dead_code)]
#[repr(usize)]
pub enum PrivilegeLevel {
    User = 0b00,
    Supervisor = 0b01,
    Reserved = 0b10,
    Machine = 0b11,
}

#[derive(Copy, Clone, Default)]
#[repr(transparent)]
pub struct MStatusVal(pub usize);

impl MStatusVal {
    rw_bool_field!(usize, sie, 1, "S-mode interrupt enable");
    rw_bool_field!(usize, mie, 3, "M-mode interrupt enable");
    rw_bool_field!(usize, spie, 5, "S-mode prior interrupt enable");
    rw_bool_field!(usize, ube, 6, "U-mode big endian");
    rw_bool_field!(usize, mpie, 7, "M-Mode prior interrupt enable");
    rw_bool_field!(usize, spp, 8, "S-Mode previous privilege");
    rw_int_field!(usize, vs, 9, 10, u8, "vector extension state");
    rw_enum_field!(
        usize,
        mpp,
        11,
        12,
        PrivilegeLevel,
        "M-Mode previous privilege"
    );
    rw_int_field!(usize, fs, 13, 14, u8, "FPU state");
    rw_int_field!(usize, xs, 15, 16, u8, "user mode extension state");
    rw_bool_field!(usize, mprv, 17, "modify privilege");
    rw_bool_field!(usize, sum, 18, "supervisor memory access");
    rw_bool_field!(usize, mxr, 19, "make executable readable");
    rw_bool_field!(usize, tvm, 20, "trap virtual memory");
    rw_bool_field!(usize, tw, 21, "timeout wait");
    rw_bool_field!(usize, tsr, 22, "trap sret");

    // Only RV32 fields are enumerated.

    /// Extract the state dirty field.
    #[inline]
    #[must_use]
    pub fn sd(&self) -> bool {
        // is_negative() is used to as a word size independent way to test the
        // high bit.
        self.0.cast_signed().is_negative()
    }
}

rw_csr_reg!(MStatus, MStatusVal, "mstatus", "Machine Status Register");

/// Machine Trap-Vector Base-Address Register Mode
#[allow(dead_code)]
#[repr(usize)]
#[non_exhaustive]
pub enum MtVecMode {
    /// All taps set pc to `base`.
    Direct = 0b00,

    /// Interrupts set pc to `base` + 4 * cause.
    Vectored = 0b01,
}

/// Machine Trap-Vector Base-Address Register value
#[derive(Copy, Clone, Default)]
#[repr(transparent)]
pub struct MtVecVal(pub usize);

impl MtVecVal {
    rw_masked_field!(
        base,
        !0b11,
        usize,
        "Machine Trap-Vector Base-Address Register"
    );
    rw_enum_field!(usize, mode, 0, 1, MtVecMode, "Mode");
}

rw_csr_reg!(
    MtVec,
    MtVecVal,
    "mtvec",
    "Machine Trap-Vector Base-Address Register"
);
