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

use regs::{rw_bool_field, rw_int_field};

use crate::rw_csr_reg;

/// Machine Internal Timer Registers (VeeR)
#[derive(Copy, Clone, Default)]
#[repr(transparent)]
pub struct MitCntVal(pub usize);

rw_csr_reg!(
    MitCnt0,
    MitCntVal,
    0x7d2,
    "Machine Internal Timer Counter 0"
);
rw_csr_reg!(
    MitCnt1,
    MitCntVal,
    0x7d5,
    "Machine Internal Timer Counter 1"
);

/// Machine Internal Timer Bound Registers (VeeR)
#[derive(Copy, Clone, Default)]
#[repr(transparent)]
pub struct MitBoundVal(pub usize);

rw_csr_reg!(
    MitBound0,
    MitBoundVal,
    0x7d3,
    "Machine Internal Timer Bound 0"
);
rw_csr_reg!(
    MitBound1,
    MitBoundVal,
    0x7d6,
    "Machine Internal Timer Bound 1"
);

/// Machine Internal Timer Control Registers (VeeR)
#[derive(Copy, Clone, Default)]
#[repr(transparent)]
pub struct MitCtlVal(pub usize);

impl MitCtlVal {
    rw_bool_field!(usize, enable, 0, "timer increment enable");
    rw_bool_field!(usize, halt_en, 1, "timer increment while in sleep");
    rw_bool_field!(usize, pause_en, 2, "timer increment while executing PAUSE");
}

rw_csr_reg!(
    MitCtl0,
    MitCtlVal,
    0x7d4,
    "Machine Internal Timer Control 0"
);
rw_csr_reg!(
    MitCtl1,
    MitCtlVal,
    0x7d7,
    "Machine Internal Timer Control 1"
);

/// Machine external interrupt vector table
#[derive(Copy, Clone, Default)]
#[repr(transparent)]
pub struct MeiVTVal(pub usize);

rw_csr_reg!(
    MeiVT,
    MeiVTVal,
    0xbc8,
    "Machine External Interrupt Vector Table"
);

/// Machine external interrupt priority threshold
#[derive(Copy, Clone, Default)]
#[repr(transparent)]
pub struct MeiPTVal(pub usize);
impl MeiPTVal {
    rw_int_field!(usize, prithresh, 0, 3, u8, "threshold");
}

rw_csr_reg!(
    MeiPT,
    MeiPTVal,
    0xbc9,
    "Machine External Interrupt Priority Threshold"
);

/// Machine external interrupt Claim ID Priority Level Capture Trigger
#[derive(Copy, Clone, Default)]
#[repr(transparent)]
pub struct MeiCPCTVal(pub usize);

rw_csr_reg!(
    MeiCPCT,
    MeiCPCTVal,
    0xbca,
    "Machine External Interrupt Claim ID Level Capture Trigger"
);

/// Machine external interrupt Claim ID Priority Level
#[derive(Copy, Clone, Default)]
#[repr(transparent)]
pub struct MeiCIDPLVal(pub usize);
impl MeiCIDPLVal {
    rw_int_field!(
        usize,
        clidpri,
        0,
        3,
        u8,
        "priority of external interrupt source"
    );
}

rw_csr_reg!(
    MeiCIDPL,
    MeiCIDPLVal,
    0xbcb,
    "Machine External Interrupt Claim ID Priority Level"
);

/// Machine external interrupt Current Priority Level
#[derive(Copy, Clone, Default)]
#[repr(transparent)]
pub struct MeiCURPLVal(pub usize);
impl MeiCURPLVal {
    rw_int_field!(
        usize,
        currpri,
        0,
        3,
        u8,
        "priority of current interrupt service routine"
    );
}

rw_csr_reg!(
    MeiCURPL,
    MeiCURPLVal,
    0xbcc,
    "Machine External Current Priority Level"
);

/// Machine external interrupt handler address pointer
#[derive(Copy, Clone, Default)]
#[repr(transparent)]
pub struct MeiHAPVal(pub usize);
impl MeiHAPVal {
    rw_int_field!(usize, claimid, 2, 9, u8, "external interrupt source id");
    rw_int_field!(
        usize,
        base,
        10,
        31,
        usize,
        "external interrupt vector table base"
    );
}

rw_csr_reg!(
    MeiHAP,
    MeiHAPVal,
    0xfc8,
    "Machine External Interrupt Handler Address Pointer"
);
