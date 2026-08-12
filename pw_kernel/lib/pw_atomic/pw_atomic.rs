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

use core::sync::atomic::Ordering;

pub trait AtomicAdd<T> {
    /// Adds `val` to the current value, returning the previous value.
    ///
    /// Behaves the same as [`core::atomic::AtomicUsize::fetch_add()`].  Notably
    /// this means that is has wrapping semantics.
    fn fetch_add(&self, val: T, order: Ordering) -> T;
}

pub trait AtomicSub<T> {
    /// Subtracts `val` to the current value, returning the previous value.
    ///
    /// Behaves the same as [`core::atomic::AtomicUsize::fetch_sub()`].  Notably
    /// this means that is has wrapping semantics.
    fn fetch_sub(&self, val: T, order: Ordering) -> T;
}

pub trait AtomicLoad<T> {
    /// Loads and returns the current value.
    ///
    /// Behaves the same as [`core::atomic::AtomicUsize::load()`].
    fn load(&self, order: Ordering) -> T;
}

pub trait AtomicStore<T> {
    /// Stores a value in the atomic.
    ///
    /// Behaves the same as [`core::atomic::AtomicUsize::store()`].
    fn store(&self, val: T, order: Ordering);
}

pub trait AtomicCompareExchange<T> {
    /// Stores a value into the atomic if the current value is the same as the [`current`] value.
    ///
    /// Behaves the same as [`core::atomic::AtomicUsize::compare_exchange()`].
    fn compare_exchange(
        &self,
        current: T,
        new: T,
        success: Ordering,
        failure: Ordering,
    ) -> Result<T, T>;
}

pub trait AtomicNew<T> {
    /// Returns a new atomic with `val`
    fn new(val: T) -> Self;
}

pub trait AtomicFalse {
    const FALSE: Self;
}

pub trait AtomicZero {
    const ZERO: Self;
}

pub trait Atomic<T>:
    AtomicNew<T> + AtomicLoad<T> + AtomicStore<T> + AtomicCompareExchange<T> + Send + Sync
{
}

pub trait AtomicBool: Atomic<bool> + AtomicFalse {}

pub trait AtomicUsize: Atomic<usize> + AtomicAdd<usize> + AtomicSub<usize> + AtomicZero {}

#[cfg(feature = "builtin_impls")]
mod builtin_impls {
    use super::*;
    macro_rules! impl_for_scalar {
        ($atomic_type:ty, $primitive_type:ty) => {
            impl AtomicLoad<$primitive_type> for $atomic_type {
                fn load(&self, ordering: Ordering) -> $primitive_type {
                    self.load(ordering)
                }
            }

            impl AtomicStore<$primitive_type> for $atomic_type {
                fn store(&self, val: $primitive_type, ordering: Ordering) {
                    self.store(val, ordering)
                }
            }

            impl AtomicCompareExchange<$primitive_type> for $atomic_type {
                fn compare_exchange(
                    &self,
                    current: $primitive_type,
                    new: $primitive_type,
                    success: Ordering,
                    failure: Ordering,
                ) -> Result<$primitive_type, $primitive_type> {
                    self.compare_exchange(current, new, success, failure)
                }
            }

            impl AtomicNew<$primitive_type> for $atomic_type {
                fn new(val: $primitive_type) -> Self {
                    Self::new(val)
                }
            }

            impl Atomic<$primitive_type> for $atomic_type {}
        };
    }

    macro_rules! impl_for_numeric {
        ($atomic_type:ty, $primitive_type:ty) => {
            impl_for_scalar!($atomic_type, $primitive_type);

            impl AtomicAdd<$primitive_type> for $atomic_type {
                fn fetch_add(&self, val: $primitive_type, ordering: Ordering) -> $primitive_type {
                    self.fetch_add(val, ordering)
                }
            }

            impl AtomicSub<$primitive_type> for $atomic_type {
                fn fetch_sub(&self, val: $primitive_type, ordering: Ordering) -> $primitive_type {
                    self.fetch_sub(val, ordering)
                }
            }

            impl AtomicZero for $atomic_type {
                const ZERO: Self = Self::new(0);
            }
        };
    }

    impl AtomicFalse for core::sync::atomic::AtomicBool {
        const FALSE: Self = Self::new(false);
    }
    impl_for_scalar!(core::sync::atomic::AtomicBool, bool);
    impl AtomicBool for core::sync::atomic::AtomicBool {}

    impl_for_numeric!(core::sync::atomic::AtomicUsize, usize);
    impl AtomicUsize for core::sync::atomic::AtomicUsize {}

    impl_for_numeric!(core::sync::atomic::AtomicU32, u32);
}
