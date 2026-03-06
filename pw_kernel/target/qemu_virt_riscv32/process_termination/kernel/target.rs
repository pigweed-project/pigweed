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
#![no_std]
#![no_main]

use arch_riscv::Arch;
use codegen as _;
use console_backend as _;
use entry as _;
use process_termination::{TestState, test_main};
use riscv_semihosting::debug::{EXIT_FAILURE, EXIT_SUCCESS, exit};
use target_common::{TargetInterface, declare_target};

pub struct Target {}
impl TargetInterface for Target {
    const NAME: &'static str = "QEMU-VIRT-RISCV Kernel Process Termination";

    fn main() -> ! {
        static mut TEST_STATE: TestState<Arch> = TestState::new(Arch);

        // SAFETY: `test_main` is only executed once, so we never generate more
        // than one `&mut` reference to `TEST_STATE`.
        #[allow(static_mut_refs)]
        let exit_status = match test_main::<Arch>(Arch, unsafe { &mut TEST_STATE }) {
            Ok(()) => EXIT_SUCCESS,
            Err(_e) => EXIT_FAILURE,
        };
        exit(exit_status);
        #[allow(clippy::empty_loop)]
        loop {}
    }
}

declare_target!(Target);
