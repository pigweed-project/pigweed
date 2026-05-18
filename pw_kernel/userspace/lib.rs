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

use pw_status::Error;
#[cfg(feature = "arch_arm_cortex_m")]
pub use userspace_macro::arm_cortex_m_entry as entry;
pub use userspace_macro::process_entry;
#[cfg(feature = "arch_riscv")]
pub use userspace_macro::riscv_entry as entry;

pub mod buffer;
pub mod syscall;
pub mod time;

//pub use iovec::{AsSlice, AsSliceMut};

#[allow(non_snake_case)]
#[unsafe(no_mangle)]
pub extern "C" fn pw_assert_HandleFailure() -> ! {
    pw_log::error!("Thread Panicking!");
    let _ = syscall::debug_shutdown(Err(Error::Unknown));
    #[allow(clippy::empty_loop)]
    loop {}
}
