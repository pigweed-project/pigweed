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
    KernelConfig, KernelConfigInterface, MTimeTimerConfigInterface, RiscVKernelConfigInterface,
};
use pw_log::info;
use pw_time_core::Duration;

use super::{Clock, TimerInterface};
use crate::spinlock::InterruptGuard;

type MTimeConfig = <KernelConfig as RiscVKernelConfigInterface>::Timer;

const _: () = {
    const ALIGN: usize = core::mem::align_of::<usize>();
    assert!(
        MTimeConfig::MTIME_REGISTER.is_multiple_of(ALIGN),
        "MTIME_REGISTER must be aligned to target word size"
    );
    assert!(
        MTimeConfig::MTIMECMP_REGISTER.is_multiple_of(ALIGN),
        "MTIMECMP_REGISTER must be aligned to target word size"
    );
};

pub struct Timer;

impl TimerInterface for Timer {
    fn early_init() {
        info!("Starting monotonic timer");

        let guard = InterruptGuard::new();
        let ctrl = ptr::with_exposed_provenance_mut::<u32>(MTimeConfig::TIMER_CTRL_REGISTER);
        let intr_enable =
            ptr::with_exposed_provenance_mut::<u32>(MTimeConfig::TIMER_INTR_ENABLE_REGISTER);
        let intr_state =
            ptr::with_exposed_provenance_mut::<u32>(MTimeConfig::TIMER_INTR_STATE_REGISTER);

        // Set the compare value to the maximum before enabling the timer.
        write_mtimecmp(u64::MAX, &guard);

        // SAFETY: `MTimeConfig` timer control and interrupt registers are valid,
        // 4-byte aligned MMIO addresses.
        unsafe {
            // Clear any pending interrupt.
            intr_state.write_volatile(1);
            // Enable interrupts.
            intr_enable.write_volatile(1);
            // Start the timer.
            ctrl.write_volatile(1);
        }
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
        let reg = ptr::with_exposed_provenance::<u64>(MTimeConfig::MTIME_REGISTER);
        // SAFETY: `MTimeConfig::MTIME_REGISTER` is a valid, 8-byte aligned MMIO
        // address for the 64-bit `mtime` register.
        unsafe { reg.read_volatile() }
    }

    #[cfg(target_pointer_width = "32")]
    fn get_current_monotonic_tick() -> u64 {
        // In RV32, reading 64-bit mtime requires reading hi, lo, hi to avoid rollover.
        let base = MTimeConfig::MTIME_REGISTER;
        let mtime_lo = ptr::with_exposed_provenance::<u32>(base);
        let mtime_hi = ptr::with_exposed_provenance::<u32>(base + 4);
        loop {
            // SAFETY: `MTimeConfig::MTIME_REGISTER` is a valid, 4-byte aligned
            // MMIO address for the 64-bit `mtime` register, making 32-bit
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
        ack_timer();

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

#[cfg(target_pointer_width = "64")]
fn write_mtimecmp(value: u64, _guard: &InterruptGuard) {
    let reg = ptr::with_exposed_provenance_mut::<u64>(MTimeConfig::MTIMECMP_REGISTER);
    // SAFETY: `MTimeConfig::MTIMECMP_REGISTER` is a valid, 8-byte aligned MMIO
    // address for the 64-bit `mtimecmp` register, and local interrupts are
    // disabled via `_guard`.
    unsafe { reg.write_volatile(value) }
}

#[cfg(target_pointer_width = "32")]
fn write_mtimecmp(value: u64, _guard: &InterruptGuard) {
    // Per RISC-V Privileged Architecture Specification §3.2.1 ("Machine Timer
    // Registers (mtime and mtimecmp)"), in RV32, writing 64-bit mtimecmp
    // requires writing u32::MAX (-1) to the low half first to prevent the
    // intermediate value from being smaller than the old or new value (which
    // could spuriously trigger a timer interrupt), then writing the high half,
    // then the low half:
    // https://github.com/riscv/riscv-isa-manual/blob/main/src/priv/machine.adoc
    let base = MTimeConfig::MTIMECMP_REGISTER;
    let mtimecmp_lo = ptr::with_exposed_provenance_mut::<u32>(base);
    let mtimecmp_hi = ptr::with_exposed_provenance_mut::<u32>(base + 4);
    // SAFETY: `MTimeConfig::MTIMECMP_REGISTER` is a valid, 4-byte aligned MMIO
    // address for the 64-bit `mtimecmp` register, making 32-bit volatile
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

pub fn ack_timer() {
    let intr_state =
        ptr::with_exposed_provenance_mut::<u32>(MTimeConfig::TIMER_INTR_STATE_REGISTER);
    // SAFETY: `MTimeConfig::TIMER_INTR_STATE_REGISTER` is a valid, 4-byte
    // aligned MMIO address.
    unsafe {
        // Clear any pending interrupt.
        intr_state.write_volatile(1);
    }
}
