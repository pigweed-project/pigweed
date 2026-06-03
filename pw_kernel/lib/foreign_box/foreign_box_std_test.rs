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

use core::ptr::NonNull;
use core::sync::atomic::Ordering;

use foreign_box::{ForeignBox, ForeignRcState, upcast_foreign_rc};

#[should_panic]
#[test]
fn non_consumed_box_panics_when_dropped() {
    let mut value = 0xdecafbad_u32;
    let ptr = &raw mut value;
    let _foreign_box = unsafe { ForeignBox::new(NonNull::new_unchecked(ptr)) };
}

#[test]
fn rc_box_ref_count_is_correct() {
    let state = ForeignRcState::<core::sync::atomic::AtomicUsize, _>::new(0xdecafbad_u32);
    let state = Box::leak(Box::new(state));
    assert_eq!(state.ref_count().load(Ordering::SeqCst), 1);

    // SAFETY: We haven't called any methods on `state` (okay, we loaded the
    // `ref_count`, but that doesn't modify anything).
    let rc = unsafe { state.create_first_ref() };

    assert_eq!(rc.state().ref_count().load(Ordering::SeqCst), 1);

    let r1 = rc.clone();
    assert_eq!(rc.state().ref_count().load(Ordering::SeqCst), 2);

    let r2 = rc.clone();
    assert_eq!(rc.state().ref_count().load(Ordering::SeqCst), 3);

    drop(r1);
    assert_eq!(rc.state().ref_count().load(Ordering::SeqCst), 2);

    drop(r2);
    assert_eq!(rc.state().ref_count().load(Ordering::SeqCst), 1);
}

#[test]
fn rc_box_can_be_read_through_deref() {
    let state = ForeignRcState::<core::sync::atomic::AtomicUsize, _>::new(0xdecafbad_u32);
    let state = Box::leak(Box::new(state));
    // SAFETY: We haven't called any methods on `state`.
    let rc = unsafe { state.create_first_ref() };

    assert_eq!(*rc, 0xdecafbad_u32);
}

#[test]
fn rc_upcast_compiles_and_has_coherent_refcount() {
    trait UpcastTestTrait {
        fn number(&self) -> u32;
    }

    struct UpcastTestStruct {
        val: u32,
    }

    impl UpcastTestTrait for UpcastTestStruct {
        fn number(&self) -> u32 {
            self.val
        }
    }

    let val = UpcastTestStruct { val: 42 };
    let state = ForeignRcState::<core::sync::atomic::AtomicUsize, _>::new(val);
    let state = Box::leak(Box::new(state));
    let rc = unsafe { state.create_first_ref() };

    let upcasted = upcast_foreign_rc!(rc => dyn UpcastTestTrait);
    assert_eq!(upcasted.number(), 42);
    assert_eq!(upcasted.state().ref_count().load(Ordering::SeqCst), 1);
}

#[test]
fn rc_ref_from_inner_increments_ref_count() {
    let val = 42;
    let state = ForeignRcState::<core::sync::atomic::AtomicUsize, _>::new(val);
    let state = Box::leak(Box::new(state));
    let rc = unsafe { state.create_first_ref() };

    let inner: &i32 = &rc;
    let rc2 = unsafe {
        ForeignRcState::<core::sync::atomic::AtomicUsize, i32>::create_ref_from_inner(inner)
    };

    assert_eq!(*rc, 42);
    assert_eq!(*rc2, 42);
    assert_eq!(rc.state().ref_count().load(Ordering::SeqCst), 2);
}
