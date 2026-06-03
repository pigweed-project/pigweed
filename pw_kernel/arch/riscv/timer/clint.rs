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

use core::ptr::{with_exposed_provenance, with_exposed_provenance_mut};

use kernel_config::{
    ClintTimerConfigInterface, KernelConfig, KernelConfigInterface, RiscVKernelConfigInterface,
};
use pw_log::info;
use time::Duration;

use crate::spinlock::InterruptGuard;
use crate::timer::{Clock, TimerInterface};

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
        unsafe {
            riscv::register::mie::set_mtimer();
        }
    }

    fn disable() {
        unsafe {
            // Simply disable the interrupt.
            riscv::register::mie::clear_mtimer();
        }
    }

    fn get_current_monotonic_tick() -> u64 {
        // TODO: make sure this is 32bit safe by reading high and low parts separately.
        let reg = with_exposed_provenance::<u64>(
            <KernelConfig as RiscVKernelConfigInterface>::Timer::MTIME_REGISTER,
        );
        unsafe { reg.read_volatile() }
    }

    #[inline(never)]
    fn set_next_monotonic_tick() {
        let _guard = InterruptGuard::new();
        let now = Self::get_current_monotonic_tick();

        let ticks_per_monotonic: Duration<Clock> =
            time::Duration::from_millis((1000 / KernelConfig::SCHEDULER_TICK_HZ).into());
        // safe to cast to u64 as ticks_per_monotonic will never be negative here,
        let next = now.checked_add(ticks_per_monotonic.ticks().cast_unsigned());
        if let Some(val) = next {
            write_mtimecmp(val);
        } else {
            pw_assert::debug_panic!("Next monotonic tick overflow");
        }
    }
}

fn write_mtimecmp(value: u64) {
    // TODO: make sure this is 32bit safe by writing high and low parts separately.
    let reg = with_exposed_provenance_mut::<u64>(
        <KernelConfig as RiscVKernelConfigInterface>::Timer::MTIMECMP_REGISTER,
    );
    unsafe { reg.write_volatile(value) }
}
