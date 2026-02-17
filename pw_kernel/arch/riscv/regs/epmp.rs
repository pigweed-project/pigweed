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

use memory_config::MemoryRegionType;
use regs::rw_bool_field;

use crate::regs::pmp::{PmpCfgAddressMode, PmpCfgVal};
use crate::rw_csr_reg;

impl PmpCfgVal {
    pub const fn from_region_type(ty: MemoryRegionType, address_mode: PmpCfgAddressMode) -> Self {
        // Prepare user mode PmpCfgVals for ePMP.
        // To understand the ePMP permission bits, see section 2.1 of "PMP Enhancements
        // for memory access and execution prevention on Machine mode (Smepmp)":
        // https://raw.githubusercontent.com/riscv/riscv-tee/main/Smepmp/Smepmp.pdf
        if ty.is_executable() {
            Self(0)
                .with_r(ty.is_readable())
                .with_w(ty.is_writeable())
                .with_x(ty.is_executable())
                .with_a(address_mode)
        } else if ty.is_writeable() {
            // Read-write region shared between M/U mode.
            // RW by kernel; RW by user mode.
            Self(0).with_w(true).with_x(true).with_a(address_mode)
        } else {
            // Read-only region shared between M/U mode.
            // RW by kernel; RO by user mode.
            Self(0).with_w(true).with_a(address_mode)
        }
    }
}

#[derive(Copy, Clone, Default)]
#[repr(transparent)]
/// Machine Security Configuration Register value
pub struct MSeccfgVal(pub usize);
impl MSeccfgVal {
    rw_bool_field!(usize, mml, 0, "Machine Mode Lockdown");
    // inclusive-language: ignore
    rw_bool_field!(usize, mmwp, 1, "Machine Mode Whitelist Policy");
    rw_bool_field!(usize, rlb, 2, "Rule Locking Bypass");
}

rw_csr_reg!(
    MSeccfg,
    MSeccfgVal,
    "mseccfg",
    "Machine Security Configuration Register"
);
