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

use core::cell::UnsafeCell;
use core::ptr::NonNull;

#[cfg(not(feature = "user_space"))]
use foreign_box::ForeignBox;
#[cfg(feature = "user_space")]
use foreign_box::ForeignBox;
use list::ForeignList;
use pw_status::{Error, Result};
use time::Instant;

use crate::Kernel;
use crate::scheduler::RescheduleReason;
use crate::scheduler::locks::SchedLockGuard;
use crate::scheduler::thread::{State, Thread, ThreadListAdapter, ThreadOwner};
use crate::scheduler::timer::{self, Timer};

const WAIT_QUEUE_DEBUG: bool = false;
macro_rules! wait_queue_debug {
    ($($args:expr),*) => {{
        log_if::debug_if!(WAIT_QUEUE_DEBUG, $($args),*)
    }}
}

pub struct WaitQueue<K: Kernel> {
    pub(super) queue: ForeignList<Thread<K>, ThreadListAdapter<K>>,
}

unsafe impl<K: Kernel> Sync for WaitQueue<K> {}
unsafe impl<K: Kernel> Send for WaitQueue<K> {}

impl<K: Kernel> WaitQueue<K> {
    #[allow(dead_code, clippy::new_without_default)]
    #[must_use]
    pub const fn new() -> Self {
        Self {
            queue: ForeignList::new(),
        }
    }
}

#[derive(Clone, Copy, Eq, PartialEq)]
#[repr(u8)]
pub enum WaitType {
    // These variant values must mirror the values in thread `State` so conversions
    // from WaitType to State can be optimized out.
    Interruptible = 6,
    NonInterruptible = 7,
}

const _: () = assert!(WaitType::Interruptible as u8 == State::WaitingInterruptible as u8);
const _: () = assert!(WaitType::NonInterruptible as u8 == State::WaitingNonInterruptible as u8);

impl From<WaitType> for State {
    fn from(value: WaitType) -> Self {
        // Note: Since the values align (see above) this is optimized out.
        match value {
            WaitType::Interruptible => State::WaitingInterruptible,
            WaitType::NonInterruptible => State::WaitingNonInterruptible,
        }
    }
}

#[derive(Clone, Copy, Eq, PartialEq)]
pub enum WakeResult {
    Woken,
    QueueEmpty,
}

impl<K: Kernel> SchedLockGuard<'_, K, WaitQueue<K>> {
    fn add_to_queue_and_reschedule(
        mut self,
        mut thread: ForeignBox<Thread<K>>,
        wait_type: WaitType,
    ) -> Self {
        let current_thread_id = thread.id();
        let current_thread_name = thread.name;
        let new_state = wait_type.into();
        thread.state = new_state;
        thread.owner = ThreadOwner::WaitQueue {
            queue: NonNull::from_ref(&self),
            wait_type,
        };
        self.queue.push_back(thread);
        wait_queue_debug!(
            "WaitQueue: thread '{}' ({:#010x}) rescheduling",
            current_thread_name as &str,
            current_thread_id as usize
        );
        self.block(current_thread_id, new_state)
    }

    // Safety:
    // Caller guarantees that thread is non-null, valid, and process_timeout
    // has exclusive access to `waiting_thread`.
    unsafe fn process_timeout(&mut self, waiting_thread: *mut Thread<K>) -> Option<Error> {
        if !(unsafe { (*waiting_thread).state }.is_waiting()) {
            // Thread has already been woken.
            return None;
        }

        let Some(mut thread) = (unsafe {
            self.queue
                .remove_element(NonNull::new_unchecked(waiting_thread))
        }) else {
            pw_assert::panic!("Thread no longer in wait queue");
        };

        wait_queue_debug!(
            "WaitQueue: timeout for thread '{}' ({:#010x})",
            thread.name as &str,
            thread.id() as usize
        );
        thread.state = State::Ready;
        self.sched_mut()
            .algorithm
            .schedule_thread(thread, RescheduleReason::Woken);
        Some(Error::DeadlineExceeded)
    }

    #[allow(clippy::must_use_candidate)]
    pub fn wake_one(mut self) -> (Self, WakeResult) {
        let Some(mut thread) = self.queue.pop_head() else {
            return (self, WakeResult::QueueEmpty);
        };
        wait_queue_debug!(
            "WaitQueue: waking thread '{}' ({:#010x})",
            thread.name as &str,
            thread.id() as usize
        );
        thread.state = State::Ready;
        self.sched_mut()
            .algorithm
            .schedule_thread(thread, RescheduleReason::Woken);

        (
            self.try_reschedule(RescheduleReason::Preempted),
            WakeResult::Woken,
        )
    }

    #[allow(clippy::return_self_not_must_use, clippy::must_use_candidate)]
    pub fn wake_all(mut self) -> Self {
        loop {
            let result;
            (self, result) = self.wake_one();
            if result == WakeResult::QueueEmpty {
                return self;
            }
        }
    }

    #[allow(clippy::return_self_not_must_use, clippy::must_use_candidate)]
    pub fn wait(mut self, wait_type: WaitType) -> (Self, Result<()>) {
        // If the current thread is terminating and this is an interruptible wait,
        // return early with an error.
        if self.sched().current_thread().terminating && wait_type == WaitType::Interruptible {
            return (self, Err(Error::Cancelled));
        }

        let thread = self.sched_mut().take_current_thread();
        wait_queue_debug!(
            "WaitQueue: thread '{}' ({:#010x}) waiting",
            thread.name as &str,
            thread.id() as usize
        );
        self = self.add_to_queue_and_reschedule(thread, wait_type);
        wait_queue_debug!(
            "WaitQueue: thread '{}' ({:#010x}) resumed",
            self.sched().current_thread_name() as &str,
            self.sched().current_thread_id() as usize
        );

        // Return `Error::Cancelled` if the wait was interrupted because this
        // thread was terminated while it was waiting.
        if self.sched().current_thread().terminating && wait_type == WaitType::Interruptible {
            (self, Err(Error::Cancelled))
        } else {
            (self, Ok(()))
        }
    }

    pub fn wait_until(
        mut self,
        wait_type: WaitType,
        deadline: Instant<K::Clock>,
    ) -> (Self, Result<()>) {
        // If the current thread is terminating and this is an interruptible wait,
        // return early with an error.
        if self.sched().current_thread().terminating && wait_type == WaitType::Interruptible {
            return (self, Err(Error::Cancelled));
        }

        let mut thread = self.sched_mut().take_current_thread();
        wait_queue_debug!(
            "WaitQueue: thread '{}' ({:#010x}) wait_until",
            thread.name as &str,
            thread.id() as usize
        );

        // Smuggle references to the thread and wait queue into the callback.
        // Safety:
        // * The thread will always be active for the lifetime of the wait as
        //   it can not be joined while in a WaitQueue.
        // * The wait queue will outlive the callback because it will either
        //   fire while the thread is in the wait queue or will be the timer
        //   will be canceled before this function returns.
        // * All access to thread_ptr and wait_queue_ptr in the callback are
        //   done while the wait queue lock is held.
        let thread_ptr = unsafe { thread.as_mut_ptr() };
        let smuggled_wait_queue = unsafe { self.smuggle() };

        // Safety:
        // * Only accessed while the wait_queue_lock is held;
        let result: UnsafeCell<Result<()>> = UnsafeCell::new(Ok(()));
        let result_ptr = result.get();

        // Timeout callback will remove the thread from the wait queue and put
        // it back on the run queue.
        let mut callback_closure = move |_kernel, callback: ForeignBox<Timer<K>>, _now| {
            // Safety: wait queue lock is valid for the lifetime of the callback.
            let mut wait_queue = unsafe { smuggled_wait_queue.lock() };

            // Safety: the wait queue lock protects access to the thread.
            wait_queue_debug!(
                "WaitQueue: timeout callback for thread '{}' ({:#010x}) (state: {})",
                unsafe { (*thread_ptr).name } as &str,
                (unsafe { (*thread_ptr).id() }) as usize,
                unsafe { crate::scheduler::thread::to_string((*thread_ptr).state) } as &str
            );

            // Safety: We know that thread_ptr is valid for the life of `wait_until`
            // and this callback will either be called or canceled before `wait_until`
            // returns.
            if let Some(error) = unsafe { wait_queue.process_timeout(thread_ptr) } {
                // Safety: Acquisition of the wait queue lock at the beginning of
                // the callback ensures mutual exclusion with accesses from the
                // body of `wait_until`.
                unsafe { result_ptr.write_volatile(Err(error)) };
            }

            let _ = callback.consume();
            None // Don't re-arm
        };

        let mut callback = Timer::new(deadline, unsafe {
            ForeignBox::new_from_ptr(&raw mut callback_closure)
        });
        let callback_ptr = &raw mut callback;
        timer::schedule_timer(self.kernel, unsafe {
            ForeignBox::new_from_ptr(callback_ptr)
        });

        // Safety: It is important hold on to the WaitQueue lock that is returned
        // from reschedule as the pointers needed by the timer canceling code
        // below rely on it for correctness.
        self = self.add_to_queue_and_reschedule(thread, wait_type);

        wait_queue_debug!(
            "WaitQueue: thread '{}' ({:#010x}) resumed",
            self.sched().current_thread_name() as &str,
            self.sched().current_thread_id() as usize
        );

        // Cancel timeout callback if has not already fired.
        //
        // Safety: callback_ptr is valid until callback goes out of scope.
        unsafe {
            timer::cancel_and_consume_timer(self.kernel, NonNull::new_unchecked(callback_ptr))
        };

        wait_queue_debug!(
            "WaitQueue: thread '{}' ({:#010x}) exiting wait_until",
            self.sched().current_thread_name() as &str,
            self.sched().current_thread_id() as usize
        );

        // Safety:
        //
        // At this point the thread will be in the run queue by virtue of
        // `reschedule()` return and the timer callback will have fired or be
        // canceled.  This leaves no dangling references to our "smuggled"
        // pointers.
        //
        // It is also now safe to read the result UnsafeCell

        // Return `Error::Cancelled` if the wait was interrupted because this
        // thread was terminated while it was waiting.
        if self.sched().current_thread().terminating && wait_type == WaitType::Interruptible {
            (self, Err(Error::Cancelled))
        } else {
            (self, unsafe { result.get().read_volatile() })
        }
    }
}
