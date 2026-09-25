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
use crate::regs::mtime::{
    TimerCtrl, TimerCtrlVal, TimerIntrEnable, TimerIntrEnableVal, TimerIntrState,
    TimerIntrStateVal, TimerMTime, TimerMTimeCmp,
};
use crate::spinlock::InterruptGuard;

pub struct Timer;

impl TimerInterface for Timer {
    fn early_init() {
        info!("Starting monotonic timer");

        let guard = InterruptGuard::new();

        // Set the compare value to the maximum before enabling the timer.
        TimerMTimeCmp::hart0().disarm(&guard);

        // Clear any pending interrupt.
        TimerIntrState.write(TimerIntrStateVal::default().with_is(true));
        // Enable interrupts.
        TimerIntrEnable.write(TimerIntrEnableVal::default().with_ie(true));
        // Start the timer.
        TimerCtrl.write(TimerCtrlVal::default().with_active(true));

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
        TimerMTime::new().read()
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
            TimerMTimeCmp::hart0().write(val, &guard);
        } else {
            pw_assert::debug_panic!("Next monotonic tick overflow");
        }
    }
}

pub fn ack_timer() {
    // Clear any pending interrupt.
    TimerIntrState.write(TimerIntrStateVal::default().with_is(true));
}
