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

use foreign_box::ForeignBox;

use crate::Kernel;
use crate::scheduler::{Priority, Thread};

/// The reason a thread is being rescheduled. This can be used as a hint to the
/// scheduling algorithm to determine where the thread should end up in the
/// queue of pending threads.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RescheduleReason {
    /// The currently running thread has been preempted causing it to be re-added to the scheduler.
    Preempted,
    /// A scheduler tick occurred causing the currently running thread to be re-added to the scheduler.
    Ticked,
    /// A currently waiting thread has been unblocked.
    Woken,
    /// A thread has been started and is being added to the scheduler for the first time.
    Started,
    /// A thread has been terminated
    Terminated,
}

/// Per-thread state used by the scheduling algorithm.
pub struct SchedulerAlgorithmThreadState {
    /// The current priority of the thread.
    ///
    /// Kept private so the priority cannot be casually mutated in place while
    /// the thread is indexed in a `PriorityQueueSet`; priority changes must go
    /// through the scheduling algorithm.
    current_priority: Priority,
}

impl SchedulerAlgorithmThreadState {
    #[must_use]
    pub const fn new(current_priority: Priority) -> Self {
        Self { current_priority }
    }

    #[inline]
    #[must_use]
    pub(crate) const fn priority(&self) -> Priority {
        self.current_priority
    }
}

/// Common interface implemented by scheduling algorithm backends.
///
/// Backends are embedded in the kernel's static scheduler state and accessed
/// from any core under the scheduler lock (hence `'static + Send + Sync`).
/// All methods are called with that lock held; implementations must not take
/// additional locks or block.
pub trait SchedulerAlgorithmInterface<K: Kernel>: 'static + Send + Sync {
    /// Takes ownership of `thread` and makes it eligible to run.
    ///
    /// `reason` is a placement hint describing why the thread is being
    /// enqueued (e.g. preempted threads may be placed ahead of peers of equal
    /// priority).
    fn schedule_thread(&mut self, thread: ForeignBox<Thread<K>>, reason: RescheduleReason);

    /// Removes and returns the highest-priority ready thread, or `None` if no
    /// thread is ready.
    fn pop_next_thread(&mut self) -> Option<ForeignBox<Thread<K>>>;
}
