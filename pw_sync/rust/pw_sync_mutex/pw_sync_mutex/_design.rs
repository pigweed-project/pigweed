// Copyright 2026 The Pigweed Authors
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

//! # Design & Architecture
//!
//! ## Unified & Deterministic Behavior
//!
//! A core design goal is providing deterministic and unified behavior across
//! all platforms and backends. Portable code written against `pw_sync_mutex`
//! behaves identically whether running on a host OS via `std` or on an
//! embedded target using backends such as FreeRTOS or the Pigweed kernel. This includes
//! consistent error handling and strict context validation (such as asserting
//! that locks are never acquired from interrupt contexts).
//!
//! ## Mirroring the Standard Library Mutex API
//!
//! Where possible, [`Mutex`] mirrors the ergonomic conventions and API of
//! the standard library `std::sync::Mutex`. This includes methods like
//! [`new`](Mutex::new), [`lock`](Mutex::lock), [`try_lock`](Mutex::try_lock),
//! [`get_mut`](Mutex::get_mut), and [`into_inner`](Mutex::into_inner),
//! as well as RAII guard dereferencing ([`Deref`](core::ops::Deref) /
//! [`DerefMut`](core::ops::DerefMut)) and `Send`/`Sync` safety invariants.
//! The primary difference is that `pw_sync_mutex` operates in `no_std`
//! environments and does not return `PoisonError` on panic recovery (see
//! the [Lock Poisoning](#lock-poisoning) section below).
//!
//! ## Generic Mutex Primitives
//!
//! The core synchronization logic, RAII guards ([`MutexGuard`]), and
//! compile-time `Send`/`Sync` safety boundaries are decoupled from specific
//! lock implementations via the [`RawMutex`] trait. This allows [`Mutex`] to
//! wrap any compatible raw locking primitive. This enables platform-specific
//! implementations, increased testability, and
//! [optional thread safety](#optional-thread-safety).
//!
//! ## Pigweed Facade Pattern for System Locks
//!
//! To provide zero-boilerplate synchronization in typical embedded and host
//! applications, this crate acts as a Pigweed facade that injects the target
//! platform's default lock backend at build time. The concrete aliases
//! [`SystemMutex`] and [`SystemTimedMutex`] bind [`Mutex`] to the
//! build-configured backend, allowing application code to use standard,
//! non-generic mutex types.
//!
//! ## Optional Thread Safety
//!
//! A key design goal is enabling concurrent data structures to degrade
//! efficiently to non-concurrent use. Because [`Mutex<T, Lock>`] is only
//! `Send` and `Sync` when `Lock` implements those traits, higher-level
//! collections can be written generically over `Lock: RawMutex`. When
//! instantiated with [`SingleThreadMutex`] (backed by simple cell checks), the
//! data structure incurs zero atomic or OS locking overhead in
//! single-threaded environments while maintaining RAII safety.
//!
//! ## Additive Timed Mutex API
//!
//! Timed blocking operations ([`try_lock_for`](Mutex::try_lock_for) and
//! [`try_lock_until`](Mutex::try_lock_until)) are designed as a separate,
//! additive API extending [`RawMutex`] through the [`RawTimedMutex`] trait.
//! Lock primitives that lack timer or clock support only need to implement
//! [`RawMutex`]. When an underlying lock implements [`RawTimedMutex`],
//! [`Mutex`] conditionally exposes timed acquisition methods with no extra
//! overhead for non-timed usage.
//!
//! ## Non-Recursive Locking
//!
//! Mutexes in this crate are **strictly non-recursive**: attempting to acquire
//! a lock that is already held by the calling thread is disallowed and will
//! result in a deadlock or panic.
//!
//! Non-recursive locking is required to uphold Rust's core memory safety
//! invariants regarding exclusive mutability. Because [`MutexGuard`] grants
//! mutable access ([`DerefMut`](core::ops::DerefMut)) to the protected data
//! `T`, allowing recursive locking on the same thread would permit creating
//! multiple simultaneous `&mut T` references to the same memory. This would
//! violate Rust's aliasing rules and lead to undefined behavior.
//!
//! ## Lock Poisoning
//!
//! Unlike `std::sync::Mutex`, `pw_sync_mutex` does not support lock poisoning
//! or return `Result<MutexGuard, PoisonError>`. The infrastructure required
//! to track poisoned state across unwinding is not available in `no_std`
//! environments, and embedded targets typically configure `panic = "abort"`.
//!
//! When compiled with `panic = "unwind"` (such as in host-side test runners),
//! if a thread panics while holding a [`MutexGuard`], the guard's [`Drop`]
//! implementation detects the active unwind and deliberately avoids
//! unlocking the underlying raw lock. Because the coherence of protected data
//! cannot be guaranteed after an interrupted critical section, leaving the
//! lock permanently held prevents other threads from observing potentially
//! corrupted state.
//!
//! ## Movable Mutexes & Allocation Trade-offs
//!
//! To achieve unified behavior across platforms and mirror standard library
//! ergonomics, [`Mutex`] is designed to be movable by value across all
//! backends (e.g., returned from functions, stored in structs, or moved into
//! collections). However, many RTOS mutex primitives (such as FreeRTOS and
//! Zephyr) rely on fixed memory addresses or self-referential internal
//! structures that cannot be relocated once initialized.
//!
//! To accommodate this, backends that require fixed addresses dynamically
//! allocate their underlying OS primitive on creation and deallocate it on
//! [`Drop`]. This preserves a consistent, movable API for the outer
//! `Mutex` wrapper across all targets without requiring `Pin` or raw pointer
//! indirection at call sites, at the trade-off of a one-time dynamic allocation
//! during mutex creation.
//!
//! Note: Experiments are underway to enable FreeRTOS and Zephyr mutexes to be
//! movable without requiring dynamic allocation.

// Allow the above docs to reference toplevel crate items directly.
#[allow(unused_imports)]
use crate::*;
