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

use kernel_config::{KernelConfig, KernelConfigInterface};
use pw_log::info;
use pw_time_core::Duration;

use super::{Clock, TimerInterface};
use crate::regs::clint::{ClintMTime, ClintMTimeCmp};
#[cfg(feature = "smp")]
use crate::regs::clint::{ClintMsip, MsipVal};
use crate::spinlock::InterruptGuard;

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

    fn get_current_monotonic_tick() -> u64 {
        ClintMTime::new().read()
    }

    #[inline(never)]
    fn set_next_monotonic_tick() {
        let guard = InterruptGuard::new();
        let now = Self::get_current_monotonic_tick();

        let ticks_per_monotonic: Duration<Clock> =
            Duration::from_millis((1000 / KernelConfig::SCHEDULER_TICK_HZ).into());
        let next = now.checked_add(ticks_per_monotonic.ticks());
        if let Some(val) = next {
            ClintMTimeCmp::hart0().write(val, &guard);
        } else {
            pw_assert::debug_panic!("Next monotonic tick overflow");
        }
    }
}

/// Triggers a Machine Software Interrupt (IPI) on `target_hart` via its CLINT
/// `msip` register.
///
/// # Safety
///
/// `target_hart` must be a valid physical hardware `mhartid` decoded by the
/// platform's CLINT `msip` register block.
#[cfg(feature = "smp")]
#[expect(dead_code)]
#[inline]
pub(crate) unsafe fn send_ipi(target_hart: usize) {
    // SAFETY: Caller guarantees `target_hart` is a valid hardware `mhartid` for
    // this CLINT.
    let mut msip = unsafe { ClintMsip::for_hart(target_hart) };
    msip.write(MsipVal::default().with_msip(true));
}

/// Clears a pending Machine Software Interrupt (IPI) for `hart_id` via its
/// CLINT `msip` register.
///
/// # Safety
///
/// `hart_id` must be a valid physical hardware `mhartid` decoded by the
/// platform's CLINT `msip` register block.
#[cfg(feature = "smp")]
#[expect(dead_code)]
#[inline]
pub(crate) unsafe fn clear_ipi(hart_id: usize) {
    // SAFETY: Caller guarantees `hart_id` is a valid hardware `mhartid` for
    // this CLINT.
    let mut msip = unsafe { ClintMsip::for_hart(hart_id) };
    msip.write(MsipVal::default().with_msip(false));
}

/// Disarms `hart_id`'s CLINT `mtimecmp` compare register by setting it to
/// `u64::MAX`.
///
/// # Safety
///
/// - `hart_id` must be a valid physical hardware `mhartid` decoded by the
///   platform's CLINT `mtimecmp` register block.
/// - The caller must ensure exclusive access to `hart_id`'s `mtimecmp` register
///   (e.g., `hart_id` is the calling hart, or `hart_id` is halted / not yet
///   booted), since `InterruptGuard::new()` only masks interrupts on the
///   calling hart.
#[cfg(feature = "smp")]
#[expect(dead_code)]
pub(crate) unsafe fn disarm_mtimecmp(hart_id: usize) {
    let guard = InterruptGuard::new();
    // SAFETY: Caller guarantees `hart_id` is a valid hardware `mhartid` for
    // this CLINT and that the caller has exclusive access to `hart_id`'s
    // `mtimecmp` register.
    let mut mtimecmp = unsafe { ClintMTimeCmp::for_hart(hart_id) };
    mtimecmp.disarm(&guard);
}
