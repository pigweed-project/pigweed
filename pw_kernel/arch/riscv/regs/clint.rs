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

// TODO: Switch to `ClintConfigInterface` once downstream `openprot` targets
// re-export `ClintConfigInterface` from their `kernel_config` crate.
use kernel_config::{ClintTimerConfigInterface, KernelConfig, RiscVKernelConfigInterface};
use regs::{BaseAddress, rw_block_reg, rw_bool_field};

use super::mtime::{MTime, MTimeCmp};

type ClintConfig = <KernelConfig as RiscVKernelConfigInterface>::Timer;

pub type ClintMTime = MTime<{ ClintConfig::MTIME_REGISTER }>;
pub type ClintMTimeCmp = MTimeCmp<{ ClintConfig::MTIMECMP_REGISTER }>;
pub type ClintMsip = Msip<{ ClintConfig::MSIP_REGISTER }>;

pub trait ClintMsipBaseAddress: BaseAddress {}

#[derive(Copy, Clone, Default)]
#[repr(transparent)]
pub struct MsipVal(pub u32);

impl MsipVal {
    rw_bool_field!(u32, msip, 0, "Machine Software Interrupt Pending");
}

rw_block_reg!(
    MsipReg,
    MsipVal,
    u32,
    ClintMsipBaseAddress,
    0,
    "CLINT Machine Software Interrupt Pending Register"
);

/// 32-bit per-hart `msip` register array starting at `BASE_ADDR`.
pub struct Msip<const BASE_ADDR: usize>(usize);

impl<const BASE_ADDR: usize> Msip<BASE_ADDR> {
    const _ASSERT_VALID: () = {
        assert!(BASE_ADDR != 0, "MSIP_REGISTER must be non-zero");
        assert!(
            BASE_ADDR.is_multiple_of(4),
            "MSIP_REGISTER must be 4-byte aligned"
        );
    };

    /// Returns the `msip` register instance for `hart_id`.
    ///
    /// # Safety
    ///
    /// `hart_id` must be a valid physical hardware `mhartid` decoded by the
    /// platform's CLINT `msip` register block.
    #[inline]
    #[must_use]
    pub const unsafe fn for_hart(hart_id: usize) -> Self {
        let () = Self::_ASSERT_VALID;
        Self(hart_id)
    }

    #[inline]
    #[must_use]
    pub fn read(&self) -> MsipVal {
        MsipReg.read(self)
    }

    #[inline]
    pub fn write(&mut self, val: MsipVal) {
        MsipReg.write(self, val);
    }
}

impl<const BASE_ADDR: usize> BaseAddress for Msip<BASE_ADDR> {
    fn base_address(&self) -> usize {
        BASE_ADDR + 4 * self.0
    }
}

impl<const BASE_ADDR: usize> ClintMsipBaseAddress for Msip<BASE_ADDR> {}
