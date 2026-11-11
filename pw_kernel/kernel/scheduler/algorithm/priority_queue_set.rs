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

use core::mem::{self, MaybeUninit};
use core::ptr::NonNull;

use foreign_box::ForeignBox;
use list::ForeignList;

use crate::Kernel;
use crate::scheduler::algorithm::priority_bitmask::PriorityBitmask;
use crate::scheduler::thread::ThreadListAdapter;
use crate::scheduler::{Priority, Thread};

type RunQueue<K> = ForeignList<Thread<K>, ThreadListAdapter<K>>;

/// A set of intrusive per-priority run queues plus a bitmask of non-empty
/// priorities, giving constant-time `O(1)` push, pop, and removal.
///
/// A thread's priority is read from `algorithm_state.priority()` when it
/// is queued and must not change while it is linked here.
pub struct PriorityQueueSet<K: Kernel> {
    /// One run queue per priority level.
    ///
    /// Index 0 is the lowest priority, `NUM_PRIORITIES - 1` is the highest.
    run_queues: [RunQueue<K>; Priority::NUM_PRIORITIES],
    /// Bit `p` is set iff `run_queues[p]` is non-empty.
    ready_bitmask: PriorityBitmask,
}

impl<K: Kernel> PriorityQueueSet<K> {
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
                mem::transmute::<
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

    /// Enqueues `thread` at the head of its priority's run queue and marks that
    /// priority ready.
    #[inline]
    pub fn push_front(&mut self, thread: ForeignBox<Thread<K>>) {
        let priority = thread.algorithm_state.priority();
        self.ready_bitmask.set_priority(priority);
        self.run_queues[priority as usize].push_front(thread);
    }

    /// Enqueues `thread` at the tail of its priority's run queue and marks that
    /// priority ready.
    #[inline]
    pub fn push_back(&mut self, thread: ForeignBox<Thread<K>>) {
        let priority = thread.algorithm_state.priority();
        self.ready_bitmask.set_priority(priority);
        self.run_queues[priority as usize].push_back(thread);
    }

    /// Dequeues the head of the highest-priority non-empty run queue, clearing
    /// that priority's ready bit if the queue drains.
    ///
    /// Returns `None` if no thread is queued.
    #[inline]
    pub fn pop_head(&mut self) -> Option<ForeignBox<Thread<K>>> {
        let priority = self.ready_bitmask.get_highest_priority()?;
        let run_queue = &mut self.run_queues[priority as usize];
        let thread = run_queue.pop_head();
        if run_queue.is_empty() {
            self.ready_bitmask.clear_priority(priority);
        }
        thread
    }

    /// Removes a specific element from the queue set in strictly `O(1)` time.
    ///
    /// # Safety
    /// Caller must ensure `element` points to a valid `Thread<K>` currently
    /// linked in `self` at `element.as_ref().algorithm_state.priority()`.
    // Not yet used; provided for future users. Remove the `expect` when the
    // first caller lands.
    #[expect(dead_code)]
    #[inline]
    #[must_use]
    pub unsafe fn remove_element(&mut self, element: NonNull<Thread<K>>) -> ForeignBox<Thread<K>> {
        // SAFETY: The caller guarantees that `element` points to a valid `Thread<K>`.
        let priority = unsafe { element.as_ref() }.algorithm_state.priority();
        let run_queue = &mut self.run_queues[priority as usize];
        // SAFETY: The caller guarantees that `element` is a valid pointer to a
        // `Thread<K>` linked in `self.run_queues[priority as usize]`.
        let thread = unsafe { run_queue.remove_element_unchecked(element) };
        if run_queue.is_empty() {
            self.ready_bitmask.clear_priority(priority);
        }
        thread
    }
}
