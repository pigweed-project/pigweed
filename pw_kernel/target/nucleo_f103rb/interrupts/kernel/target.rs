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
#![no_std]
#![no_main]

use arch_arm_cortex_m::Arch;
use codegen as _;
use console_backend as _;
#[cfg(feature = "qemu")]
use cortex_m_semihosting::debug::{EXIT_FAILURE, EXIT_SUCCESS, exit};
use entry as _;
use target_common::{TargetInterface, declare_target};

pub struct Target {}

// Must match the IRQ number in system.json5 / system_qemu.json5.
//
// Real hardware (STM32F103RB) implements 43 NVIC lines (0-42); IRQ 42
// (USBWakeUp) is spare/unused on this board and safe for a software-only
// trigger test.
#[cfg(feature = "board")]
const TEST_IRQ: u32 = 42;
// QEMU's mps2-an385 machine model only implements 32 external NVIC lines
// (0-31); IRQs >= 32 accept enable_interrupt()/trigger_interrupt() calls
// but the pending bit never fires. Verified empirically (bisected 31 pass,
// 32 fail). Not fixable from kernel code; it's a QEMU machine-model limit.
#[cfg(feature = "qemu")]
const TEST_IRQ: u32 = 31;

impl TargetInterface for Target {
    const NAME: &'static str = "Nucleo-F103RB Kernel Interrupts";

    #[cfg(feature = "board")]
    fn console_init() {
        // SAFETY: Called once at boot before kernel initialization.
        unsafe { clock::clock_init() };
        console_backend::init();
    }

    fn main() -> ! {
        let result = test_interrupts::main::<Arch>(Arch, TEST_IRQ);

        #[cfg(feature = "qemu")]
        {
            let status = match result {
                Ok(()) => EXIT_SUCCESS,
                Err(_) => EXIT_FAILURE,
            };
            exit(status);
        }

        #[cfg(feature = "board")]
        {
            let _ = result;
        }

        #[expect(clippy::empty_loop)]
        loop {}
    }
}

codegen::declare_kernel_interrupt_handlers!();
declare_target!(Target);
