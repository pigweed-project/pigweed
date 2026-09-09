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
#![no_std]

use core::cell::{Cell, UnsafeCell};
use core::marker::PhantomData;
use core::ops::{Deref, DerefMut};

/// The interface for low-level (raw) mutual exclusion locks.
///
/// Implementations of this trait provide synchronization primitives that
/// protect shared resources from concurrent access.
///
/// This trait is typically not used directly by application code. Instead, use
/// [`Mutex`], which wraps a backend implementation of this trait to provide a
/// safe, RAII-based, owning API.
///
/// # Safety
///
/// Implementations of this trait must ensure that `lock`, `try_lock`, and
/// `unlock` correctly manage mutual exclusion.
pub unsafe trait RawMutex: Default {
    /// Attempt to acquire the lock without blocking.
    ///
    /// Returns `true` if the lock was acquired successfully, or `false` if
    /// the lock was already held.
    ///
    /// # Panics
    ///
    /// The implementation MUST panic when called from an invalid context such
    /// as with interrupts or scheduling disabled.
    fn try_lock(&self) -> bool;

    /// Acquire the lock, blocking the calling thread indefinitely until
    /// acquired.
    ///
    /// # Panics
    ///
    /// The implementation MUST panic when called from an invalid context such
    /// as with interrupts or scheduling disabled.
    fn lock(&self);

    /// Release the lock.
    ///
    /// The implementation MUST NOT panic if the preconditions below are met.
    ///
    /// # Safety
    ///
    /// The caller must ensure that the lock is currently held and was acquired
    /// by the ***calling thread***.
    unsafe fn unlock(&self);
}

/// Extends [`RawMutex`] with timed blocking operations.
///
/// # Safety
///
/// Implementations of this trait must ensure that `try_lock_for` and
/// `try_lock_until` correctly manage mutual exclusion and adhere to the
/// specified timeouts or deadlines.
pub unsafe trait RawTimedMutex: RawMutex {
    /// The duration type used for timed lock attempts.
    type Duration;

    /// The instant type used for deadline lock attempts.
    type Instant;

    /// Attempt to acquire the lock, blocking up to `timeout`.
    ///
    /// Returns `true` if the lock was acquired successfully before the timeout
    /// elapsed, or `false` if the timeout expired.
    ///
    /// # Panics
    ///
    /// The implementation MUST panic when called from an invalid context such
    /// as with interrupts or scheduling disabled.
    fn try_lock_for(&self, timeout: Self::Duration) -> bool;

    /// Attempt to acquire the lock, blocking until `deadline`.
    ///
    /// Returns `true` if the lock was acquired successfully before the deadline
    /// was reached, or `false` if the deadline expired.
    ///
    /// # Panics
    ///
    /// The implementation MUST panic when called from an invalid context such
    /// as with interrupts or scheduling disabled.
    fn try_lock_until(&self, deadline: Self::Instant) -> bool;
}

/// A mutual exclusion synchronization primitive for protecting shared data.
///
/// `Mutex` is generic over the raw locking primitive `Lock`. Thread-safety
/// (`Send` and `Sync`) depends on the capabilities of the underlying `Lock`.
///
/// # Movability and Allocation
///
/// To support as broad a range of use cases as possible, `Mutex` is designed to
/// be movable by value. Because some RTOS backends (notably FreeRTOS and
/// Zephyr) rely on fixed memory addresses or self-referential internal
/// structures for their mutex primitives, those backends dynamically allocate
/// their underlying OS mutex on creation and clean it up on drop to guarantee
/// address stability when the `Mutex` wrapper is moved.
///
/// # Poisoning
///
/// Unlike `std::sync::Mutex`, this mutex does not support lock poisoning.
/// If a panic occurs while holding a [`MutexGuard`] during unwinding, the
/// mutex remains permanently locked to prevent access to corrupted state.
/// See the lock poisoning design documentation in
/// [`pw_sync_mutex Design & Architecture`](../pw_sync_mutex/_design/index.html#lock-poisoning)
/// for details.
pub struct Mutex<T, Lock> {
    lock: Lock,
    data: UnsafeCell<T>,
}

// SAFETY: Like `std::sync::Mutex`, `Mutex<T, Lock>` is `Sync` if `T: Send` and
// `Lock: Sync`. Because exclusive access is transferred across threads over
// time rather than being shared concurrently, `T` only needs to be `Send`, not
// `Sync`. Additionally, `Lock` must be `Sync` so multiple threads can
// concurrently attempt to acquire the lock via `&self.lock`.
unsafe impl<T: Send, Lock: RawMutex + Sync> Sync for Mutex<T, Lock> {}

// SAFETY: Like `std::sync::Mutex`, `Mutex<T, Lock>` is `Send` if `T: Send` and
// `Lock: Send`. Moving a `Mutex` to another thread transfers ownership of the
// inner `T` and `Lock`, which is safe as long as both `T` and `Lock` are
// `Send`.
unsafe impl<T: Send, Lock: RawMutex + Send> Send for Mutex<T, Lock> {}

impl<T, Lock> Mutex<T, Lock> {
    /// Creates a new `Mutex` wrapping the given initial value using the lock's
    /// [`Default`] implementation.
    ///
    /// # Panics
    ///
    /// Implementations that dynamically allocate their underlying resource will
    /// panic if that allocation fails.
    pub fn new(value: T) -> Self
    where
        Lock: Default,
    {
        Self::from_raw(Lock::default(), value)
    }

    /// Creates a new `Mutex` wrapping a given raw lock and value.
    pub const fn from_raw(lock: Lock, value: T) -> Self {
        Self {
            lock,
            data: UnsafeCell::new(value),
        }
    }

    /// Returns a mutable reference to the underlying data.
    ///
    /// Since this call borrows the `Mutex` mutably, no actual locking needs to
    /// take place—the mutable borrow statically guarantees exclusive access.
    pub fn get_mut(&mut self) -> &mut T {
        self.data.get_mut()
    }

    /// Consumes the `Mutex`, returning the underlying data.
    pub fn into_inner(self) -> T {
        self.data.into_inner()
    }
}

impl<T, Lock: RawMutex> Mutex<T, Lock> {
    /// Acquire the lock, blocking the calling thread indefinitely until
    /// acquired.
    ///
    /// Returns a [`MutexGuard`] that gives exclusive access to the protected
    /// data and releases the lock when dropped.
    ///
    /// # Panics
    ///
    /// Implementations will panic when called from an invalid context such as
    /// with interrupts or scheduling disabled.
    pub fn lock(&self) -> MutexGuard<'_, T, Lock> {
        self.lock.lock();
        MutexGuard {
            lock: self,
            _marker: PhantomData,
        }
    }

    /// Attempt to acquire the lock without blocking.
    ///
    /// Returns `Some(MutexGuard)` if the lock was successfully acquired,
    /// or `None` otherwise.
    ///
    /// # Panics
    ///
    /// Implementations will panic when called from an invalid context such as
    /// with interrupts or scheduling disabled.
    pub fn try_lock(&self) -> Option<MutexGuard<'_, T, Lock>> {
        if self.lock.try_lock() {
            Some(MutexGuard {
                lock: self,
                _marker: PhantomData,
            })
        } else {
            None
        }
    }
}

impl<T: Default, Lock: Default> Default for Mutex<T, Lock> {
    fn default() -> Self {
        Self::new(T::default())
    }
}

/// An RAII guard providing exclusive access to data protected by a [`Mutex`].
///
/// The lock is released when this guard is dropped.
pub struct MutexGuard<'a, T, Lock: RawMutex> {
    lock: &'a Mutex<T, Lock>,

    // Marked !Send via raw pointer PhantomData to enforce that the mutex is
    // unlocked from the same thread it was locked fulfilling the contract of
    // RawMutex.
    _marker: PhantomData<*const ()>,
}

impl<T, Lock: RawMutex> Drop for MutexGuard<'_, T, Lock> {
    fn drop(&mut self) {
        #[cfg(panic = "unwind")]
        {
            // If the thread is panicking during unwind, do not unlock the mutex
            // to avoid exposing potentially inconsistent state. See the lock
            // poisoning section in the design documentation for details.
            extern crate std;
            if std::thread::panicking() {
                return;
            }
        }

        // SAFETY: The existence of `MutexGuard` guarantees that the lock was
        // acquired by the calling thread and is currently held.
        unsafe {
            self.lock.lock.unlock();
        }
    }
}

// SAFETY: Like `std::sync::MutexGuard`, `MutexGuard` is `Sync` if `T: Sync` and
// `Lock: Sync`. A shared reference `&MutexGuard` allows shared access to `&T`
// via `Deref::deref`. If multiple threads have concurrent access to
// `&MutexGuard`, they have concurrent access to `&T`, which requires `T: Sync`.
unsafe impl<T: Sync, Lock: RawMutex + Sync> Sync for MutexGuard<'_, T, Lock> {}

impl<T, Lock: RawMutex> Deref for MutexGuard<'_, T, Lock> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        // SAFETY: `MutexGuard` guarantees exclusive access to the inner data
        // for the duration of its lifetime while the lock is held.
        unsafe { &*self.lock.data.get() }
    }
}

impl<T, Lock: RawMutex> DerefMut for MutexGuard<'_, T, Lock> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        // SAFETY: `&mut self` on `MutexGuard` guarantees exclusive access to
        // the guard, and the held lock guarantees exclusive access to the inner
        // data.
        unsafe { &mut *self.lock.data.get() }
    }
}

impl<T, Lock: RawTimedMutex> Mutex<T, Lock> {
    /// Attempt to acquire the lock, blocking up to `timeout`.
    ///
    /// Returns `Some(MutexGuard)` if the lock was acquired before the timeout,
    /// or `None` if the timeout elapsed.
    ///
    /// # Panics
    ///
    /// Implementations will panic when called from an invalid context such as
    /// with interrupts or scheduling disabled.
    pub fn try_lock_for(&self, timeout: Lock::Duration) -> Option<MutexGuard<'_, T, Lock>> {
        if self.lock.try_lock_for(timeout) {
            Some(MutexGuard {
                lock: self,
                _marker: PhantomData,
            })
        } else {
            None
        }
    }

    /// Attempt to acquire the lock, blocking until `deadline`.
    ///
    /// Returns `Some(MutexGuard)` if the lock was acquired before the deadline,
    /// or `None` if the deadline expired.
    ///
    /// # Panics
    ///
    /// Implementations will panic when called from an invalid context such as
    /// with interrupts or scheduling disabled.
    pub fn try_lock_until(&self, deadline: Lock::Instant) -> Option<MutexGuard<'_, T, Lock>> {
        if self.lock.try_lock_until(deadline) {
            Some(MutexGuard {
                lock: self,
                _marker: PhantomData,
            })
        } else {
            None
        }
    }
}

/// A single-threaded (`Send`, `!Sync`) raw mutex for single-threaded
/// environments.
///
/// This lock can be used with [`Mutex`] in single-threaded environments or
/// thread-local contexts where multithreading synchronization is not required.
/// It implements [`RawMutex`].
#[derive(Default)]
pub struct SingleThreadRawMutex {
    locked: Cell<bool>,
}

// SAFETY: `SingleThreadRawMutex` is `!Sync` (via `Cell<bool>`), restricting its
// concurrent usage to a single thread at a time. Moving ownership to another
// thread (`Send`) is safe when no references are held.
unsafe impl RawMutex for SingleThreadRawMutex {
    fn try_lock(&self) -> bool {
        !self.locked.replace(true)
    }

    fn lock(&self) {
        pw_assert::assert!(
            self.try_lock(),
            "SingleThreadRawMutex deadlock: lock is already held in the current execution context"
        );
    }

    unsafe fn unlock(&self) {
        pw_assert::assert!(
            self.locked.replace(false),
            "unlock called on unlocked SingleThreadRawMutex"
        );
    }
}

/// A mutual exclusion synchronization primitive for single-threaded contexts.
pub type SingleThreadMutex<T> = Mutex<T, SingleThreadRawMutex>;

/// A single-threaded (`Send`, `!Sync`) raw mutex supporting timed locking
/// operations parameterized by a [`Clock`][pw_time::Clock].
///
/// In a single-threaded environment without threading or preemptive scheduling,
/// timed acquisition behaves identically to non-blocking [`try_lock`][RawMutex::try_lock].
pub struct SingleThreadTimedRawMutex<C: pw_time::Clock = pw_time::SystemClock> {
    raw: SingleThreadRawMutex,
    _clock: PhantomData<fn() -> C>,
}

impl<C: pw_time::Clock> Default for SingleThreadTimedRawMutex<C> {
    fn default() -> Self {
        Self {
            raw: SingleThreadRawMutex::default(),
            _clock: PhantomData,
        }
    }
}

// SAFETY: `SingleThreadTimedRawMutex` wraps `SingleThreadRawMutex`, inheriting
// its `Send + !Sync` single-threaded exclusion invariants.
unsafe impl<C: pw_time::Clock> RawMutex for SingleThreadTimedRawMutex<C> {
    fn try_lock(&self) -> bool {
        self.raw.try_lock()
    }

    fn lock(&self) {
        self.raw.lock();
    }

    unsafe fn unlock(&self) {
        // SAFETY: Delegated to `SingleThreadRawMutex::unlock`.
        unsafe { self.raw.unlock() };
    }
}

// SAFETY: In a single-threaded environment without threading or preemptive
// scheduling, timed acquisition behaves identically to `try_lock()`.
unsafe impl<C: pw_time::Clock> RawTimedMutex for SingleThreadTimedRawMutex<C> {
    type Duration = pw_time::Duration<C>;
    type Instant = pw_time::Instant<C>;

    fn try_lock_for(&self, _timeout: Self::Duration) -> bool {
        self.raw.try_lock()
    }

    fn try_lock_until(&self, _deadline: Self::Instant) -> bool {
        self.raw.try_lock()
    }
}

/// A mutual exclusion synchronization primitive with timed locking support for
/// single-threaded contexts.
pub type SingleThreadTimedMutex<T, C = pw_time::SystemClock> =
    Mutex<T, SingleThreadTimedRawMutex<C>>;
