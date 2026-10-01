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

//! Per-hart interrupt masking for RISC-V (M-mode).
//!
//! # Guarantees and assumptions
//!
//! - **Per hart only.** CSRs are "associated with each hart" ([Zicsr]), so
//!   clearing `mstatus.MIE` masks interrupts on the executing hart only. For
//!   exclusion across harts, use [`RiscVSpinLockGuard`], which pairs this guard
//!   with a `BareSpinLock`.
//!
//! - **Nestable (re-entrant).** A nested guard saves `MIE`=0 and restores
//!   nothing, so only the outermost guard re-enables interrupts. This also works
//!   inside trap handlers, since on trap entry "xIE is set to 0" ([mstatus]).
//!   Guards must be dropped in LIFO order. `RiscVSpinLockGuard` itself is *not*
//!   re-entrant on the same lock.
//!
//! - **Assumes precise interrupts.** The RISC-V ISA specifies that the
//!   Execution Environment Interface (EEI) defines whether each trap is handled
//!   precisely ([traps]). In bare-metal M-mode, this depends on the core and
//!   platform documentation.
//!
//! # Why no hardware `fence` is needed
//!
//! Masking needs no hardware `fence`:
//!
//! - **Same hart:** Clearing `mstatus.MIE` takes effect immediately
//!   ([interrupts]), so no maskable interrupt can fire between `csrrci` and
//!   `csrs`. An ISR therefore runs entirely before or after the critical
//!   section. On a core with precise interrupts, this is standard single-hart
//!   program order, which RVWMO already preserves without fences ([rvwmo]).
//!
//! - **Other harts:** Hardware `FENCE` only orders accesses as seen by other
//!   harts and external devices ([fence]). That cross-hart ordering is handled
//!   by the spin lock's atomics, not this guard.
//!
//! NMIs and synchronous exceptions are *not* masked. Their handlers must not
//! touch data protected by these guards.
//!
//! [Zicsr]: https://github.com/riscv/riscv-isa-manual/blob/51c1291fc8168bf36530de3386d3f452069ce327/src/unpriv/zicsr.adoc?plain=1#L4-L5
//! [mstatus]: https://github.com/riscv/riscv-isa-manual/blob/51c1291fc8168bf36530de3386d3f452069ce327/src/priv/machine.adoc?plain=1#L394-L396
//! [interrupts]: https://github.com/riscv/riscv-isa-manual/blob/51c1291fc8168bf36530de3386d3f452069ce327/src/priv/machine.adoc?plain=1#L1346-L1358
//! [traps]: https://github.com/riscv/riscv-isa-manual/blob/51c1291fc8168bf36530de3386d3f452069ce327/src/unpriv/intro.adoc?plain=1#L658-L661
//! [rvwmo]: https://github.com/riscv/riscv-isa-manual/blob/51c1291fc8168bf36530de3386d3f452069ce327/src/unpriv/rvwmo.adoc?plain=1#L11-L14
//! [fence]: https://github.com/riscv/riscv-isa-manual/blob/51c1291fc8168bf36530de3386d3f452069ce327/src/unpriv/rv32.adoc?plain=1#L773-L779

use core::arch::asm;
use core::marker::PhantomData;
use core::sync::atomic::{Ordering, compiler_fence};

use super::BareSpinLock;

/// `mstatus.MIE` (bit 3).
const MSTATUS_MIE: usize = 1 << 3;

/// Masks M-mode interrupts on the current hart; restores the previous `MIE`
/// value on drop. See the module docs for scope and assumptions.
pub struct InterruptGuard {
    saved_mstatus: usize,
    /// Makes the guard `!Send`: it must be dropped on the hart that created it.
    _not_send: PhantomData<*mut ()>,
}

impl InterruptGuard {
    #[inline]
    pub fn new() -> Self {
        let saved_mstatus: usize;
        // "All CSR instructions atomically read-modify-write a single CSR", so
        // reading `mstatus` and clearing MIE happen in one step. Omitting
        // `nomem` stops the compiler from reordering memory accesses across
        // this.
        unsafe {
            asm!(
                "csrrci {}, mstatus, {mie}",
                out(reg) saved_mstatus,
                mie = const MSTATUS_MIE,
                options(nostack, preserves_flags),
            );
        }
        // Synchronizes `Relaxed` atomics with ISRs on this hart.
        compiler_fence(Ordering::SeqCst);
        Self {
            saved_mstatus,
            _not_send: PhantomData,
        }
    }
}

impl Drop for InterruptGuard {
    #[inline]
    fn drop(&mut self) {
        compiler_fence(Ordering::SeqCst);
        // Set MIE only if it was set on entry. For a nested guard the operand
        // is 0, which leaves `mstatus` unchanged.
        unsafe {
            asm!(
                "csrs mstatus, {}",
                in(reg) self.saved_mstatus & MSTATUS_MIE,
                options(nostack, preserves_flags),
            );
        }
    }
}

/// An RAII guard that keeps a `BareSpinLock` held and interrupts masked on the
/// current hart.
///
/// Dropping the guard releases the spin lock and restores the previous
/// interrupt mask state.
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
        // SAFETY: `Drop::drop` runs before the fields are dropped, so the lock
        // is released while `_guard` still masks interrupts.
        unsafe { self.lock.unlock() };
    }
}
