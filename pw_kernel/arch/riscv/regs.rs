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

#[cfg(feature = "timer_clint")]
pub mod clint;
#[cfg(feature = "epmp")]
pub mod epmp;
pub mod mtime;
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

/// First `mcause` interrupt code (and `mie`/`mip` bit) designated for
/// platform / hart-local use per the RISC-V Privileged Specification.
pub const LOCAL_INTERRUPT_BASE: usize = 16;

/// Maximum number of hart-local interrupt lines supported by the standard `mie` / `mip` CSRs.
pub const MAX_LOCAL_INTERRUPTS: usize = usize::BITS as usize - LOCAL_INTERRUPT_BASE;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Exception {
    InstructionAddressMisaligned,
    InstructionAddressFault,
    IllegalInstruction,
    Breakpoint,
    LoadAddressMisaligned,
    LoadAccessFault,
    StoreAddressMisaligned,
    StoreAccessFault,
    EnvironmentCallFromUMode,
    EnvironmentCallFromSMode,
    EnvironmentCallFromMMode,
    InstructionPageFault,
    LoadPageFault,
    StorePageFault,
    SoftwareCheck,
    HardwareError,
    /// Reserved or platform-defined synchronous exception code.
    Unknown(usize),
}

impl Exception {
    /// Decode a raw `mcause` exception code (with the interrupt bit cleared).
    #[inline]
    #[must_use]
    pub const fn from_raw_cause(raw_cause: usize) -> Self {
        match raw_cause {
            0 => Self::InstructionAddressMisaligned,
            1 => Self::InstructionAddressFault,
            2 => Self::IllegalInstruction,
            3 => Self::Breakpoint,
            4 => Self::LoadAddressMisaligned,
            5 => Self::LoadAccessFault,
            6 => Self::StoreAddressMisaligned,
            7 => Self::StoreAccessFault,
            8 => Self::EnvironmentCallFromUMode,
            9 => Self::EnvironmentCallFromSMode,
            11 => Self::EnvironmentCallFromMMode,
            12 => Self::InstructionPageFault,
            13 => Self::LoadPageFault,
            15 => Self::StorePageFault,
            18 => Self::SoftwareCheck,
            19 => Self::HardwareError,
            other => Self::Unknown(other),
        }
    }

    /// Return the raw `mcause` exception code for this exception.
    #[inline]
    #[must_use]
    pub const fn raw_cause(self) -> usize {
        match self {
            Self::InstructionAddressMisaligned => 0,
            Self::InstructionAddressFault => 1,
            Self::IllegalInstruction => 2,
            Self::Breakpoint => 3,
            Self::LoadAddressMisaligned => 4,
            Self::LoadAccessFault => 5,
            Self::StoreAddressMisaligned => 6,
            Self::StoreAccessFault => 7,
            Self::EnvironmentCallFromUMode => 8,
            Self::EnvironmentCallFromSMode => 9,
            Self::EnvironmentCallFromMMode => 11,
            Self::InstructionPageFault => 12,
            Self::LoadPageFault => 13,
            Self::StorePageFault => 15,
            Self::SoftwareCheck => 18,
            Self::HardwareError => 19,
            Self::Unknown(raw) => raw,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Interrupt {
    MachineTimer,
    MachineExternal,
    /// Platform-defined hart-local interrupt (`mcause` interrupt code `>= 16`).
    /// The payload is the 0-based local interrupt line (`raw_cause - 16`).
    Local(usize),
    /// Unhandled, reserved, or supervisor interrupt code (`< 16`).
    Unknown(usize),
}

impl Interrupt {
    /// Decode a raw `mcause` interrupt code (with the interrupt bit cleared).
    #[inline]
    #[must_use]
    pub const fn from_raw_cause(raw_cause: usize) -> Self {
        // Codes >= 16 are platform/hart-local interrupts.
        if raw_cause >= LOCAL_INTERRUPT_BASE {
            Self::Local(raw_cause - LOCAL_INTERRUPT_BASE)
        } else {
            // Only the standard M-mode interrupts pw_kernel handles get named
            // variants; the rest map to `Unknown`.
            match raw_cause {
                7 => Self::MachineTimer,
                11 => Self::MachineExternal,
                other => Self::Unknown(other),
            }
        }
    }

    /// Return the raw `mcause` interrupt code for this interrupt.
    #[inline]
    #[must_use]
    pub const fn raw_cause(self) -> usize {
        match self {
            Self::MachineTimer => 7,
            Self::MachineExternal => 11,
            Self::Local(line) => LOCAL_INTERRUPT_BASE + line,
            Self::Unknown(raw) => raw,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Cause {
    Exception(Exception),
    Interrupt(Interrupt),
}

#[derive(Copy, Clone, Default, Debug, PartialEq, Eq)]
#[repr(transparent)]
pub struct MCauseVal(pub usize);
impl MCauseVal {
    rw_masked_field!(raw_cause, usize::MAX >> 1, usize, "raw cause");

    /// Extract the trap cause field.
    #[inline]
    #[must_use]
    pub const fn cause(&self) -> Cause {
        if self.is_interrupt() {
            Cause::Interrupt(Interrupt::from_raw_cause(self.raw_cause()))
        } else {
            Cause::Exception(Exception::from_raw_cause(self.raw_cause()))
        }
    }

    /// Returns `true` if the cause is an interrupt
    #[inline]
    #[must_use]
    pub const fn is_interrupt(&self) -> bool {
        // is_negative() is used to test the high bit to be word size independent.
        self.0.cast_signed().is_negative()
    }
}

rw_csr_reg!(MCause, MCauseVal, "mcause", "Machine Cause Register");

/// Machine Interrupt Enable Register value
#[derive(Copy, Clone, Default, Debug, PartialEq, Eq)]
#[repr(transparent)]
pub struct MieVal(pub usize);

impl MieVal {
    rw_bool_field!(usize, ssie, 1, "S-mode software interrupt enable");
    rw_bool_field!(usize, msie, 3, "M-mode software interrupt enable");
    rw_bool_field!(usize, stie, 5, "S-mode timer interrupt enable");
    rw_bool_field!(usize, mtie, 7, "M-mode timer interrupt enable");
    rw_bool_field!(usize, seie, 9, "S-mode external interrupt enable");
    rw_bool_field!(usize, meie, 11, "M-mode external interrupt enable");
    rw_bool_field!(usize, lcofie, 13, "Local counter-overflow interrupt enable");

    /// Extract hart-local interrupt `line` enable field (`mie` bit `16 + line`).
    ///
    /// Panics if `line >= MAX_LOCAL_INTERRUPTS`, matching
    /// `with_local_interrupt()` and the `Mie` helpers.
    #[inline]
    #[must_use]
    pub fn local_interrupt(&self, line: usize) -> bool {
        pw_assert::assert!(
            line < MAX_LOCAL_INTERRUPTS,
            "Local interrupt line exceeds maximum supported by mie"
        );
        ops::get_bool(self.0, LOCAL_INTERRUPT_BASE + line)
    }

    /// Update hart-local interrupt `line` enable field (`mie` bit `16 + line`).
    #[inline]
    #[must_use]
    pub fn with_local_interrupt(self, line: usize, val: bool) -> Self {
        pw_assert::assert!(
            line < MAX_LOCAL_INTERRUPTS,
            "Local interrupt line exceeds maximum supported by mie"
        );
        Self(ops::set_bool(self.0, LOCAL_INTERRUPT_BASE + line, val))
    }
}

rw_csr_reg!(Mie, MieVal, "mie", "Machine Interrupt Enable Register");

impl Mie {
    /// Atomically enable hart-local interrupt `line` (`mie` bit `16 + line`).
    #[inline]
    pub fn enable_local_interrupt(line: usize) {
        pw_assert::assert!(
            line < MAX_LOCAL_INTERRUPTS,
            "Local interrupt line exceeds maximum supported by mie"
        );
        let mask = 1usize << (LOCAL_INTERRUPT_BASE + line);
        // SAFETY: Setting a single `mie` bit has no memory-safety implications;
        // the shift is in-bounds because `line` was asserted to be below
        // `MAX_LOCAL_INTERRUPTS`.
        unsafe { core::arch::asm!("csrs mie, {0}", in(reg) mask) };
    }

    /// Atomically disable hart-local interrupt `line` (`mie` bit `16 + line`).
    #[inline]
    pub fn disable_local_interrupt(line: usize) {
        pw_assert::assert!(
            line < MAX_LOCAL_INTERRUPTS,
            "Local interrupt line exceeds maximum supported by mie"
        );
        let mask = 1usize << (LOCAL_INTERRUPT_BASE + line);
        // SAFETY: Clearing a single `mie` bit has no memory-safety implications;
        // the shift is in-bounds because `line` was asserted to be below
        // `MAX_LOCAL_INTERRUPTS`.
        unsafe { core::arch::asm!("csrc mie, {0}", in(reg) mask) };
    }
}

#[derive(Copy, Clone, Default)]
#[repr(transparent)]
pub struct MtValVal(pub usize);

rw_csr_reg!(MtVal, MtValVal, "mtval", "Machine Trap Value Register");

/// Execution Privilege Level
///
/// Only Machine mode is guaranteed to be implemented.
#[allow(dead_code)]
#[derive(Clone, Copy, PartialEq)]
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

#[cfg(test)]
mod tests {
    use unittest::test;

    use super::{Cause, Exception, Interrupt, LOCAL_INTERRUPT_BASE, MCauseVal, Mie, MieVal};

    const INTERRUPT_BIT: usize = 1usize << (usize::BITS - 1);

    #[test]
    fn mcause_decodes_standard_interrupts() -> unittest::Result<()> {
        let timer = MCauseVal(INTERRUPT_BIT | 7);
        unittest::assert_true!(timer.is_interrupt());
        unittest::assert_eq!(timer.raw_cause(), 7);
        unittest::assert_eq!(timer.cause(), Cause::Interrupt(Interrupt::MachineTimer));
        unittest::assert_eq!(Interrupt::MachineTimer.raw_cause(), 7);

        let ext = MCauseVal(INTERRUPT_BIT | 11);
        unittest::assert_eq!(ext.cause(), Cause::Interrupt(Interrupt::MachineExternal));
        unittest::assert_eq!(Interrupt::MachineExternal.raw_cause(), 11);
        Ok(())
    }

    #[test]
    fn mcause_decodes_local_interrupts() -> unittest::Result<()> {
        let line0 = MCauseVal(INTERRUPT_BIT | LOCAL_INTERRUPT_BASE);
        unittest::assert_true!(line0.is_interrupt());
        unittest::assert_eq!(line0.cause(), Cause::Interrupt(Interrupt::Local(0)));
        unittest::assert_eq!(Interrupt::Local(0).raw_cause(), LOCAL_INTERRUPT_BASE);

        let line15 = MCauseVal(INTERRUPT_BIT | (LOCAL_INTERRUPT_BASE + 15));
        unittest::assert_eq!(line15.cause(), Cause::Interrupt(Interrupt::Local(15)));
        unittest::assert_eq!(Interrupt::Local(15).raw_cause(), LOCAL_INTERRUPT_BASE + 15);
        Ok(())
    }

    #[test]
    fn mcause_decodes_unknown_interrupt_and_exception_codes() -> unittest::Result<()> {
        let reserved_irq = MCauseVal(INTERRUPT_BIT | 14);
        unittest::assert_eq!(
            reserved_irq.cause(),
            Cause::Interrupt(Interrupt::Unknown(14))
        );
        unittest::assert_eq!(Interrupt::Unknown(14).raw_cause(), 14);

        let breakpoint = MCauseVal(3);
        unittest::assert_false!(breakpoint.is_interrupt());
        unittest::assert_eq!(breakpoint.cause(), Cause::Exception(Exception::Breakpoint));
        unittest::assert_eq!(Exception::Breakpoint.raw_cause(), 3);

        let custom_exc = MCauseVal(24);
        unittest::assert_false!(custom_exc.is_interrupt());
        unittest::assert_eq!(custom_exc.cause(), Cause::Exception(Exception::Unknown(24)));
        unittest::assert_eq!(Exception::Unknown(24).raw_cause(), 24);
        Ok(())
    }

    #[test]
    fn mie_local_interrupt_bits_map_to_16_plus_line() -> unittest::Result<()> {
        let mie = MieVal::default()
            .with_mtie(true)
            .with_local_interrupt(0, true)
            .with_local_interrupt(5, true);

        unittest::assert_true!(mie.mtie());
        unittest::assert_false!(mie.meie());
        unittest::assert_true!(mie.local_interrupt(0));
        unittest::assert_false!(mie.local_interrupt(1));
        unittest::assert_true!(mie.local_interrupt(5));
        unittest::assert_eq!(mie.0, (1 << 7) | (1 << 16) | (1 << 21));

        let cleared = mie.with_local_interrupt(0, false);
        unittest::assert_false!(cleared.local_interrupt(0));
        unittest::assert_true!(cleared.local_interrupt(5));
        Ok(())
    }

    #[test]
    fn mie_only_enables_timer_and_external_after_init() -> unittest::Result<()> {
        // `exceptions::early_init()` masks every `mie` source other than the
        // machine timer and machine external interrupts, including all
        // hart-local lines (bits 16+). Verify the live CSR reflects that.
        const HANDLED: usize = (1 << 7) | (1 << 11);
        unittest::assert_eq!(Mie::read().0 & !HANDLED, 0);
        Ok(())
    }
}
