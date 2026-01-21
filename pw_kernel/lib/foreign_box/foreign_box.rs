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

// This module uses the std test harness to allow catching of panics.
#![cfg_attr(not(test), no_std)]

use core::cell::UnsafeCell;
use core::marker::PhantomData;
use core::mem::{ManuallyDrop, offset_of};
use core::ops::{Deref, DerefMut};
use core::ptr::NonNull;
use core::sync::atomic::Ordering;

use pw_atomic::AtomicUsize;

macro_rules! local_panic {
    ($msg:literal $(,)?) => {{
        if cfg!(feature = "core_panic") {
            panic!($msg);
        } else {
            pw_assert::panic!($msg);
        }

    }};

    ($msg:literal, $($args:expr),* $(,)?) => {{
        #[allow(clippy::unnecessary_cast)]
        if cfg!(feature = "core_panic") {
            panic!($msg, $($args),*);
        } else {
            pw_assert::panic!($msg, $($args),*);
        }
    }};
}

pub struct ForeignBox<T: ?Sized> {
    inner: NonNull<T>,

    // Guards against the type being dropped w/o a consume call.
    //
    // TODO: add feature to disable this field on production builds to avoid
    // the overhead of the extra bool.
    consumed: bool,

    // NunNull is covariant over `T` which can not be guaranteed of `KernelRef`.
    // Use `PhantomData` in mark as invariant.
    _phantom: PhantomData<T>,
}

// Safety: Same bounds on `T` as [`Box`].
unsafe impl<T: Send + ?Sized> Send for ForeignBox<T> {}
unsafe impl<T: Sync + ?Sized> Sync for ForeignBox<T> {}

impl<T: ?Sized> ForeignBox<T> {
    #[allow(dead_code)]
    /// Create a new `ForeignBox` from a `NonNull<T>`.
    ///
    /// # Safety
    /// The caller guarantees that `ptr` remains valid throughout the lifetime
    /// of the `ForeignBox` object.
    #[must_use]
    pub const unsafe fn new(ptr: NonNull<T>) -> Self {
        Self {
            inner: ptr,
            consumed: false,
            _phantom: PhantomData,
        }
    }

    /// Create a new `ForeignBox` from a `*mut T`.
    ///
    /// # Panics
    /// panics if `ptr` is null.
    /// # Safety
    /// The caller guarantees that `ptr` remains valid throughout the lifetime
    /// of the `ForeignBox` object.
    #[must_use]
    pub unsafe fn new_from_ptr(ptr: *mut T) -> Self {
        let Some(ptr) = NonNull::new(ptr) else {
            local_panic!("Null pointer");
        };
        unsafe { Self::new(ptr) }
    }

    #[allow(clippy::must_use_candidate)]
    pub fn consume(mut self) -> NonNull<T> {
        self.consumed = true;
        self.inner
    }

    /// Returns a pointer to the contained data.
    ///
    /// # Safety
    /// Creates an "unenforceable borrow" of the contained data.
    #[must_use]
    pub unsafe fn as_ptr(&self) -> *const T {
        self.inner.as_ptr()
    }

    /// Returns a mutable pointer to the contained data.
    ///
    /// # Safety
    /// Creates an "mutable unenforceable borrow" of the contained data.
    #[must_use]
    pub unsafe fn as_mut_ptr(&mut self) -> *mut T {
        unsafe { self.inner.as_mut() }
    }
}

impl<T: ?Sized> Drop for ForeignBox<T> {
    fn drop(&mut self) {
        if !self.consumed {
            local_panic!(
                "ForeignBox@{:08x} dropped before being consumed!",
                self.inner.as_ptr().cast::<()>() as usize
            );
        }
    }
}

impl<T: ?Sized> From<&'static mut T> for ForeignBox<T> {
    fn from(t: &'static mut T) -> ForeignBox<T> {
        // SAFETY: The `'static` lifetime guarantees that `t`'s referent will
        // remain valid for the lifetime of the returned `ForeignBox`.
        unsafe { ForeignBox::new(NonNull::from(t)) }
    }
}

impl<T: ?Sized> AsRef<T> for ForeignBox<T> {
    fn as_ref(&self) -> &T {
        unsafe { self.inner.as_ref() }
    }
}

impl<T: ?Sized> AsMut<T> for ForeignBox<T> {
    fn as_mut(&mut self) -> &mut T {
        unsafe { self.inner.as_mut() }
    }
}

impl<T: ?Sized> Deref for ForeignBox<T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        self.as_ref()
    }
}

impl<T: ?Sized> DerefMut for ForeignBox<T> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        self.as_mut()
    }
}

#[doc(hidden)]
pub struct ForeignRcState<A: AtomicUsize, T: ?Sized> {
    // Disable auto `Send` and `Sync`. We implement them manually to give us an
    // opportunity to document their safety proofs.
    _not_send_sync: PhantomData<NonNull<()>>,

    // `ForeignRcState` has the following lifecycle:
    // - At creation, `ref_count` is equal to 1 and no `ForeignRc` references
    //   exist.
    // - `create_first_ref` is called, which creates the first `ForeignRc`
    //   reference without incrementing the `ref_count`.
    // - From here on out, `ref_count` counts the number of `ForeignRc`
    //   references which exist or which have been forgotten. Each time a
    //   `ForeignRc` is created or dropped, `ref_count` is updated accordingly.
    // - Once `ref_count` reaches 0, there are no `ForeignRc` references, and so
    //   `inner` can be dropped. At this point, `inner` is in an invalid state
    //   and must never be operated on or exposed to safe code outside of this
    //   module.
    //
    // INVARIANTS:
    // - `ref_count` is only modified consistent with this lifecycle
    // - `ref_count` is only modified using thread-safe operations
    ref_count: A,
    // INVARIANTS: `inner` is in a valid state so long as `ref_count > 0`.
    inner: UnsafeCell<ManuallyDrop<T>>,
}

// SAFETY: None of the `ForeignRcState` lifecycle is affected by sending a
// `ForeignRcState` across threads. TODO: Make this more precise
unsafe impl<A: Send + AtomicUsize, T: Send + ?Sized> Send for ForeignRcState<A, T> {}
// SAFETY: By invariant on `ref_count`, it is only modified using thread-safe
// operations. Thus, permitting references (and, by extension, `ForeignRc`s,
// which contain a `&ForeignRcState`) to be sent between threads won't violate
// the `ForeignRcState` lifecycle (so long as `A: Sync` and `T: Sync`).
unsafe impl<A: Sync + AtomicUsize, T: Sync + ?Sized> Sync for ForeignRcState<A, T> {}

impl<A: AtomicUsize, T> ForeignRcState<A, T> {
    pub fn new(val: T) -> Self {
        Self {
            ref_count: A::new(1),
            inner: UnsafeCell::new(ManuallyDrop::new(val)),
            _not_send_sync: PhantomData,
        }
    }
}

impl<A: AtomicUsize, T: ?Sized> ForeignRcState<A, T> {
    /// # Safety
    ///
    /// `create_first_ref` must be the first method called on `self` after
    /// creation with [`new`]. It must not be called twice.
    ///
    /// [`new`]: ForeignRcState::new
    pub unsafe fn create_first_ref(&'static mut self) -> ForeignRc<A, T> {
        // SAFETY: The caller promises that this is the first method call. Thus,
        // this is the first `ForeignRc` reference, consistent with the
        // lifecycle.
        ForeignRc { state: self }
    }
}

impl<A: AtomicUsize, T: Sized> ForeignRcState<A, T> {
    /// Return a new [`ForeignRc`] from a reference to a [`ForeignRcState`]'s
    /// inner storage.
    ///
    /// # Safety
    /// The caller must guarantee that `inner: &T` is a reference storage that
    /// is contained inside a [`ForeignRcState<A, T>`].
    pub unsafe fn create_ref_from_inner(inner: &T) -> ForeignRc<A, T> {
        let inner = NonNull::from_ref(inner);
        let state: &Self = unsafe { inner.byte_sub(offset_of!(Self, inner)).cast().as_ref() };
        state.ref_count.fetch_add(1, Ordering::SeqCst);
        ForeignRc { state }
    }
}

pub struct ForeignRc<A: AtomicUsize + 'static, T: ?Sized + 'static> {
    state: &'static ForeignRcState<A, T>,
}

impl<A: AtomicUsize, T: ?Sized> ForeignRc<A, T> {
    #[doc(hidden)]
    /// Map a `ForeignRc` into a compatible type.
    ///
    /// # Safety
    /// The caller must guarantee that the `map` closure returns a reference
    /// pointing to the same address and byte range as was passed in.
    pub unsafe fn map<U: ?Sized>(
        self,
        map: impl Fn(&ForeignRcState<A, T>) -> &ForeignRcState<A, U>,
    ) -> ForeignRc<A, U> {
        ForeignRc {
            state: map(self.state),
        }
    }
}

impl<A: AtomicUsize, T: ?Sized> Deref for ForeignRc<A, T> {
    type Target = T;

    fn deref(&self) -> &T {
        let ptr = self.state.inner.get();
        // SAFETY: By invariant, `self.state.ref_count` is at least 1 since this
        // `ForeignRc` exists, and also by invariant, `self.state.inner` is
        // valid since `ref_count` is at least 1.
        unsafe { &*ptr }
    }
}

impl<A: AtomicUsize, T: ?Sized> Clone for ForeignRc<A, T> {
    fn clone(&self) -> Self {
        self.state.ref_count.fetch_add(1, Ordering::SeqCst);
        // SAFETY: We're creating a new `ForeignRc`, but we've just increased
        // the `ref_count` by 1. Since, by invariant, `ref_count` previously
        // accurately counted the number of `ForeignRc`s, and since we just
        // created a new one and incremented `ref_count`, it is still accurate.
        Self { state: self.state }
    }
}

impl<A: AtomicUsize, T: ?Sized> Drop for ForeignRc<A, T> {
    fn drop(&mut self) {
        let old = self.state.ref_count.fetch_sub(1, Ordering::SeqCst);
        if old == 1 {
            let ptr = self.state.inner.get();
            // SAFETY: By invariant, `ref_count` counts the number of
            // `ForeignRc`s which exist or which have been forgotten. Since it
            // was 1 before being decremented, this is the last `ForeignRc`. It
            // is sound to construct a mutable reference to `self.state.inner`
            // because:
            // - By virtue of being the last `ForeignRc`, no other code is
            //   accessing `self.state.inner`
            // - By invariant, `self.state.inner` is valid so long as `ref_count
            //   > 0`, which it was at the beginning of this function
            let md = unsafe { &mut *ptr };
            // SAFETY: As described above, `md` references a valid value, so it
            // is sound to drop it. By invariant, since `ref_count` is now 0, no
            // other code will be permitted to access `self.state.inner`, so it
            // is acceptable to leave it in an invalid state.
            unsafe { ManuallyDrop::drop(md) };
        }
    }
}

/// Upcasts a [`ForeignRc`] from a concrete type to a `dyn Trait` that the concrete
/// type implements
#[macro_export]
macro_rules! upcast_foreign_rc {
    ($rc:expr => dyn $trait:ident $(<$($trait_tyvar:ident),*>)? ) => {{
        use $crate::ForeignRcState;
        // SAFETY: The closure passed to `.map()` fulfils the precondition that
        // the returned reference points to the same place as the passed in reference.
        unsafe {
            $rc.map(|inner: &ForeignRcState<_, _>|
                -> &ForeignRcState<_, dyn $trait $(<$($trait_tyvar),*>)?> { inner })
        }
    }};
}

/// Helper type to declare a static value with runtime initialization.
///
/// # Safety
/// The user must ensure that [`StaticStorage::init()`] is only called once.
#[doc(hidden)]
pub struct StaticStorage<T> {
    inner: UnsafeCell<core::mem::MaybeUninit<T>>,
}

impl<T> StaticStorage<T> {
    /// Initialize a new `StaticStorage`.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            inner: UnsafeCell::new(core::mem::MaybeUninit::uninit()),
        }
    }

    /// Initialize the value in the `StaticStorage` and return a mutable
    /// reference to it.
    ///
    /// # Safety
    /// The user must ensure that this method is only called once.
    #[allow(clippy::mut_from_ref)]
    pub unsafe fn init(&self, val: T) -> &mut T {
        unsafe { (*self.inner.get()).write(val) }
    }

    /// Return the address of the underlying storage as an untyped pointer.
    ///
    /// There are circumstances where the address of the static storage needs to
    /// be known at `const` time. One example of this is adding debug annotation
    /// to an image.
    ///
    /// # Safety
    ///
    /// It is up to the caller to ensure that all uses of the returned addresses
    /// are safe and sound with respect to uses of `init()`
    pub const unsafe fn address(&self) -> *const () {
        self.inner.get() as *const ()
    }
}

// SAFETY: By contract, the user will only call `init()` once therefore only
// creating a single reference to the inner data.
unsafe impl<T> Sync for StaticStorage<T> {}

/// Declare a [`ForeignBox`] in static storage that is runtime initialized.
///
/// # Safety
/// Caller must ensure that the macro is executed only once.
#[macro_export]
macro_rules! static_foreign_box {
    ($ty:ty, $init:expr) => {{
        unsafe fn declare_static(val: $ty) -> &'static mut $ty {
            use $crate::StaticStorage;

            static STORAGE: StaticStorage<$ty> = StaticStorage::new();
            unsafe { STORAGE.init(val) }
        }
        let r = declare_static($init);

        // ForeignBox created outside of function to allow type coercion.
        $crate::ForeignBox::new(NonNull::from_ref(r))
    }};
}

/// Declare a [`ForeignRc`] in static storage that is runtime initialized.
///
/// # Safety
/// Caller must ensure that the macro is executed only once.
#[macro_export]
macro_rules! static_foreign_rc {
    ($atomic_usize:ty, $ty:ty, $init:expr) => {{
        unsafe fn declare_static(val: $ty) -> $crate::ForeignRc<$atomic_usize, $ty> {
            use $crate::{ForeignRcState, StaticStorage};

            static STORAGE: StaticStorage<ForeignRcState<$atomic_usize, $ty>> =
                StaticStorage::new();
            unsafe {
                let r = STORAGE.init(ForeignRcState::new(val));
                r.create_first_ref()
            }
        }

        declare_static($init)
    }};
}

#[cfg(test)]
mod tests {
    // Ensure that the console backend (needed for pw_log) is linked.
    use console_backend as _;

    use super::*;
    #[test]
    fn consume_returns_the_same_pointer() {
        let mut value = 0xdecafbad_u32;
        let ptr = &raw mut value;
        let foreign_box = unsafe { ForeignBox::new(NonNull::new_unchecked(ptr)) };
        let consumed_ptr = foreign_box.consume();

        assert_eq!(ptr, consumed_ptr.as_ptr());
    }

    #[should_panic]
    #[test]
    fn non_consumed_box_panics_when_dropped() {
        let mut value = 0xdecafbad_u32;
        let ptr = &raw mut value;
        let _foreign_box = unsafe { ForeignBox::new(NonNull::new_unchecked(ptr)) };

        // Should panic when foreign_box is dropped as it goes out of scope.
    }

    #[test]
    fn value_can_be_read_through_as_ref() {
        let mut value = 0xdecafbad_u32;
        let ptr = &raw mut value;
        let foreign_box = unsafe { ForeignBox::new(NonNull::new_unchecked(ptr)) };
        assert_eq!(value, *(foreign_box.as_ref()));

        // Prevent foreign_box from being dropped.
        let _ = foreign_box.consume();
    }

    #[test]
    fn value_can_be_modified_through_as_mut() {
        let mut value = 0xdecafbad_u32;
        let ptr = &raw mut value;
        let mut foreign_box = unsafe { ForeignBox::new(NonNull::new_unchecked(ptr)) };
        *(foreign_box.as_mut()) = 0xcafecafe;

        assert_eq!(unsafe { ptr.read_volatile() }, 0xcafecafe);

        // Prevent foreign_box from being dropped.
        let _ = foreign_box.consume();
    }

    #[test]
    fn value_can_be_read_through_deref() {
        let mut value = 0xdecafbad_u32;
        let ptr = &raw mut value;
        let foreign_box = unsafe { ForeignBox::new(NonNull::new_unchecked(ptr)) };
        assert_eq!(value, *foreign_box);

        // Prevent foreign_box from being dropped.
        let _ = foreign_box.consume();
    }

    #[test]
    fn value_can_be_modified_through_deref_mut() {
        let mut value = 0xdecafbad_u32;
        let ptr = &raw mut value;
        let mut foreign_box = unsafe { ForeignBox::new(NonNull::new_unchecked(ptr)) };
        *foreign_box = 0xcafecafe;

        assert_eq!(unsafe { ptr.read_volatile() }, 0xcafecafe);

        // Prevent foreign_box from being dropped.
        let _ = foreign_box.consume();
    }

    trait TestDynTrait {
        fn number(&self) -> u32;
    }

    struct TimesOne {
        val: u32,
    }

    impl TestDynTrait for TimesOne {
        fn number(&self) -> u32 {
            self.val
        }
    }

    #[allow(dead_code)]
    struct TimesTwo {
        val: u32,
    }

    #[allow(dead_code)]
    impl TestDynTrait for TimesTwo {
        fn number(&self) -> u32 {
            self.val * 2
        }
    }

    fn get_number(val: &ForeignBox<dyn TestDynTrait>) -> u32 {
        val.as_ref().number()
    }

    #[test]
    fn supports_dynamic_dispatch() {
        let mut times_one_val = TimesOne { val: 10 };
        let ptr = &raw mut times_one_val;
        let times_one_box = unsafe { ForeignBox::<dyn TestDynTrait>::new_from_ptr(ptr) };

        let mut times_two_val = TimesTwo { val: 10 };
        let ptr = &raw mut times_two_val;
        let times_two_box = unsafe { ForeignBox::<dyn TestDynTrait>::new_from_ptr(ptr) };

        assert_eq!(get_number(&times_one_box), 10);
        assert_eq!(get_number(&times_two_box), 20);

        times_one_box.consume();
        times_two_box.consume();
    }

    #[test]
    fn rc_box_ref_count_is_correct() {
        let state = ForeignRcState::<core::sync::atomic::AtomicUsize, _>::new(0xdecafbad_u32);
        let state = Box::leak(Box::new(state));
        assert_eq!(state.ref_count.load(Ordering::SeqCst), 1);

        // SAFETY: We haven't called any methods on `state` (okay, we loaded the
        // `ref_count`, but that doesn't modify anything).
        let rc = unsafe { state.create_first_ref() };

        assert_eq!(rc.state.ref_count.load(Ordering::SeqCst), 1);

        let r1 = rc.clone();
        assert_eq!(rc.state.ref_count.load(Ordering::SeqCst), 2);

        let r2 = rc.clone();
        assert_eq!(rc.state.ref_count.load(Ordering::SeqCst), 3);

        drop(r1);
        assert_eq!(rc.state.ref_count.load(Ordering::SeqCst), 2);

        drop(r2);
        assert_eq!(rc.state.ref_count.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn rc_box_can_be_read_through_deref() {
        let state = ForeignRcState::<core::sync::atomic::AtomicUsize, _>::new(0xdecafbad_u32);
        let state = Box::leak(Box::new(state));
        // SAFETY: We haven't called any methods on `state`.
        let rc = unsafe { state.create_first_ref() };

        assert_eq!(*rc, 0xdecafbad_u32);
    }
}
