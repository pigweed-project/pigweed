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
use console_backend as _;
#[cfg(feature = "qemu")]
use cortex_m_semihosting::debug::{EXIT_FAILURE, EXIT_SUCCESS, exit};
use entry as _;
use target_common::{TargetInterface, declare_target};

pub struct Target {}

impl TargetInterface for Target {
    const NAME: &'static str = "Nucleo-F103RB Kernel Thread Termination";

    #[cfg(feature = "board")]
    fn console_init() {
        // SAFETY: Called once at boot before kernel initialization.
        unsafe { clock::clock_init() };
        console_backend::init();
    }

    fn main() -> ! {
        static mut TEST_STATE: thread_termination::TestState<Arch> =
            thread_termination::TestState::new(Arch);
        // SAFETY: `test_main` is only executed once, so we never generate more
        // than one `&mut` reference to `TEST_STATE`.
        #[expect(static_mut_refs)]
        let result = thread_termination::test_main::<Arch>(Arch, unsafe { &mut TEST_STATE });

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

declare_target!(Target);
