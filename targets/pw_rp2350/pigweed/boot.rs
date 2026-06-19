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
use kernel::Arch as _;
use rp235x_hal as hal;
use target_common::{declare_target, TargetInterface};

#[unsafe(link_section = ".start_block")]
#[used]
pub static IMAGE_DEF: hal::block::ImageDef = hal::block::ImageDef::secure_exe();

#[unsafe(link_section = ".bi_entries")]
#[used]
pub static PICOTOOL_ENTRIES: [hal::binary_info::EntryAddr; 5] = [
    hal::binary_info::rp_program_name!(c"pigweed"),
    hal::binary_info::rp_cargo_version!(),
    hal::binary_info::rp_program_description!(c"Pigweed Kernel"),
    hal::binary_info::rp_program_url!(c"https://pigweed.dev"),
    hal::binary_info::rp_program_build_attribute!(),
];

#[unsafe(no_mangle)]
#[allow(non_snake_case)]
pub extern "C" fn pw_assert_HandleFailure() -> ! {
    use kernel::Arch as _;
    Arch::panic()
}

#[cortex_m_rt::entry]
fn main() -> ! {
    kernel::static_init_state!(static mut INIT_STATE: InitKernelState<Arch>);

    // SAFETY: `main` is only executed once, so we never generate more than one
    // `&mut` reference to `INIT_STATE`.
    #[allow(static_mut_refs)]
    kernel::main(Arch, unsafe { &mut INIT_STATE });
}

unsafe extern "C" {
    fn pw_boot_rust_entry(arg: *mut core::ffi::c_void);
}

pub struct Target {}

impl TargetInterface for Target {
    const NAME: &'static str = "Pigweed RP2350 Target Board";

    fn console_init() {
        console_backend::init();
    }

    fn main() -> ! {
        // cortex_m_rt does not run ctors, so we do it manually.
        unsafe { target_common::run_ctors() };

        // Run the entry code
        unsafe {
            pw_boot_rust_entry(core::ptr::null_mut());
        }

        loop {}
    }
}

declare_target!(Target);

// Implement the FreeRTOS delay function used in entry.rs under pw_kernel.
// Remove once we have the OSAL
#[unsafe(no_mangle)]
pub extern "C" fn vTaskDelay(x_ticks_to_delay: u32) {
    let now = Arch.now();
    let duration = kernel::Duration::from_millis(x_ticks_to_delay as i64);
    let _ = kernel::sleep_until(Arch, now + duration);
}
