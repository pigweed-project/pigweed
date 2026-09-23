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

use core::ptr;

use kernel_config::{
    ClintTimerConfigInterface, KernelConfig, KernelConfigInterface, RiscVKernelConfigInterface,
};
use pw_log::info;
use pw_time_core::Duration;

use super::{Clock, TimerInterface};
use crate::spinlock::InterruptGuard;

type ClintConfig = <KernelConfig as RiscVKernelConfigInterface>::Timer;

const _: () = {
    const ALIGN: usize = core::mem::align_of::<usize>();
    assert!(
        ClintConfig::MTIME_REGISTER.is_multiple_of(ALIGN),
        "MTIME_REGISTER must be aligned to target word size"
    );
    assert!(
        ClintConfig::MTIMECMP_REGISTER.is_multiple_of(ALIGN),
        "MTIMECMP_REGISTER must be aligned to target word size"
    );
};

pub struct Timer;

impl TimerInterface for Timer {
    fn early_init() {
        info!("Starting monotonic timer");
        Self::disable();
        Self::set_next_monotonic_tick();
    }

    fn init() {
        Self::enable();
    }

    fn enable() {
        // SAFETY: Enabling the machine timer interrupt bit (`mie.mtie`) is safe
        // in M-mode.
        unsafe {
            riscv::register::mie::set_mtimer();
        }
    }

    fn disable() {
        // SAFETY: Clearing the machine timer interrupt bit (`mie.mtie`) is safe
        // in M-mode.
        unsafe {
            // Simply disable the interrupt.
            riscv::register::mie::clear_mtimer();
        }
    }

    #[cfg(target_pointer_width = "64")]
    fn get_current_monotonic_tick() -> u64 {
        let reg = ptr::with_exposed_provenance::<u64>(ClintConfig::MTIME_REGISTER);
        // SAFETY: `ClintConfig::MTIME_REGISTER` is a valid, 8-byte aligned MMIO
        // address for the 64-bit CLINT `mtime` register.
        unsafe { reg.read_volatile() }
    }

    #[cfg(target_pointer_width = "32")]
    fn get_current_monotonic_tick() -> u64 {
        // In RV32, reading 64-bit mtime requires reading hi, lo, hi to avoid rollover.
        let base = ClintConfig::MTIME_REGISTER;
        let mtime_lo = ptr::with_exposed_provenance::<u32>(base);
        let mtime_hi = ptr::with_exposed_provenance::<u32>(base + 4);
        loop {
            // SAFETY: `ClintConfig::MTIME_REGISTER` is a valid, 4-byte aligned
            // MMIO address for the 64-bit CLINT `mtime` register, making 32-bit
            // volatile reads at `base` and `base + 4` valid and 4-byte aligned.
            let (hi, lo, hi2) = unsafe {
                let hi = mtime_hi.read_volatile();
                let lo = mtime_lo.read_volatile();
                let hi2 = mtime_hi.read_volatile();
                (hi, lo, hi2)
            };
            if hi == hi2 {
                return (u64::from(hi) << 32) | u64::from(lo);
            }
        }
    }

    #[inline(never)]
    fn set_next_monotonic_tick() {
        let guard = InterruptGuard::new();
        let now = Self::get_current_monotonic_tick();

        let ticks_per_monotonic: Duration<Clock> =
            Duration::from_millis((1000 / KernelConfig::SCHEDULER_TICK_HZ).into());
        let next = now.checked_add(ticks_per_monotonic.ticks());
        if let Some(val) = next {
            write_mtimecmp(val, &guard);
        } else {
            pw_assert::debug_panic!("Next monotonic tick overflow");
        }
    }
}

// Writes to Hart 0's `mtimecmp` register (`ClintConfig::MTIMECMP_REGISTER`).
//
// Requires an `InterruptGuard` so a timer ISR cannot preempt the update
// mid-sequence, and must only be called from the hart owning
// `MTIMECMP_REGISTER` (Hart 0).
#[cfg(target_pointer_width = "64")]
fn write_mtimecmp(value: u64, _guard: &InterruptGuard) {
    let reg = ptr::with_exposed_provenance_mut::<u64>(ClintConfig::MTIMECMP_REGISTER);
    // SAFETY: `ClintConfig::MTIMECMP_REGISTER` is a valid, 8-byte aligned MMIO
    // address for the 64-bit CLINT `mtimecmp` register, and local interrupts
    // are disabled via `_guard`.
    unsafe { reg.write_volatile(value) }
}

// Writes to Hart 0's `mtimecmp` register (`ClintConfig::MTIMECMP_REGISTER`).
//
// Requires an `InterruptGuard` so a timer ISR cannot preempt the 3-step RV32
// write sequence mid-update, and must only be called from the hart owning
// `MTIMECMP_REGISTER` (Hart 0).
#[cfg(target_pointer_width = "32")]
fn write_mtimecmp(value: u64, _guard: &InterruptGuard) {
    // Per RISC-V Privileged Architecture Specification §3.2.1 ("Machine Timer
    // Registers (mtime and mtimecmp)"), in RV32, writing 64-bit mtimecmp
    // requires writing u32::MAX (-1) to the low half first to prevent the
    // intermediate value from being smaller than the old or new value (which
    // could spuriously trigger a timer interrupt), then writing the high half,
    // then the low half:
    // https://github.com/riscv/riscv-isa-manual/blob/main/src/priv/machine.adoc
    let base = ClintConfig::MTIMECMP_REGISTER;
    let mtimecmp_lo = ptr::with_exposed_provenance_mut::<u32>(base);
    let mtimecmp_hi = ptr::with_exposed_provenance_mut::<u32>(base + 4);
    // SAFETY: `ClintConfig::MTIMECMP_REGISTER` is a valid, 4-byte aligned MMIO
    // address for the 64-bit CLINT `mtimecmp` register, making 32-bit volatile
    // writes at `base` and `base + 4` valid and 4-byte aligned. Local
    // interrupts are disabled via `_guard`, preventing ISR reentrancy across
    // the three writes.
    #[allow(clippy::cast_possible_truncation)]
    unsafe {
        mtimecmp_lo.write_volatile(u32::MAX);
        mtimecmp_hi.write_volatile((value >> 32) as u32);
        mtimecmp_lo.write_volatile(value as u32);
    }
}
