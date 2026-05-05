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
use entry as _;
use target_common::{TargetInterface, declare_target};

pub struct Target {}

// make sure this IRQ matches the value
// defined in the system.json5 file.
const TEST_IRQ: u32 = 46;

impl TargetInterface for Target {
    const NAME: &'static str = "RP2350 Kernel Interrupts";

    fn console_init() {
        console_backend::init();
    }

    fn main() -> ! {
        let _ = test_interrupts::main::<Arch>(Arch, TEST_IRQ);
        #[expect(clippy::empty_loop)]
        loop {}
    }
}

codegen::declare_kernel_interrupt_handlers!();
declare_target!(Target);
