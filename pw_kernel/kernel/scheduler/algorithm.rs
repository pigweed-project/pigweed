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

use core::mem::MaybeUninit;

use foreign_box::ForeignBox;
use list::ForeignList;

use crate::Kernel;
use crate::scheduler::priority_bitmask::PriorityBitmask;
use crate::scheduler::thread::{ThreadListAdapter, ThreadOwner};
use crate::scheduler::{Priority, Thread};

type RunQueue<K> = ForeignList<Thread<K>, ThreadListAdapter<K>>;

/// The reason a thread is being rescheduled. This can be used as a hint to the
/// scheduling algorithm to determine where the thread should end up in the
/// queue of pending threads.
#[derive(Clone, Copy, Debug)]
pub enum RescheduleReason {
    /// The currently running thread has been preempted causing it to be re-added to the scheduler.
    Preempted,
    /// A scheduler tick occurred causing the currently running thread to be readded to the scheduler.
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
    current_priority: Priority,
}

impl SchedulerAlgorithmThreadState {
    #[allow(clippy::new_without_default)]
    #[must_use]
    pub const fn new(current_priority: Priority) -> Self {
        Self { current_priority }
    }
}

/// The algorithm used for determining which thread to run next.
pub struct SchedulerAlgorithm<K: Kernel> {
    // Array of run queues, one for each priority level.
    // Index 0 is the lowest priority, NUM_PRIORITIES - 1 is the highest.
    run_queues: [RunQueue<K>; Priority::NUM_PRIORITIES],
    // Bitmask to track non-empty run queues.
    ready_bitmask: PriorityBitmask,
}

unsafe impl<K: Kernel> Sync for SchedulerAlgorithm<K> {}
unsafe impl<K: Kernel> Send for SchedulerAlgorithm<K> {}

impl<K: Kernel> SchedulerAlgorithm<K> {
    #[allow(clippy::new_without_default)]
    #[must_use]
    pub const fn new() -> Self {
        // Initialize the array of ForeignLists. There are a few limitations
        // from running in a `const` context that make this more complicated
        // than usual.
        // - ForeignList does not implement `Copy`
        // - `Default::default` and `core::array::from_fn` don't support `const`
        // - `const` `for` loops are an unstable feature
        //
        // Instead a combination of `MaybeUninit`, `while`, and `transmute` are
        // used.
        let run_queues = {
            let mut queues =
                [const { MaybeUninit::<RunQueue<K>>::uninit() }; Priority::NUM_PRIORITIES];

            let mut i = 0;
            while i < Priority::NUM_PRIORITIES {
                queues[i].write(ForeignList::new());
                i += 1;
            }

            // SAFETY: All elements have been initialized in the loop above.
            unsafe {
                core::mem::transmute::<
                    [MaybeUninit<RunQueue<K>>; Priority::NUM_PRIORITIES],
                    [RunQueue<K>; Priority::NUM_PRIORITIES],
                >(queues)
            }
        };

        Self {
            run_queues,
            ready_bitmask: PriorityBitmask::new(),
        }
    }

    pub fn schedule_thread(&mut self, mut thread: ForeignBox<Thread<K>>, reason: RescheduleReason) {
        thread.owner = ThreadOwner::Scheduler;
        let priority = thread.algorithm_state.current_priority;
        self.ready_bitmask.set_priority(priority);
        let run_queue = &mut self.run_queues[priority as usize];
        match reason {
            RescheduleReason::Preempted | RescheduleReason::Terminated => {
                run_queue.push_front(thread);
            }
            RescheduleReason::Started | RescheduleReason::Ticked | RescheduleReason::Woken => {
                run_queue.push_back(thread);
            }
        }
    }

    pub fn get_next_thread(&mut self) -> Option<ForeignBox<Thread<K>>> {
        let priority = self.ready_bitmask.get_highest_priority()?;

        let run_queue = &mut self.run_queues[priority as usize];
        let thread = run_queue.pop_head();
        if run_queue.is_empty() {
            self.ready_bitmask.clear_priority(priority);
        }
        thread
    }
}
