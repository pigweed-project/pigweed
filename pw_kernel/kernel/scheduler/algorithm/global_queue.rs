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
use crate::scheduler::Thread;
use crate::scheduler::algorithm::priority_queue_set::PriorityQueueSet;
use crate::scheduler::algorithm::{RescheduleReason, SchedulerAlgorithmInterface};
use crate::scheduler::thread::ThreadOwner;

/// Scheduler backend with a single global [`PriorityQueueSet`] shared by all
/// cores.
///
/// Preempted and terminating threads are placed at the head of their
/// priority's queue so they resume (or are reaped) before peers of equal
/// priority; started, ticked, and woken threads go to the tail for round-robin
/// fairness.
pub struct GlobalQueueScheduler<K: Kernel> {
    queue: PriorityQueueSet<K>,
}

impl<K: Kernel> GlobalQueueScheduler<K> {
    #[must_use]
    pub const fn new() -> Self {
        Self {
            queue: PriorityQueueSet::new(),
        }
    }
}

impl<K: Kernel> SchedulerAlgorithmInterface<K> for GlobalQueueScheduler<K> {
    #[inline]
    fn schedule_thread(&mut self, mut thread: ForeignBox<Thread<K>>, reason: RescheduleReason) {
        thread.owner = ThreadOwner::Scheduler;

        match reason {
            RescheduleReason::Preempted | RescheduleReason::Terminated => {
                self.queue.push_front(thread);
            }
            RescheduleReason::Started | RescheduleReason::Ticked | RescheduleReason::Woken => {
                self.queue.push_back(thread);
            }
        }
    }

    #[inline]
    fn pop_next_thread(&mut self) -> Option<ForeignBox<Thread<K>>> {
        self.queue.pop_head()
    }
}
