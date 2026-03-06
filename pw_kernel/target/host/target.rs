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

#![no_main]

use arch_host as _;
use console_backend as _;
use kernel as _;

#[unsafe(no_mangle)]
#[allow(non_snake_case)]
pub extern "C" fn pw_assert_HandleFailure() -> ! {
    panic!()
}

#[unsafe(no_mangle)]
pub extern "C" fn main() -> core::ffi::c_int {
    #[cfg(test)]
    {
        let ret = match unittest_core::run_bare_metal_tests!() {
            unittest_core::TestsResult::AllPassed => 0,
            unittest_core::TestsResult::SomeFailed => 1,
        };
        pw_log::info!("Test runner exited with code {}", ret);
        ret
    }

    #[cfg(not(test))]
    0
}
