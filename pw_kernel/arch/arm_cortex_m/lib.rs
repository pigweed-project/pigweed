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

use core::arch::asm;

use kernel::{Kernel, KernelState};
use pw_log::info;

mod exceptions;
mod nvic;
mod protection;
mod regs;
mod spinlock;
#[cfg(feature = "user_space")]
mod syscall;
mod threads;
mod timer;

// Re-exports to conform to simplify public API.
pub use protection::MemoryConfig;
pub use spinlock::BareSpinLock;
pub use threads::ArchThreadState;

#[derive(Copy, Clone, Default)]
pub struct Arch;

kernel::impl_thread_arg_for_default_zst!(Arch);

impl Kernel for Arch {
    fn get_state(self) -> &'static KernelState<Arch> {
        static STATE: KernelState<Arch> =
            KernelState::new(kernel::ArchState::new(nvic::Nvic::new()));
        kernel::annotate_kernel_state!(STATE);
        &STATE
    }
}

fn ipsr_register_read() -> u32 {
    let ipsr: u32;
    // Note: cortex-m crate does not implement this register for some reason, so
    // read and mask manually
    unsafe {
        asm!("mrs {ipsr}, ipsr", ipsr = out(reg) ipsr);
    }
    ipsr
}

// Utility function to read whether or not the cpu considers itself in a handler
fn in_interrupt_handler() -> bool {
    // IPSR[8:0] is the current exception handler (or 0 if in thread mode)
    // TODO: konkers - Create register wrapper for IPSR.
    let current_exception = ipsr_register_read() & 0x1ff;
    current_exception != 0
}

#[allow(dead_code)]
fn dump_int_pri() {
    let basepri = cortex_m::register::basepri::read();
    let primask: u8 = cortex_m::register::primask::read().is_active().into();
    info!("BASEPRI={}, PRIMASK={}", basepri as u8, primask as u8);
}
