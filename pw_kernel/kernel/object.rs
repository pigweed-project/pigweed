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
pub use syscall_defs::{Signals, WaitReturn};
use time::Instant;

use crate::Kernel;
use crate::sync::event::{Event, EventConfig, EventSignaler};
use crate::sync::spinlock::SpinLock;

mod buffer;
mod channel;
mod interrupt;

pub use buffer::SyscallBuffer;
pub use channel::{ChannelHandlerObject, ChannelInitiatorObject};
pub use interrupt::InterruptObject;

/// Trait that all kernel objects implement.
///
/// The methods on this trait map directly to the kernel's system calls.
pub trait KernelObject<K: Kernel>: Any + Send + Sync {
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

struct ObjectWaiter<K: Kernel> {
    link: Link,
    signaler: EventSignaler<K>,
    signal_mask: Signals,
    wait_result: WaitResult,
}

pub struct ObjectBaseState<K: Kernel> {
    active_signals: Signals,
    waiters: RandomAccessForeignList<ObjectWaiter<K>, ObjectWaiterListAdapter<K>>,
}

impl<K: Kernel> ObjectBaseState<K> {
    #[must_use]
    pub const fn new() -> Self {
        Self {
            active_signals: Signals::new(),
            waiters: RandomAccessForeignList::new(),
        }
    }
}

/// Translates handles to dynamic references to kernel objects.
///
/// `ObjectTable` is a trait to allow for both static and dynamic tables to be
/// used in the same time.
pub trait ObjectTable<K: Kernel> {
    fn get_object(
        &self,
        kernel: K,
        handle: u32,
    ) -> Option<ForeignRc<K::AtomicUsize, dyn KernelObject<K>>>;
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
}

/// Common functionality used by many kernel objects
pub struct ObjectBase<K: Kernel> {
    state: SpinLock<K, ObjectBaseState<K>>,
}

impl<K: Kernel> ObjectBase<K> {
    #[must_use]
    pub const fn new() -> Self {
        Self {
            state: SpinLock::new(ObjectBaseState::new()),
        }
    }
}

impl<K: Kernel> ObjectBase<K> {
    pub fn wait_until(
        &self,
        kernel: K,
        signal_mask: Signals,
        deadline: Instant<K::Clock>,
    ) -> Result<WaitReturn> {
        let mut state = self.state.lock(kernel);

        // Skip waiting if signals are already pending.
        if state.active_signals.intersects(signal_mask) {
            return Ok(WaitReturn {
                user_data: 0,
                pending_signals: state.active_signals,
            });
        }

        let event = Event::new(kernel, EventConfig::ManualReset);
        let signaler = event.get_signaler();

        let waiter = ObjectWaiter {
            link: Link::new(),
            signaler,
            signal_mask,
            wait_result: WaitResult::new(),
        };

        // Safety: `waiter_box`'s membership in `self.waiters` is managed by
        // and never outlives this function's lifetime.
        let waiter_box = unsafe { ForeignBox::new(NonNull::from_ref(&waiter)) };

        let key = state.waiters.push_back(waiter_box);

        // Drop state lock while waiting.
        drop(state);
        let wait_result = event.wait_until(deadline);
        let mut state = self.state.lock(kernel);

        // Before processing the wait result, we remove the waiter from the queue.
        let waiter_box = state.waiters.remove_element(key);

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

    pub fn signal(&self, kernel: K, active_signals: Signals) {
        let mut state = self.state.lock(kernel);
        state.active_signals = active_signals;

        let _ = state.waiters.for_each(|waiter| -> Result<()> {
            if waiter.signal_mask.intersects(active_signals) {
                // Safety: While a waiter is in an object's `waiters` list, that
                // object has exclusive access to the waiter.  The below
                // operation is done with the object's spinlock held.
                unsafe {
                    waiter.wait_result.set(Ok(WaitReturn {
                        user_data: 0,
                        pending_signals: active_signals,
                    }))
                };
                waiter.signaler.signal();
            }
            Ok(())
        });
    }
}
