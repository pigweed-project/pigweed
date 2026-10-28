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

use kernel::Kernel;
use kernel::interrupt_controller::{InterruptController, InterruptGuard};
use kernel_config::{NvicConfig, NvicConfigInterface};
use log_if::debug_if;
use pw_log::info;

use crate::{regs, threads};

const LOG_INTERRUPTS: bool = false;

pub struct Nvic {}

impl Nvic {
    #[must_use]
    pub const fn new() -> Self {
        Self {}
    }
}

impl InterruptController for Nvic {
    type InterruptHandler = unsafe extern "C" fn();

    fn early_init(&self) {
        info!("Initializing NVIC");
        let mut nvic_regs = regs::Nvic {};
        // Disable and clear any pending state on every external IRQ. The NVIC
        // enable/pending registers are not architecturally reset on a warm
        // reset, so a bootloader or prior stage may have left interrupts armed
        // that would otherwise fire as soon as the kernel enables interrupts.
        // It is up to the kernel driver or interrupt object to enable the
        // interrupts it services.
        //
        // Also set all of the NVIC external IRQs to the same priority as
        // SysTick.
        for i in 0..NvicConfig::MAX_IRQS {
            nvic_regs.disable(i as usize);
            nvic_regs.clear_pending(i as usize);
            nvic_regs.set_priority(i as usize, threads::InterruptPriority::Interrupt as u8);
        }
    }

    fn enable_interrupt(irq: u32) {
        debug_if!(LOG_INTERRUPTS, "Enable interrupt {}", irq as u32);
        let mut nvic_regs = regs::Nvic {};
        nvic_regs.enable(irq as usize);
    }

    fn disable_interrupt(irq: u32) {
        debug_if!(LOG_INTERRUPTS, "Disable interrupt {}", irq as u32);
        let mut nvic_regs = regs::Nvic {};
        nvic_regs.disable(irq as usize);
    }

    fn userspace_interrupt_ack(irq: u32) {
        debug_if!(
            LOG_INTERRUPTS,
            "Userspace interrupt {} handler ack",
            irq as u32
        );
        // re-enable the interrupt to allow it to be triggered again.
        Self::enable_interrupt(irq);
    }

    fn userspace_interrupt_handler_enter<K: Kernel>(_kernel: K, irq: u32) {
        debug_if!(
            LOG_INTERRUPTS,
            "Userspace interrupt {} handler enter",
            irq as u32
        );
        Self::disable_interrupt(irq);
    }

    fn userspace_interrupt_handler_exit<K: Kernel>(
        _kernel: K,
        irq: u32,
        _guard: InterruptGuard<K>,
    ) {
        debug_if!(
            LOG_INTERRUPTS,
            "Userspace interrupt {} handler exit",
            irq as u32
        );
    }

    fn kernel_interrupt_handler_enter<K: Kernel>(_kernel: K, irq: u32) {
        debug_if!(
            LOG_INTERRUPTS,
            "Kernel interrupt {} handler enter",
            irq as u32
        );
    }

    fn kernel_interrupt_handler_exit<K: Kernel>(_kernel: K, irq: u32, _guard: InterruptGuard<K>) {
        debug_if!(
            LOG_INTERRUPTS,
            "Kernel interrupt {} handler exit",
            irq as u32
        );
        // No need to unmask the interrupt, as the NVIC will automatically
        // unmask the interrupt when the handler exits.
    }

    fn enable_interrupts() {
        debug_if!(LOG_INTERRUPTS, "Enable interrupts");
        unsafe {
            cortex_m::interrupt::enable();
        }
    }

    fn disable_interrupts() {
        debug_if!(LOG_INTERRUPTS, "Disable interrupts");
        cortex_m::interrupt::disable();
    }

    fn interrupts_enabled() -> bool {
        // It's a complicated concept in cortex-m:
        // If PRIMASK is inactive, then interrupts are 100% disabled otherwise
        // if the current interrupt priority level is not zero (BASEPRI register) interrupts
        // at that level are not allowed. For now we're treating nonzero as full disabled.
        let primask = cortex_m::register::primask::read();
        let basepri = cortex_m::register::basepri::read();
        debug_if!(
            LOG_INTERRUPTS,
            "Interrupts enabled: PRIMASK={}, BASEPRI={}",
            primask as u32,
            basepri as u8,
        );
        primask.is_active() && (basepri == 0)
    }

    fn trigger_interrupt(irq: u32) {
        debug_if!(LOG_INTERRUPTS, "Trigger interrupt {}", irq as u32);
        let mut nvic_regs = regs::Nvic {};
        nvic_regs.set_pending(irq as usize);
    }
}
