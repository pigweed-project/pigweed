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

use core::ptr::NonNull;
use core::sync::atomic::Ordering;

use pw_atomic::{AtomicAdd, AtomicLoad, AtomicSub, AtomicZero};
use pw_status::Result;
use pw_time_core::Instant;

use crate::scheduler::{WaitQueueLock, WaitQueueLockGuard, WaitType, WakeList, WakeResult};
use crate::{Kernel, SchedulerState, SpinLockGuard};

/// Configuration for the behavior of an [`Event`].
#[derive(Eq, PartialEq)]
pub enum EventConfig {
    /// When an [`Event`] is signaled, the first waiter to observe that signal
    /// automatically clears the signaled state, returning the `Event` to the
    /// un-signaled state.
    AutoReset,
    /// When an [`Event`] is signaled, it remains signaled indefinitely until
    /// [`unsignal`] is called, which returns the `Event` to the un-signaled
    /// state.
    ///
    /// [`unsignal`]: EventSignaler::unsignal
    ManualReset,
}

struct EventState {
    signaled: bool,
}

/// A basic synchronization primitive allowing threads to block on an event
/// happening.
///
/// An `Event` starts in an un-signaled state. Threads can use [`wait`] or
/// [`wait_until`] to block until the state has changed to signaled. The signal
/// state is controlled by an [`EventSignaler`] via the [`signal_and_wake`] and [`unsignal`]
/// methods.
///
/// Depending on what [`EventConfig`] is used, [`signal_and_wake`] may either set the
/// signal permanently, or it may be automatically cleared once a single waiter
/// has been un-blocked by it.
///
/// # Panics
///
/// Panics on drop if there are [`EventSignaler`]s referencing this object.
///
/// [`wait`]: Event::wait
/// [`wait_until`]: Event::wait_until
/// [`signal_and_wake`]: EventSignaler::signal_and_wake
/// [`unsignal`]: EventSignaler::unsignal
pub struct Event<K: Kernel> {
    kernel: K,
    config: EventConfig,
    state: WaitQueueLock<K, EventState>,
    signalers: K::AtomicUsize,
}

/// A signaler for an [`Event`]
///
/// [`EventSignaler`] is returned from [`Event::get_signaler()`] and is used to
/// signal the referenced Event.  It can be cloned and the referenced [`Event`]
/// will panic if it is dropped when any [`EventSignaler`]s still reference it.
pub struct EventSignaler<K: Kernel> {
    event: NonNull<Event<K>>,
}

// SAFETY: Event will panic if there are outstanding signalers referencing it
// guaranteeing that the event pointer will always be valid.
unsafe impl<K: Kernel> Send for EventSignaler<K> {}
unsafe impl<K: Kernel> Sync for EventSignaler<K> {}

impl<K: Kernel> Clone for EventSignaler<K> {
    fn clone(&self) -> Self {
        self.event()
            .signalers
            // TODO: rationalize ordering
            .fetch_add(1, Ordering::SeqCst);
        Self { event: self.event }
    }
}

impl<K: Kernel> Drop for EventSignaler<K> {
    fn drop(&mut self) {
        self.event()
            .signalers
            // TODO: rationalize ordering
            .fetch_sub(1, Ordering::SeqCst);
    }
}

impl<K: Kernel> EventSignaler<K> {
    fn event(&self) -> &Event<K> {
        // SAFETY: An `Event` will panic if there are outstanding signalers
        // referencing it.  Since we bind the lifetime of the `&Event` to the
        // lifetime of this signaler, we are assured the Event will remain
        // valid.
        unsafe { self.event.as_ref() }
    }

    /// Signals the `Event` and wakes any waiting threads, rescheduling if needed.
    ///
    /// Consumes the signaler so no references to the event remain before threads
    /// are woken, which avoids use-after-free on stack-allocated events.
    ///
    /// For long-lived events, the signaler can be cloned to signal multiple times
    /// or from multiple threads.  It is up to the user to ensure that the
    /// [`Event`] is only dropped after all signalers are dropped.
    ///
    /// # Interrupt context
    ///
    /// This method *is* safe to call in an interrupt context.
    pub fn signal_and_wake(self) {
        let kernel = self.event().kernel;
        let sched = kernel.get_scheduler().lock(kernel);
        let _ = self.signal_and_wake_locked(kernel, sched);
    }

    /// Sets the `Event`'s state to signaled and wakes waiters while holding the scheduler lock.
    ///
    /// Consumes the signaler, matching [`Self::signal_and_wake`].
    ///
    /// # Interrupt context
    ///
    /// This method *is* safe to call in an interrupt context.
    pub(crate) fn signal_and_wake_locked<'a>(
        self,
        kernel: K,
        sched: SpinLockGuard<'a, K, SchedulerState<K>>,
    ) -> SpinLockGuard<'a, K, SchedulerState<K>> {
        let mut wake_list = WakeList::new();
        let sched = self.signal_and_dequeue_locked(sched, &mut wake_list);
        wake_list.wake_and_reschedule(kernel, sched)
    }

    /// Signals the event and dequeues waiters into `wake_list` without waking them.
    ///
    /// Consumes `self` so no signaler references remain before the caller wakes the threads.
    pub(crate) fn signal_and_dequeue_locked<'a>(
        self,
        sched: SpinLockGuard<'a, K, SchedulerState<K>>,
        wake_list: &mut WakeList<K>,
    ) -> SpinLockGuard<'a, K, SchedulerState<K>> {
        // `state` (`WaitQueueLockGuard`) borrows the `EventSignaler` through
        // `event()`, preventing `drop(self)` from being called before
        // `state.into_sched()`. Wrap `self` in `ManuallyDrop` so we can
        // manually decrement `event.signalers` while `state` is still held.
        let this = core::mem::ManuallyDrop::new(self);
        let event = this.event();
        let state = event.state.inherit_sched_lock(sched);

        let state = event.signal_locked_dequeue(state, wake_list);
        // Decrement the signaler count while holding `state` so that once
        // `WaitQueueLock` uses a fine-grained lock, a waiter taking the fast
        // path in `Event::wait` cannot observe `state.signaled == true` and
        // drop a stack-allocated `Event` before the signaler count is updated.
        event.signalers.fetch_sub(1, Ordering::SeqCst);
        state.into_sched()
    }

    /// Sets the `Event`'s state to un-signaled.
    ///
    /// # Interrupt context
    ///
    /// This method *is* safe to call in an interrupt context.
    pub fn unsignal(&self) {
        self.event().unsignal();
    }
}

unsafe impl<K: Kernel> Sync for Event<K> {}
unsafe impl<K: Kernel> Send for Event<K> {}

impl<K: Kernel> Event<K> {
    /// Constructs a new `Event` with the given configuration.
    ///
    #[must_use]
    pub const fn new(kernel: K, config: EventConfig) -> Self {
        Self {
            kernel,
            config,
            state: WaitQueueLock::new(kernel, EventState { signaled: false }),
            signalers: K::AtomicUsize::ZERO,
        }
    }

    /// Returns an [`EventSignaler`] for signaling an `Event`.
    ///
    #[must_use]
    pub fn get_signaler(&self) -> EventSignaler<K> {
        self.signalers.fetch_add(1, Ordering::SeqCst);
        EventSignaler {
            event: NonNull::from_ref(self),
        }
    }

    /// Waits until the `Event` is in the signaled state.
    ///
    /// If the event's configuration is [`AutoReset`], the thread which is
    /// un-blocked by a signal also clears that signal, resetting its value to
    /// un-signaled.
    ///
    /// [`AutoReset`]: EventConfig::AutoReset
    ///
    /// # Interrupt context
    ///
    /// This method is *not* safe to call in an interrupt context.
    pub fn wait(&self) -> Result<()> {
        let mut state = self.state.lock();
        if !state.signaled {
            let (_, ret) = state.wait(WaitType::Interruptible);
            return ret;
        } else if self.config == EventConfig::AutoReset {
            state.signaled = false;
        }
        Ok(())
    }

    /// Waits until the `Event` is in the signaled state or the `deadline` is
    /// reached, whichever happens first.
    ///
    /// If the event's configuration is [`EventConfig::AutoReset`], the thread
    /// which is un-blocked by a signal also clears that signal, resetting its
    /// value to un-signaled.
    ///
    /// # Interrupt context
    ///
    /// This method is *not* safe to call in an interrupt context.
    pub fn wait_until(&self, deadline: Instant<K::Clock>) -> Result<()> {
        let mut state = self.state.lock();
        if !state.signaled {
            let (_state, result) = state.wait_until(WaitType::Interruptible, deadline);
            return result;
        } else if self.config == EventConfig::AutoReset {
            state.signaled = false;
        }

        Ok(())
    }

    /// Updates the event's state and dequeues its waiters into `wake_list` without waking them.
    fn signal_locked_dequeue<'lock, 'sched>(
        &self,
        mut state: WaitQueueLockGuard<'lock, 'sched, K, EventState>,
        wake_list: &mut WakeList<K>,
    ) -> WaitQueueLockGuard<'lock, 'sched, K, EventState> {
        if !state.signaled {
            match self.config {
                EventConfig::AutoReset => {
                    if state.dequeue_one(wake_list) == WakeResult::QueueEmpty {
                        state.signaled = true;
                    }
                }
                EventConfig::ManualReset => {
                    state.signaled = true;
                    let _ = state.dequeue_all(wake_list);
                }
            }
        }
        state
    }

    fn unsignal(&self) {
        self.state.lock().signaled = false;
    }
}

impl<K: Kernel> Drop for Event<K> {
    fn drop(&mut self) {
        // TODO: rationalize ordering
        let num_signalers = self.signalers.load(Ordering::SeqCst);
        if num_signalers > 0 {
            pw_assert::panic!(
                "Event droped with {} active signalers",
                num_signalers as usize
            )
        }
    }
}
