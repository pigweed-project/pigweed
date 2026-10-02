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

//! ARM Cortex-M Special-Purpose Registers (`MRS` / `MSR`).

use core::arch::asm;

use pw_cast::CastFrom as _;
use regs::*;

/// Stack-pointer selection
#[repr(u32)]
pub enum Spsel {
    Main = 0,
    Process = 1,
}

macro_rules! rw_msr_reg {
    ($name:ident, $val_type:ident, $reg_name:ident, $doc:literal) => {
        #[doc=$doc]
        pub struct $name;
        impl $name {
            #[inline]
            pub fn read() -> $val_type {
                let mut val: usize;
                // `nostack` and `preserves_flags` are set because `mrs` does
                // not push data to the stack or modify condition flags (APSR).
                // `nomem` is intentionally omitted so the inline assembly acts
                // as a compiler memory barrier around special register accesses.
                unsafe {
                    asm!(
                        concat!("mrs {0}, ", stringify!($reg_name)),
                        out(reg) val,
                        options(nostack, preserves_flags),
                    )
                };
                $val_type(u32::cast_from(val))
            }

            #[inline]
            pub fn write(val: $val_type) {
                // `nostack` and `preserves_flags` are set because `msr` does
                // not push data to the stack or modify condition flags (APSR).
                // `nomem` is intentionally omitted so the inline assembly acts
                // as a compiler memory barrier, preventing stack or
                // privilege-sensitive memory operations from being reordered
                // across writes to registers like `CONTROL`, `MSPLIM`, or
                // `PSPLIM`.
                unsafe {
                    asm!(
                        concat!("msr ", stringify!($reg_name), ", {0}"),
                        in(reg) val.0,
                        options(nostack, preserves_flags),
                    )
                };
            }
        }
    };
}

/// CONTROL register value.
///
/// ARMv7-M (DDI 0403E.e §B1.4.4): bits 0-2 only; bits 31:3 reserved RAZ/WI.
/// ARMv8-M (DDI 0553 §B3.1.4): bits 0-7; adds TrustZone, BTI, and PAC fields.
#[derive(Copy, Clone, Default)]
#[repr(transparent)]
pub struct ControlVal(pub u32);

impl ControlVal {
    rw_bool_field!(u32, npriv, 0, "non privileged");
    rw_enum_field!(u32, spsel, 1, 1, Spsel, "stack-pointer select");
    rw_bool_field!(u32, fpca, 2, "floating-point context active");
    #[cfg(feature = "armv8m")]
    rw_bool_field!(u32, sfpa, 3, "secure floating-point active");
    #[cfg(feature = "armv8m")]
    rw_bool_field!(
        u32,
        bti_en,
        4,
        "privileged branch target identification enable"
    );
    #[cfg(feature = "armv8m")]
    rw_bool_field!(
        u32,
        ubti_en,
        5,
        "un-privileged branch target identification enable"
    );
    #[cfg(feature = "armv8m")]
    rw_bool_field!(u32, pac_en, 6, "privileged pointer authentication enable");
    #[cfg(feature = "armv8m")]
    rw_bool_field!(
        u32,
        upac_en,
        7,
        "un-privileged pointer authentication enable"
    );
}

rw_msr_reg!(Control, ControlVal, control, "Control Register");

#[cfg(feature = "armv8m")]
#[derive(Copy, Clone, Default)]
#[repr(transparent)]
pub struct MsplimVal(pub u32);

#[cfg(feature = "armv8m")]
rw_msr_reg!(
    Msplim,
    MsplimVal,
    msplim,
    "Main Stack Pointer Limit Register"
);

#[cfg(feature = "armv8m")]
#[derive(Copy, Clone, Default)]
#[repr(transparent)]
pub struct PsplimVal(pub u32);

#[cfg(feature = "armv8m")]
rw_msr_reg!(
    Psplim,
    PsplimVal,
    psplim,
    "Process Stack Pointer Limit Register"
);
