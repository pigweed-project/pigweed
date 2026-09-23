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
use core::sync::atomic::{Ordering, compiler_fence};

use super::BareSpinLock;

pub(super) struct InterruptGuard {
    saved_primask: u32,
}

impl InterruptGuard {
    #[inline]
    pub(super) fn new() -> Self {
        let saved_primask: u32;
        unsafe {
            asm!(
                "mrs {}, PRIMASK
                 cpsid i",
                out(reg) saved_primask,
                options(preserves_flags)
            );
        }
        compiler_fence(Ordering::SeqCst);

        Self { saved_primask }
    }
}

impl Drop for InterruptGuard {
    #[inline]
    fn drop(&mut self) {
        compiler_fence(Ordering::SeqCst);
        if (self.saved_primask & 0x1) == 0x0 {
            unsafe {
                asm!("cpsie i", options(preserves_flags));
            }
        }
    }
}

pub struct CortexMSpinLockGuard<'a> {
    lock: &'a BareSpinLock,
    _guard: InterruptGuard,
}

impl<'a> CortexMSpinLockGuard<'a> {
    #[inline(always)]
    pub(super) fn new(guard: InterruptGuard, lock: &'a BareSpinLock) -> Self {
        Self {
            lock,
            _guard: guard,
        }
    }
}

impl Drop for CortexMSpinLockGuard<'_> {
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
