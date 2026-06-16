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

use foreign_box::{ForeignRc, ForeignRcState};
use list::RandomAccessForeignList;
use pw_status::{Error, Result};
use pw_time_core::Instant;
use syscall_defs::WaitReturn;

use crate::Kernel;
use crate::object::{
    KernelObject, ObjectBase, ObjectWaiter, ObjectWaiterListAdapter, Signals, WaiterState,
    signal_all_matching_waiters_with_return_signals_locked, wait_on_object,
};
use crate::scheduler::SchedulerState;
use crate::sync::spinlock::{SpinLock, SpinLockGuard};

list::define_adapter!(pub ObjectListAdapter<K: Kernel> => ObjectBase<K>::wait_group_link);

struct WaitGroupState<K: Kernel> {
    /// List of wait group members that have one or more active signals which are
    /// waited on by this wait group.
    ///
    /// # Safety
    ///
    /// * Access to this list must be guarded by the `WaitGroupObject` state lock.
    /// * Elements in this list must be valid `ObjectBase` instances.
    /// * Elements in this list can only be in one list at a time.
    /// * Elements in this list must have their `wait_group` member set to a `WaitGroupMember`
    ///   of this wait group.
    signaled_objects: list::UnsafeList<ObjectBase<K>, ObjectListAdapter<K>>,
    /// List of wait group members that do not have any active signals which are
    /// waited on by this wait group.
    ///
    /// # Safety
    ///
    /// * Access to this list must be guarded by the `WaitGroupObject` state lock.
    /// * Elements in this list must be valid `ObjectBase` instances.
    /// * Elements in this list can only be in one list at a time.
    /// * Elements in this list must have their `wait_group` member set to a `WaitGroupMember`
    ///   of this wait group.
    unsignaled_objects: list::UnsafeList<ObjectBase<K>, ObjectListAdapter<K>>,
    waiters: RandomAccessForeignList<ObjectWaiter<K>, ObjectWaiterListAdapter<K>>,
}

impl<K: Kernel> WaitGroupState<K> {
    #[must_use]
    pub const fn new() -> Self {
        Self {
            signaled_objects: list::UnsafeList::new(),
            unsignaled_objects: list::UnsafeList::new(),
            waiters: RandomAccessForeignList::new(),
        }
    }
}

impl<K: Kernel> WaiterState<K> for WaitGroupState<K> {
    fn waiters(
        &mut self,
    ) -> &mut RandomAccessForeignList<ObjectWaiter<K>, ObjectWaiterListAdapter<K>> {
        &mut self.waiters
    }
}

pub struct WaitGroupMember<K: Kernel> {
    signal_mask: Signals,
    user_data: usize,
    wait_group: ForeignRc<K::AtomicUsize, WaitGroupObject<K>>,
    is_signaled: bool,
}

impl<K: Kernel> WaitGroupMember<K> {
    pub(crate) fn signal_locked<'a>(
        &mut self,
        kernel: K,
        mut sched: SpinLockGuard<'a, K, SchedulerState<K>>,
        active_signals: Signals,
        base: &ObjectBase<K>,
    ) -> SpinLockGuard<'a, K, SchedulerState<K>> {
        let signaled = active_signals.intersects(self.signal_mask);
        if !signaled && !self.is_signaled {
            return sched;
        }

        let mut lock = self.wait_group.state.lock(kernel);
        let state = &mut *lock;
        match (signaled, self.is_signaled) {
            (true, false) => Self::move_member_between_lists(
                &mut state.unsignaled_objects,
                &mut state.signaled_objects,
                base,
            ),
            (false, true) => Self::move_member_between_lists(
                &mut state.signaled_objects,
                &mut state.unsignaled_objects,
                base,
            ),
            _ => {}
        }

        self.is_signaled = signaled;
        if signaled {
            sched = signal_all_matching_waiters_with_return_signals_locked(
                sched,
                &mut state.waiters,
                Signals::READABLE,
                active_signals,
                self.user_data,
            );
        }
        sched
    }

    fn move_member_between_lists(
        from: &mut list::UnsafeList<ObjectBase<K>, ObjectListAdapter<K>>,
        to: &mut list::UnsafeList<ObjectBase<K>, ObjectListAdapter<K>>,
        base: &ObjectBase<K>,
    ) {
        // Safety:
        // - The `WaitGroupObject` state lock is held.
        // - The list is valid per `WaitGroupState` list preconditions.
        let Some(member) = (unsafe { from.unlink_element(base.into()) }) else {
            pw_assert::panic!("Wait group member not in a list");
        };

        // Safety:
        // - The preconditions from the above unsafe block apply here.
        // - Upholds the invariant that the member is in a single list by
        //   removing it from the first list before adding it to the second.
        unsafe { to.push_front_unchecked(member) };
    }
}

/// Object for waiting on a group of objects.
///
/// When any member object is signaled, the `WaitGroupObject` itself becomes `READABLE`.
pub struct WaitGroupObject<K: Kernel> {
    state: SpinLock<K, WaitGroupState<K>>,
}

impl<K: Kernel> WaitGroupObject<K> {
    #[must_use]
    pub const fn new() -> Self {
        Self {
            state: SpinLock::new(WaitGroupState::new()),
        }
    }
}

impl<K: Kernel> KernelObject<K> for WaitGroupObject<K> {
    fn object_wait(
        &self,
        kernel: K,
        signal_mask: Signals,
        deadline: Instant<K::Clock>,
    ) -> Result<WaitReturn> {
        let state = self.state.lock(kernel);
        // Error if there are no members in this wait group.
        // Safety:
        // - The `WaitGroupObject` state lock is held.
        // - The lists are valid per `WaitGroupState` list preconditions.
        if unsafe { state.signaled_objects.is_empty() && state.unsignaled_objects.is_empty() } {
            return Err(Error::InvalidArgument);
        }

        // Skip waiting if signals are already pending on any of the objects.
        // Safety:
        // - The `WaitGroupObject` state lock is held.
        // - The `signaled_objects` list is valid per `WaitGroupState` list preconditions.
        if let Some(already_signaled) = unsafe { state.signaled_objects.peek_head() } {
            // Safety:
            // - The list is guarded by the `WaitGroupObject` state lock, ensuring the
            //   pointer is valid.
            // - `ObjectBase` is `Sync`, so it is safe to create a shared reference.
            let object = unsafe { &(*already_signaled.as_ptr()) };
            let object_state = object.state.lock(kernel);
            let Some(wait_group_member) = object_state.wait_group.as_ref() else {
                pw_assert::panic!(
                    "Object in wait group signaled objects list is not a member of a wait group."
                )
            };
            return Ok(WaitReturn {
                user_data: wait_group_member.user_data,
                pending_signals: object_state.active_signals,
            });
        }

        wait_on_object(kernel, &self.state, state, signal_mask, deadline)
    }

    unsafe fn wait_group_add(
        &self,
        kernel: K,
        object: &dyn KernelObject<K>,
        signal_mask: Signals,
        user_data: usize,
    ) -> Result<()> {
        let Some(base) = object.base() else {
            // Currently, the only object which doesn't have a base is WaitGroupObject,
            // so we use this property to test whether `object` is a wait group or not.
            // In the future we could possibly add support for wait groups being a member
            // of a wait group once there is a use case for it.
            return Err(Error::InvalidArgument);
        };

        // Objects can only ever be in one wait_group at a time.
        let mut object_state = base.state.lock(kernel);
        if object_state.wait_group.is_some() {
            return Err(Error::ResourceExhausted);
        }

        let is_signaled = object_state.active_signals.intersects(signal_mask);
        let mut state = self.state.lock(kernel);
        // TODO: b/487292381 - Improve the ergonamics of the object table and references which
        // would allow the removal of unsafe on wait_group_add().
        // Safety: The caller guarantees that `self` is wrapped in a [`ForeignRcState<A, T>`].
        let wait_group_rc = unsafe { ForeignRcState::create_ref_from_inner(self) };
        let entry = WaitGroupMember {
            signal_mask,
            user_data,
            wait_group: wait_group_rc,
            is_signaled,
        };

        object_state.wait_group = Some(entry);
        if is_signaled {
            // Safety: list guarded by WaitGroupObject state lock.
            unsafe { state.signaled_objects.push_front_unchecked(base.into()) };
        } else {
            // Safety: list guarded by WaitGroupObject state lock.
            unsafe { state.unsignaled_objects.push_front_unchecked(base.into()) };
        }

        Ok(())
    }

    fn wait_group_remove(&self, kernel: K, object: &dyn KernelObject<K>) -> Result<()> {
        let Some(base) = object.base() else {
            // Currently, the only object which doesn't have a base is WaitGroupObject,
            // so we use this property to test whether `object` is a wait group or not.
            // In the future we could possibly add support for wait groups being a member
            // of a wait group once there is a use case for it.
            return Err(Error::InvalidArgument);
        };

        let mut object_state = base.state.lock(kernel);
        let Some(ref wait_group) = object_state.wait_group else {
            // Object is not in a wait group.
            return Err(Error::NotFound);
        };

        // Check the object is in this wait group.
        if !core::ptr::eq(&*wait_group.wait_group, self) {
            return Err(Error::NotFound);
        }

        let mut state = self.state.lock(kernel);

        // The object to remove can be in either the signaled or unsignaled
        // state, so check which list to remove it from.
        if wait_group.is_signaled {
            // Safety: list guarded by WaitGroupObject state lock.
            unsafe { state.signaled_objects.unlink_element_unchecked(base.into()) }
        } else {
            // Safety: list guarded by WaitGroupObject state lock.
            unsafe {
                state
                    .unsignaled_objects
                    .unlink_element_unchecked(base.into())
            }
        }

        object_state.wait_group = None;

        Ok(())
    }

    fn reset(&self, kernel: K) -> Result<()> {
        loop {
            let mut wait_group_state = self.state.lock(kernel);
            // Safety: Access to lists is guarded by WaitGroupObject state lock.
            let base_ptr = unsafe {
                wait_group_state
                    .signaled_objects
                    .pop_head()
                    .or_else(|| wait_group_state.unsignaled_objects.pop_head())
            };
            let Some(base_ptr) = base_ptr else {
                break;
            };

            // Drop the wait group state lock before acquiring the object base lock
            // to avoid potential deadlock.
            drop(wait_group_state);

            // Safety: The pointer is valid as long as the object lives.
            let base = unsafe { base_ptr.as_ref() };
            let mut base_state = base.state.lock(kernel);
            base_state.wait_group = None;
        }

        Ok(())
    }
}
