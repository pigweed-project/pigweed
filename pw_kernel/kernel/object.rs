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

use core::any::Any;
use core::cell::UnsafeCell;
use core::ptr::NonNull;

use foreign_box::{ForeignBox, ForeignRc};
use list::{self, Link, RandomAccessForeignList};
use pw_status::{Error, Result};
use pw_time_core::Instant;
pub use syscall_defs::{ExitStatus, Signals, WaitReturn};

use crate::Kernel;
use crate::object::wait_group::WaitGroupMember;
use crate::scheduler::SchedulerState;
use crate::sync::event::{Event, EventConfig, EventSignaler};
use crate::sync::spinlock::{SpinLock, SpinLockGuard};

mod buffer;
mod channel;
mod interrupt;
mod process;
mod thread;
mod wait_group;

pub use buffer::SyscallBuffer;
pub use channel::{ChannelHandlerObject, ChannelInitiatorObject};
pub use interrupt::InterruptObject;
pub use process::{MainThread, ProcessObject};
pub use thread::ThreadObject;
pub use wait_group::WaitGroupObject;

/// Trait that all kernel objects implement.
///
/// The methods on this trait map directly to the kernel's system calls.
pub trait KernelObject<K: Kernel>: Any + Send + Sync {
    fn base(&self) -> Option<&ObjectBase<K>> {
        None
    }

    /// Dump the object's state for debugging.
    fn dump(&self, kernel: K) {
        if let Some(base) = self.base() {
            base.dump(kernel);
        }
    }
    /// Wait on a set of signals to be active.
    ///
    /// Blocks until any of the signals in `signal_mask` are active on the object
    /// or `deadline` has expired.
    #[allow(unused_variables)]
    fn object_wait(
        &self,
        kernel: K,
        signal_mask: Signals,
        deadline: Instant<K::Clock>,
    ) -> Result<WaitReturn> {
        Err(Error::Unimplemented)
    }

    /// Add an `object` to a `WaitGroupObject`.
    ///
    /// `object_wait()` on the `WaitGroup` will return if any of the signals
    /// in `signal_mask` are active on the object, return the value of `user_data`.
    ///
    /// # Safety
    /// Caller must ensure that `self` is a reference storage that is contained
    /// inside a [`foreign_box::ForeignRcState<A, T>`].
    #[allow(unused_variables)]
    unsafe fn wait_group_add(
        &self,
        kernel: K,
        object: &dyn KernelObject<K>,
        signal_mask: Signals,
        user_data: usize,
    ) -> Result<()> {
        Err(Error::Unimplemented)
    }

    /// Remove an `object` to a `WaitGroupObject`.
    #[allow(unused_variables)]
    fn wait_group_remove(&self, kernel: K, object: &dyn KernelObject<K>) -> Result<()> {
        Err(Error::Unimplemented)
    }

    #[allow(unused_variables)]
    fn channel_transact(
        &self,
        kernel: K,
        send_buffer: SyscallBuffer,
        recv_buffer: SyscallBuffer,
        deadline: Instant<K::Clock>,
    ) -> Result<usize> {
        Err(Error::Unimplemented)
    }

    #[allow(unused_variables)]
    fn channel_async_transact(
        &self,
        kernel: K,
        send_buffer: SyscallBuffer,
        recv_buffer: SyscallBuffer,
    ) -> Result<()> {
        Err(Error::Unimplemented)
    }

    #[allow(unused_variables)]
    fn channel_async_transact_complete(&self, kernel: K) -> Result<usize> {
        Err(Error::Unimplemented)
    }

    #[allow(unused_variables)]
    fn channel_async_cancel(&self, kernel: K) -> Result<()> {
        Err(Error::Unimplemented)
    }

    #[allow(unused_variables)]
    fn channel_read(&self, kernel: K, offset: usize, read_buffer: SyscallBuffer) -> Result<usize> {
        Err(Error::Unimplemented)
    }

    #[allow(unused_variables)]
    fn channel_respond(&self, kernel: K, response_buffer: SyscallBuffer) -> Result<()> {
        Err(Error::Unimplemented)
    }

    #[allow(unused_variables)]
    fn interrupt_ack(&self, kernel: K, signal_mask: Signals) -> Result<()> {
        Err(Error::Unimplemented)
    }

    #[allow(unused_variables)]
    fn thread_start(&self, kernel: K, initial_pc: usize, initial_sp: usize) -> Result<()> {
        Err(Error::Unimplemented)
    }

    #[allow(unused_variables)]
    fn task_terminate(&self, kernel: K) -> Result<()> {
        Err(Error::Unimplemented)
    }

    #[allow(unused_variables)]
    fn task_join(&self, kernel: K) -> Result<ExitStatus> {
        Err(Error::Unimplemented)
    }

    #[allow(unused_variables)]
    fn process_start(&self, kernel: K) -> Result<()> {
        Err(Error::Unimplemented)
    }

    /// Set (`set=true`) or clear (`set=false`) `Signals::USER` on the paired peer.
    #[allow(unused_variables)]
    fn object_set_peer_user_signal(&self, kernel: K, set: bool) -> Result<()> {
        Err(Error::Unimplemented)
    }

    /// Reset the object's signals to its initial signals.
    fn reset(&self, _kernel: K) -> Result<()> {
        Ok(())
    }
}

trait WaiterState<K: Kernel> {
    fn waiters(
        &mut self,
    ) -> &mut RandomAccessForeignList<ObjectWaiter<K>, ObjectWaiterListAdapter<K>>;
}

fn wait_on_object<'a, K: Kernel, S: WaiterState<K>>(
    kernel: K,
    object_lock: &'a SpinLock<K, S>,
    mut object_state: SpinLockGuard<'a, K, S>,
    signal_mask: Signals,
    deadline: Instant<K::Clock>,
) -> Result<WaitReturn> {
    let event = Event::new(kernel, EventConfig::ManualReset);
    let waiter = ObjectWaiter {
        link: Link::new(),
        signaler: event.get_signaler(),
        signal_mask,
        wait_result: WaitResult::new(),
    };

    // Safety: `waiter_box`'s membership in `self.waiters` is managed by
    // and never outlives this function's lifetime.
    let waiter_box = unsafe { ForeignBox::new(NonNull::from_ref(&waiter)) };

    let key = object_state.waiters().push_back(waiter_box);

    // Drop state lock while waiting.
    drop(object_state);
    let wait_result = event.wait_until(deadline);
    let mut state = object_lock.lock(kernel);

    // Before processing the wait result, we remove the waiter from the queue.
    let waiter_box = state.waiters().remove_element(key);

    let result = match wait_result {
        Err(e) => Err(e),
        // Safety: Since `waiter_box` has been removed from the object's
        // `waiters` list, there is no way for another thread to have a
        // reference to the waiter.
        _ => unsafe { waiter_box.wait_result.get() },
    };

    // `waiter_box` is no longer referenced and is safe to consume.
    waiter_box.consume();

    result
}

// For all objects except wait groups, `wake_signals`` and `return_signals`` should be
// the same.  For a wait group, the wake_signal is READABLE, and the return signal
// is the signal of the woken element.
pub(crate) fn signal_all_matching_waiters_with_return_signals_locked<'a, K: Kernel>(
    mut sched: SpinLockGuard<'a, K, SchedulerState<K>>,
    waiters: &mut RandomAccessForeignList<ObjectWaiter<K>, ObjectWaiterListAdapter<K>>,
    wake_signals: Signals,
    return_signals: Signals,
    user_data: usize,
) -> SpinLockGuard<'a, K, SchedulerState<K>> {
    for waiter in waiters.iter() {
        if waiter.signal_mask.intersects(wake_signals) {
            // SAFETY: While a waiter is in an object's `waiters` list, that
            // object has exclusive access to the waiter.  The below
            // operation is done with the object's spinlock held.
            unsafe {
                waiter.wait_result.set(Ok(WaitReturn {
                    user_data,
                    pending_signals: return_signals,
                }))
            };
            sched = waiter.signaler.signal_locked(sched);
        }
    }
    sched
}

pub(crate) fn signal_all_matching_waiters_locked<'a, K: Kernel>(
    sched: SpinLockGuard<'a, K, SchedulerState<K>>,
    waiters: &mut RandomAccessForeignList<ObjectWaiter<K>, ObjectWaiterListAdapter<K>>,
    active_signals: Signals,
    user_data: usize,
) -> SpinLockGuard<'a, K, SchedulerState<K>> {
    signal_all_matching_waiters_with_return_signals_locked(
        sched,
        waiters,
        active_signals,
        active_signals,
        user_data,
    )
}

list::define_adapter!(pub ObjectWaiterListAdapter<K: Kernel> => ObjectWaiter<K>::link);

struct WaitResult {
    result: UnsafeCell<Result<WaitReturn>>,
}

impl WaitResult {
    pub const fn new() -> Self {
        Self {
            result: UnsafeCell::new(Err(Error::Unknown)),
        }
    }

    /// Safety: Users of [`WaitResult::get()`] and [`WaitResult::set()`] must
    /// ensure that their calls into them are not done concurrently.
    unsafe fn get(&self) -> Result<WaitReturn> {
        unsafe { *self.result.get() }
    }

    /// Safety: Users of [`WaitResult::get()`] and [`WaitResult::set()`] must
    /// ensure that their calls into them are not done concurrently.
    unsafe fn set(&self, result: Result<WaitReturn>) {
        unsafe { (*self.result.get()) = result }
    }
}

pub struct ObjectWaiter<K: Kernel> {
    link: Link,
    signaler: EventSignaler<K>,
    signal_mask: Signals,
    wait_result: WaitResult,
}

pub struct ObjectBaseState<K: Kernel> {
    active_signals: Signals,
    wait_group: Option<WaitGroupMember<K>>,
    waiters: RandomAccessForeignList<ObjectWaiter<K>, ObjectWaiterListAdapter<K>>,
}

impl<K: Kernel> ObjectBaseState<K> {
    #[must_use]
    pub const fn new(active_signals: Signals) -> Self {
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

/// Translates handles to dynamic references to kernel objects.
///
/// `ObjectTable` is a trait to allow for both static and dynamic tables to be
/// used in the same time.
pub trait ObjectTable<K: Kernel>: Send + Sync {
    fn get_object(
        &self,
        kernel: K,
        handle: u32,
    ) -> Option<ForeignRc<K::AtomicUsize, dyn KernelObject<K>>>;

    /// Dump the object table's state for debugging.
    fn dump(&self, kernel: K);

    fn reset_all(&self, kernel: K) -> Result<()>;
}
/// An object table with no entries.
///
/// This may be replaced with a static object tables with no entries.
pub struct NullObjectTable {}

impl NullObjectTable {
    #[must_use]
    pub const fn new() -> Self {
        Self {}
    }
}

impl<K: Kernel> ObjectTable<K> for NullObjectTable {
    fn get_object(
        &self,
        _kernel: K,
        _handle: u32,
    ) -> Option<ForeignRc<K::AtomicUsize, dyn KernelObject<K>>> {
        None
    }

    fn dump(&self, _kernel: K) {
        pw_log::info!("      No objects in table.");
    }

    fn reset_all(&self, _kernel: K) -> Result<()> {
        Ok(())
    }
}

impl<const N: usize, K: Kernel> ObjectTable<K>
    for [ForeignRc<<K>::AtomicUsize, dyn KernelObject<K>>; N]
{
    fn get_object(
        &self,
        _kernel: K,
        handle: u32,
    ) -> Option<ForeignRc<<K>::AtomicUsize, dyn KernelObject<K>>> {
        self.get(handle as usize).cloned()
    }

    fn dump(&self, kernel: K) {
        for (i, obj) in self.iter().enumerate() {
            pw_log::info!("      Object {}:", i as usize);
            obj.dump(kernel);
        }
    }

    fn reset_all(&self, kernel: K) -> Result<()> {
        for obj in self.iter() {
            obj.reset(kernel)?;
        }
        Ok(())
    }
}
/// Common functionality used by many kernel objects
pub struct ObjectBase<K: Kernel> {
    wait_group_link: Link,
    state: SpinLock<K, ObjectBaseState<K>>,
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

    pub fn signal<F: Fn(Signals) -> Signals>(&self, kernel: K, update_fn: F) {
        let sched = kernel.get_scheduler().lock(kernel);
        let _ = self.signal_locked(kernel, sched, update_fn);
    }

    pub(crate) fn signal_locked<'a, F: Fn(Signals) -> Signals>(
        &self,
        kernel: K,
        sched: SpinLockGuard<'a, K, SchedulerState<K>>,
        update_fn: F,
    ) -> SpinLockGuard<'a, K, SchedulerState<K>> {
        let mut state = self.state.lock(kernel);
        state.active_signals = update_fn(state.active_signals);
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
        signal_all_matching_waiters_locked(sched, &mut state.waiters, active_signals, 0)
    }
}
