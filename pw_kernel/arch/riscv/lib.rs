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

use kernel::KernelState;

#[cfg(feature = "disable_interrupts_atomic")]
mod disable_interrupts_atomic;
mod exceptions;
#[cfg(not(feature = "veer_pic"))]
mod plic;
mod protection;
pub mod regs;
mod spinlock;
mod threads;
mod timer;
#[cfg(feature = "veer_pic")]
mod veer_pic;

#[cfg(not(feature = "veer_pic"))]
pub type InterruptController = plic::Plic;
#[cfg(feature = "veer_pic")]
pub type InterruptController = veer_pic::VeerPic;

// Re-exports to conform to simplify public API.
pub use protection::MemoryConfig;
pub use spinlock::BareSpinLock;
pub use threads::ArchThreadState;

#[derive(Copy, Clone, Default)]
pub struct Arch;

kernel::impl_thread_arg_for_default_zst!(Arch);

impl kernel::Kernel for Arch {
    fn get_state(self) -> &'static KernelState<Arch> {
        static STATE: KernelState<Arch> =
            KernelState::new(kernel::ArchState::new(InterruptController::new()));
        &STATE
    }
}
