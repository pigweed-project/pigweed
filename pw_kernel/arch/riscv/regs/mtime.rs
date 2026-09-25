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

use regs::{BaseAddress, ro_block_reg, rw_block_reg};

use crate::spinlock::InterruptGuard;

pub trait MTimeBaseAddress: BaseAddress {}
pub trait MTimeCmpBaseAddress: BaseAddress {}

#[cfg(target_pointer_width = "64")]
#[repr(transparent)]
struct MTimeVal(pub u64);

#[cfg(target_pointer_width = "64")]
ro_block_reg!(
    MTime64,
    MTimeVal,
    u64,
    MTimeBaseAddress,
    0,
    "Machine Timer Register (64-bit)"
);

#[cfg(target_pointer_width = "64")]
rw_block_reg!(
    MTimeCmp64,
    MTimeVal,
    u64,
    MTimeCmpBaseAddress,
    0,
    "Machine Timer Compare Register (64-bit)"
);

#[cfg(target_pointer_width = "32")]
#[repr(transparent)]
struct MTimeWordVal(pub u32);

#[cfg(target_pointer_width = "32")]
ro_block_reg!(
    MTimeLo,
    MTimeWordVal,
    u32,
    MTimeBaseAddress,
    0,
    "Machine Timer Register Low Word"
);

#[cfg(target_pointer_width = "32")]
ro_block_reg!(
    MTimeHi,
    MTimeWordVal,
    u32,
    MTimeBaseAddress,
    4,
    "Machine Timer Register High Word"
);

#[cfg(target_pointer_width = "32")]
rw_block_reg!(
    MTimeCmpLo,
    MTimeWordVal,
    u32,
    MTimeCmpBaseAddress,
    0,
    "Machine Timer Compare Register Low Word"
);

#[cfg(target_pointer_width = "32")]
rw_block_reg!(
    MTimeCmpHi,
    MTimeWordVal,
    u32,
    MTimeCmpBaseAddress,
    4,
    "Machine Timer Compare Register High Word"
);

/// 64-bit `mtime` register at `ADDR`.
pub struct MTime<const ADDR: usize>(());

impl<const ADDR: usize> MTime<ADDR> {
    const _ASSERT_ALIGNED: () = assert!(
        ADDR.is_multiple_of(core::mem::align_of::<usize>()),
        "MTIME_REGISTER must be aligned to target word size"
    );

    #[must_use]
    pub const fn new() -> Self {
        let () = Self::_ASSERT_ALIGNED;
        Self(())
    }

    #[cfg(target_pointer_width = "64")]
    #[must_use]
    pub fn read(&self) -> u64 {
        MTime64.read(self).0
    }

    #[cfg(target_pointer_width = "32")]
    #[must_use]
    pub fn read(&self) -> u64 {
        // In RV32, reading 64-bit mtime requires reading hi, lo, hi to avoid rollover.
        loop {
            let hi = MTimeHi.read(self).0;
            let lo = MTimeLo.read(self).0;
            let hi2 = MTimeHi.read(self).0;
            if hi == hi2 {
                return (u64::from(hi) << 32) | u64::from(lo);
            }
        }
    }
}

impl<const ADDR: usize> BaseAddress for MTime<ADDR> {
    fn base_address(&self) -> usize {
        ADDR
    }
}

impl<const ADDR: usize> MTimeBaseAddress for MTime<ADDR> {}

/// 64-bit `mtimecmp` register array starting at `BASE_ADDR` (indexed by `hart_id`).
pub struct MTimeCmp<const BASE_ADDR: usize>(usize);

impl<const BASE_ADDR: usize> MTimeCmp<BASE_ADDR> {
    const _ASSERT_ALIGNED: () = assert!(
        BASE_ADDR.is_multiple_of(core::mem::align_of::<usize>()),
        "MTIMECMP_REGISTER must be aligned to target word size"
    );

    #[must_use]
    pub const fn hart0() -> Self {
        let () = Self::_ASSERT_ALIGNED;
        Self(0)
    }

    #[cfg(target_pointer_width = "64")]
    pub fn write(&mut self, value: u64, _guard: &InterruptGuard) {
        MTimeCmp64.write(self, MTimeVal(value));
    }

    #[cfg(target_pointer_width = "32")]
    pub fn write(&mut self, value: u64, _guard: &InterruptGuard) {
        // Per RISC-V Privileged Architecture Specification §3.2.1 ("Machine Timer
        // Registers (mtime and mtimecmp)"), in RV32, writing 64-bit mtimecmp
        // requires writing u32::MAX (-1) to the low half first to prevent the
        // intermediate value from being smaller than the old or new value (which
        // could spuriously trigger a timer interrupt), then writing the high half,
        // then the low half:
        // https://github.com/riscv/riscv-isa-manual/blob/main/src/priv/machine.adoc
        #[allow(clippy::cast_possible_truncation)]
        let (hi, lo) = ((value >> 32) as u32, value as u32);
        MTimeCmpLo.write(self, MTimeWordVal(u32::MAX));
        MTimeCmpHi.write(self, MTimeWordVal(hi));
        MTimeCmpLo.write(self, MTimeWordVal(lo));
    }

    #[cfg(target_pointer_width = "64")]
    pub fn disarm(&mut self, _guard: &InterruptGuard) {
        MTimeCmp64.write(self, MTimeVal(u64::MAX));
    }

    #[cfg(target_pointer_width = "32")]
    pub fn disarm(&mut self, _guard: &InterruptGuard) {
        // Writing `MTimeCmpHi` to `u32::MAX` first ensures the intermediate
        // 64-bit compare value is never smaller than the previous compare value.
        MTimeCmpHi.write(self, MTimeWordVal(u32::MAX));
        MTimeCmpLo.write(self, MTimeWordVal(u32::MAX));
    }
}

impl<const BASE_ADDR: usize> BaseAddress for MTimeCmp<BASE_ADDR> {
    fn base_address(&self) -> usize {
        BASE_ADDR + 8 * self.0
    }
}

impl<const BASE_ADDR: usize> MTimeCmpBaseAddress for MTimeCmp<BASE_ADDR> {}

#[cfg(feature = "timer_mtime")]
pub use mtime_regs::*;

#[cfg(feature = "timer_mtime")]
mod mtime_regs {
    use kernel_config::{KernelConfig, MTimeTimerConfigInterface, RiscVKernelConfigInterface};
    use regs::{rw_bool_field, rw_reg};

    use super::{MTime, MTimeCmp};

    type MTimeConfig = <KernelConfig as RiscVKernelConfigInterface>::Timer;

    pub type TimerMTime = MTime<{ MTimeConfig::MTIME_REGISTER }>;
    pub type TimerMTimeCmp = MTimeCmp<{ MTimeConfig::MTIMECMP_REGISTER }>;

    #[derive(Copy, Clone, Default)]
    #[repr(transparent)]
    pub struct TimerCtrlVal(pub u32);

    impl TimerCtrlVal {
        rw_bool_field!(u32, active, 0, "Timer active");
    }

    rw_reg!(
        TimerCtrl,
        TimerCtrlVal,
        u32,
        MTimeConfig::TIMER_CTRL_REGISTER,
        "Timer Control Register"
    );

    #[derive(Copy, Clone, Default)]
    #[repr(transparent)]
    pub struct TimerIntrEnableVal(pub u32);

    impl TimerIntrEnableVal {
        rw_bool_field!(u32, ie, 0, "Interrupt enable");
    }

    rw_reg!(
        TimerIntrEnable,
        TimerIntrEnableVal,
        u32,
        MTimeConfig::TIMER_INTR_ENABLE_REGISTER,
        "Timer Interrupt Enable Register"
    );

    #[derive(Copy, Clone, Default)]
    #[repr(transparent)]
    pub struct TimerIntrStateVal(pub u32);

    impl TimerIntrStateVal {
        rw_bool_field!(u32, is, 0, "Interrupt state (write 1 to clear)");
    }

    rw_reg!(
        TimerIntrState,
        TimerIntrStateVal,
        u32,
        MTimeConfig::TIMER_INTR_STATE_REGISTER,
        "Timer Interrupt State Register"
    );
}
