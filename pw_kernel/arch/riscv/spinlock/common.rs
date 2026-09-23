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

use core::arch::asm;

use riscv::register::*;

use super::BareSpinLock;

pub struct InterruptGuard {
    saved_interrupt_enable: bool,
}

impl InterruptGuard {
    #[inline]
    pub fn new() -> Self {
        // TODO: combine these two into single instruction
        let saved_interrupt_enable = mstatus::read().mie();
        unsafe {
            mstatus::clear_mie();
        }
        // A hardware fence rather than compiler_fence is required on RISC-V to
        // prevent the CPU from reordering memory accesses (including volatile operations) across
        // the CSR write. Acquire ordering ("fence r, rw") is sufficient because we only need to
        // prevent subsequent memory accesses from being reordered before the interrupt disable.
        unsafe {
            asm!("fence r, rw", options(nostack, preserves_flags));
        }
        Self {
            saved_interrupt_enable,
        }
    }
}

impl Drop for InterruptGuard {
    #[inline]
    fn drop(&mut self) {
        // A hardware fence rather than compiler_fence is required on RISC-V to
        // ensure all memory accesses (like releasing the spinlock) are visible before interrupts are
        // re-enabled via the CSR write. Release ordering ("fence rw, w") is sufficient because we
        // only need to ensure all prior memory writes are visible before the interrupt enable.
        unsafe {
            asm!("fence rw, w", options(nostack, preserves_flags));
        }
        if self.saved_interrupt_enable {
            unsafe {
                mstatus::set_mie();
            }
        }
    }
}

pub struct RiscVSpinLockGuard<'a> {
    lock: &'a BareSpinLock,
    _guard: InterruptGuard,
}

impl<'a> RiscVSpinLockGuard<'a> {
    #[inline(always)]
    pub(super) fn new(guard: InterruptGuard, lock: &'a BareSpinLock) -> Self {
        Self {
            lock,
            _guard: guard,
        }
    }
}

impl Drop for RiscVSpinLockGuard<'_> {
    #[inline]
    fn drop(&mut self) {
        // SAFETY: `Drop::drop` runs before struct fields are dropped, ensuring
        // `self.lock.unlock()` executes while `_guard: InterruptGuard` still
        // keeps interrupts disabled.
        unsafe {
            self.lock.unlock();
        }
    }
}
