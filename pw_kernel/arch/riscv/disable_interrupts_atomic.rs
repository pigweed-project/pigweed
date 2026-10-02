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

#[cfg(feature = "smp")]
compile_error!("disable_interrupts_atomic only supports uniprocessor (UP) systems");

use core::arch::asm;
use core::cell::UnsafeCell;
use core::sync::atomic::Ordering;

use pw_atomic::{
    Atomic, AtomicAdd, AtomicCompareExchange, AtomicFalse, AtomicLoad, AtomicNew, AtomicOne,
    AtomicStore, AtomicSub, AtomicZero,
};

use crate::spinlock::InterruptGuard;

/// Compiler memory barrier (`asm volatile("" ::: "memory")`).
///
/// Unlike `core::sync::atomic::compiler_fence` (which formally only orders
/// atomic accesses), an inline `asm!` block without `options(nomem)` acts as a
/// general compiler memory clobber that prevents the optimizer from reordering
/// both volatile and non-volatile memory reads and writes across it while
/// emitting zero machine instructions.
#[inline(always)]
fn compiler_memory_barrier() {
    // SAFETY: Emits no instructions; omitting `nomem` clobbers memory in the
    // compiler optimizer.
    unsafe {
        asm!("", options(nostack, preserves_flags));
    }
}

macro_rules! impl_for_scalar {
    ($atomic_type:ty, $primitive_type:ty) => {
        impl AtomicLoad<$primitive_type> for $atomic_type {
            #[inline]
            fn load(&self, _ordering: Ordering) -> $primitive_type {
                // `read_volatile` only orders volatile accesses relative to
                // other volatile accesses. Unconditionally bracket the load
                // with `compiler_memory_barrier()` (`asm!("")`) to provide
                // `SeqCst` compiler ordering (which satisfies all weaker
                // `Ordering`s on UP) without branching at runtime on a
                // compile-time barrier.
                compiler_memory_barrier();
                // SAFETY: `self.value.get()` is valid and properly aligned, and
                // aligned `bool`/`usize` reads compile to a single instruction
                // (`lbu`/`lw`) on RV32, which cannot be torn by an interrupt.
                let val = unsafe { self.value.get().read_volatile() };
                compiler_memory_barrier();
                val
            }
        }

        impl AtomicStore<$primitive_type> for $atomic_type {
            #[inline]
            fn store(&self, val: $primitive_type, _ordering: Ordering) {
                // Unconditionally bracket the volatile store with
                // `compiler_memory_barrier()` (`asm!("")`) to provide `SeqCst`
                // compiler ordering (satisfying all weaker `Ordering`s on UP)
                // without branching at runtime on a compile-time barrier.
                compiler_memory_barrier();
                // SAFETY: `self.value.get()` is valid and properly aligned, and
                // aligned `bool`/`usize` writes compile to a single instruction
                // (`sb`/`sw`) on RV32, which cannot be torn by an interrupt.
                unsafe {
                    self.value.get().write_volatile(val);
                }
                compiler_memory_barrier();
            }
        }

        impl AtomicCompareExchange<$primitive_type> for $atomic_type {
            #[inline]
            fn compare_exchange(
                &self,
                current: $primitive_type,
                new: $primitive_type,
                _success: Ordering,
                _failure: Ordering,
            ) -> Result<$primitive_type, $primitive_type> {
                // `_success` and `_failure` are ignored because RMW operations
                // on UP must disable interrupts via `InterruptGuard`, whose
                // entry and exit `asm!` blocks omit `nomem` and therefore
                // inherently act as a two-sided (`SeqCst`) compiler memory
                // barrier. In the Rust/C++ memory model, providing stronger
                // (`SeqCst`) ordering satisfies all weaker orderings (`Relaxed`,
                // `Acquire`, `Release`, `AcqRel`).
                let _guard = InterruptGuard::new();
                let ptr = self.value.get();
                // SAFETY: Exclusive access is guaranteed on UP because local
                // interrupts are masked by `_guard`.
                let actual = unsafe { ptr.read_volatile() };
                if actual == current {
                    // SAFETY: Exclusive access is guaranteed by `_guard`.
                    unsafe {
                        ptr.write_volatile(new);
                    }
                    Ok(current)
                } else {
                    Err(actual)
                }
            }
        }

        impl AtomicNew<$primitive_type> for $atomic_type {
            #[inline]
            fn new(val: $primitive_type) -> Self {
                Self {
                    value: UnsafeCell::new(val),
                }
            }
        }

        impl Atomic<$primitive_type> for $atomic_type {}
    };
}

macro_rules! impl_for_numeric {
    ($atomic_type:ty, $primitive_type:ty) => {
        impl_for_scalar!($atomic_type, $primitive_type);

        impl AtomicAdd<$primitive_type> for $atomic_type {
            #[inline]
            fn fetch_add(&self, val: $primitive_type, _ordering: Ordering) -> $primitive_type {
                // `_ordering` is ignored because `InterruptGuard`'s entry and
                // exit `asm!` blocks omit `nomem` and inherently act as a
                // two-sided (`SeqCst`) compiler memory barrier, which satisfies
                // all weaker orderings (`Relaxed`, `Acquire`, `Release`,
                // `AcqRel`) on UP.
                let _guard = InterruptGuard::new();
                let ptr = self.value.get();
                // SAFETY: Exclusive access is guaranteed on UP because local
                // interrupts are masked by `_guard`.
                unsafe {
                    let current = ptr.read_volatile();
                    // Wrapping semantics per
                    // https://doc.rust-lang.org/std/sync/atomic/type.AtomicUsize.html#method.fetch_add
                    ptr.write_volatile(current.wrapping_add(val));
                    current
                }
            }
        }

        impl AtomicSub<$primitive_type> for $atomic_type {
            #[inline]
            fn fetch_sub(&self, val: $primitive_type, _ordering: Ordering) -> $primitive_type {
                // `_ordering` is ignored because `InterruptGuard`'s entry and
                // exit `asm!` blocks omit `nomem` and inherently act as a
                // two-sided (`SeqCst`) compiler memory barrier, which satisfies
                // all weaker orderings (`Relaxed`, `Acquire`, `Release`,
                // `AcqRel`) on UP.
                let _guard = InterruptGuard::new();
                let ptr = self.value.get();
                // SAFETY: Exclusive access is guaranteed on UP because local
                // interrupts are masked by `_guard`.
                unsafe {
                    let current = ptr.read_volatile();
                    // Wrapping semantics per
                    // https://doc.rust-lang.org/std/sync/atomic/type.AtomicUsize.html#method.fetch_sub
                    ptr.write_volatile(current.wrapping_sub(val));
                    current
                }
            }
        }

        impl AtomicZero for $atomic_type {
            const ZERO: Self = Self {
                value: UnsafeCell::new(0),
            };
        }

        impl AtomicOne for $atomic_type {
            const ONE: Self = Self {
                value: UnsafeCell::new(1),
            };
        }
    };
}

pub struct AtomicBool {
    value: UnsafeCell<bool>,
}

impl AtomicBool {
    pub const fn new(val: bool) -> Self {
        Self {
            value: UnsafeCell::new(val),
        }
    }
}

impl AtomicFalse for AtomicBool {
    const FALSE: Self = Self::new(false);
}
impl_for_scalar!(AtomicBool, bool);
impl pw_atomic::AtomicBool for AtomicBool {}

// SAFETY: Atomicity is guaranteed by single-instruction volatile loads/stores
// paired with compiler memory barriers and by disabling interrupts for RMW
// operations on a UP system. MP systems are not supported.
unsafe impl Sync for AtomicBool {}

pub struct AtomicUsize {
    value: UnsafeCell<usize>,
}

impl_for_numeric!(AtomicUsize, usize);
impl pw_atomic::AtomicUsize for AtomicUsize {}

// SAFETY: Atomicity is guaranteed by single-instruction volatile loads/stores
// paired with compiler memory barriers and by disabling interrupts for RMW
// operations on a UP system. MP systems are not supported.
unsafe impl Sync for AtomicUsize {}
