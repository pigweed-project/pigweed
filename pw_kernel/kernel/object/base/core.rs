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

use list::{Link, RandomAccessForeignList};
use pw_status::Result;
use pw_time_core::Instant;

use super::wait_group::WaitGroupMember;
use crate::Kernel;
use crate::object::{
    ObjectWaiter, ObjectWaiterListAdapter, SignalUpdate, Signals, WaitReturn, WaiterState,
    signal_all_matching_waiters_locked, wait_on_object,
};
use crate::scheduler::SchedulerState;
use crate::sync::spinlock::{SpinLock, SpinLockGuard};

pub(super) struct ObjectBaseState<K: Kernel> {
    pub(super) active_signals: Signals,
    pub(super) wait_group: Option<WaitGroupMember<K>>,
    waiters: RandomAccessForeignList<ObjectWaiter<K>, ObjectWaiterListAdapter<K>>,
}

impl<K: Kernel> ObjectBaseState<K> {
    #[must_use]
    const fn new(active_signals: Signals) -> Self {
        Self {
            active_signals,
            wait_group: None,
            waiters: RandomAccessForeignList::new(),
        }
    }
}

impl<K: Kernel> WaiterState<K> for ObjectBaseState<K> {
    fn waiters(
        &mut self,
    ) -> &mut RandomAccessForeignList<ObjectWaiter<K>, ObjectWaiterListAdapter<K>> {
        &mut self.waiters
    }
}

/// Common functionality used by many kernel objects
pub struct ObjectBase<K: Kernel> {
    pub(super) wait_group_link: Link,
    pub(super) state: SpinLock<K, ObjectBaseState<K>>,
}

impl<K: Kernel> ObjectBase<K> {
    #[must_use]
    pub const fn new(active_signals: Signals) -> Self {
        Self {
            wait_group_link: Link::new(),
            state: SpinLock::new(ObjectBaseState::new(active_signals)),
        }
    }
}

impl<K: Kernel> ObjectBase<K> {
    #[must_use]
    pub fn active_signals(&self, kernel: K) -> Signals {
        self.state.lock(kernel).active_signals
    }

    pub fn dump(&self, kernel: K) {
        let state = self.state.lock(kernel);
        pw_log::info!("        Signals: {}", state.active_signals.bits() as u32);
    }

    pub fn wait_until(
        &self,
        kernel: K,
        signal_mask: Signals,
        deadline: Instant<K::Clock>,
    ) -> Result<WaitReturn> {
        let state = self.state.lock(kernel);

        // Skip waiting if any of the requested signals are already pending.
        if state.active_signals.intersects(signal_mask) {
            return Ok(WaitReturn {
                user_data: 0,
                pending_signals: state.active_signals,
            });
        }

        wait_on_object(kernel, &self.state, state, signal_mask, deadline)
    }

    pub fn signal(&self, kernel: K, update: SignalUpdate) {
        let sched = kernel.get_scheduler().lock(kernel);
        let _ = self.signal_locked(kernel, sched, update);
    }

    pub(crate) fn signal_locked<'a>(
        &self,
        kernel: K,
        sched: SpinLockGuard<'a, K, SchedulerState<K>>,
        update: SignalUpdate,
    ) -> SpinLockGuard<'a, K, SchedulerState<K>> {
        let mut state = self.state.lock(kernel);
        state.active_signals = update.apply(state.active_signals);
        self.signal_impl_locked(kernel, sched, state)
    }

    // Hint to avoid monomorphization bloat.
    #[inline(never)]
    fn signal_impl_locked<'a>(
        &self,
        kernel: K,
        mut sched: SpinLockGuard<'a, K, SchedulerState<K>>,
        mut state: SpinLockGuard<K, ObjectBaseState<K>>,
    ) -> SpinLockGuard<'a, K, SchedulerState<K>> {
        let active_signals = state.active_signals;
        if let Some(wait_group) = &mut state.wait_group {
            sched = wait_group.signal_locked(kernel, sched, active_signals, self);
        }

        // These waiters are never a wait group, so always set user_data to 0.
        signal_all_matching_waiters_locked(kernel, sched, &mut state.waiters, active_signals, 0)
    }
}
