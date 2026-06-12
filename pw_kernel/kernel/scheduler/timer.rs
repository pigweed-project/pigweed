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

use foreign_box::ForeignBox;
use list::{ForeignList, Link};
use time::Instant;

use crate::Kernel;

list::define_adapter!(pub TimerCallbackListAdapter<K: Kernel> => Timer<K>::link);

pub trait TimerCallback<K: Kernel> {
    fn callback(
        &mut self,
        kernel: K,
        timer: ForeignBox<Timer<K>>,
        now: Instant<K::Clock>,
    ) -> Option<ForeignBox<Timer<K>>>;
}

impl<K, F> TimerCallback<K> for F
where
    K: Kernel,
    F: FnMut(K, ForeignBox<Timer<K>>, Instant<K::Clock>) -> Option<ForeignBox<Timer<K>>>,
{
    fn callback(
        &mut self,
        kernel: K,
        timer: ForeignBox<Timer<K>>,
        now: Instant<K::Clock>,
    ) -> Option<ForeignBox<Timer<K>>> {
        self(kernel, timer, now)
    }
}

#[allow(dead_code)]
pub struct Timer<K: Kernel> {
    deadline: Instant<K::Clock>,
    // XXX: make private
    pub callback: Option<ForeignBox<dyn TimerCallback<K>>>,
    link: Link,
}

impl<K: Kernel> Timer<K> {
    #[allow(dead_code)]
    #[must_use]
    pub fn new(deadline: Instant<K::Clock>, callback: ForeignBox<dyn TimerCallback<K>>) -> Self {
        Self {
            deadline,
            callback: Some(callback),
            link: Link::new(),
        }
    }

    pub fn set(&mut self, deadline: Instant<K::Clock>) {
        self.deadline = deadline
    }
}

#[allow(dead_code)]
pub struct TimerQueue<K: Kernel> {
    queue: ForeignList<Timer<K>, TimerCallbackListAdapter<K>>,
}

unsafe impl<K: Kernel> Sync for TimerQueue<K> {}
unsafe impl<K: Kernel> Send for TimerQueue<K> {}

impl<K: Kernel> TimerQueue<K> {
    #[must_use]
    pub const fn new() -> Self {
        Self {
            queue: ForeignList::new(),
        }
    }

    pub fn get_next_exipred_callback(
        &mut self,
        now: Instant<K::Clock>,
    ) -> Option<ForeignBox<Timer<K>>> {
        // TODO - konkers: An optimization opportunity exists here to implement
        // a something like `peek_front(|front| front.deadline > now)` which
        // would eliminate the popping and re-pushing of the non-expired
        // callback.
        let callback = self.queue.pop_head()?;
        if callback.deadline > now {
            self.queue.push_front(callback);
            return None;
        }

        Some(callback)
    }
}

#[allow(dead_code)]
pub fn schedule_timer<K: Kernel>(kernel: K, timer: ForeignBox<Timer<K>>) {
    let mut timer_queue = kernel.get_timer_queue().lock(kernel);
    timer_queue
        .queue
        .sorted_insert_by_key(timer, |timer| timer.deadline);
}

/// # Safety
///
/// `timer` must point to a valid [`TimerCallback`] object which is in the
/// timer queue.
pub unsafe fn cancel_timer<K: Kernel>(
    kernel: K,
    timer: NonNull<Timer<K>>,
) -> Option<ForeignBox<Timer<K>>> {
    let mut timer_queue = kernel.get_timer_queue().lock(kernel);
    unsafe { timer_queue.queue.remove_element(timer) }
}

/// # Safety
///
/// `timer` must point to a valid [`TimerCallback`] object which is in the
/// timer queue.
pub unsafe fn cancel_and_consume_timer<K: Kernel>(kernel: K, timer: NonNull<Timer<K>>) {
    if let Some(mut callback) = unsafe { cancel_timer(kernel, timer) } {
        if let Some(callback) = callback.callback.take() {
            callback.consume();
        }
        callback.consume();
    }
}

#[allow(dead_code)]
pub fn process_queue<K: Kernel>(kernel: K, now: Instant<K::Clock>) {
    let mut timer_queue = kernel.get_timer_queue().lock(kernel);
    while let Some(mut callback) = timer_queue.get_next_exipred_callback(now) {
        // Drop the timer queue lock before processing callbacks.  This is
        // Safe to do because the callback is already removed from the queue.
        drop(timer_queue);

        let Some(mut callback_fn) = callback.callback.take() else {
            pw_assert::panic!("No callback function found on timer");
        };
        match callback_fn.callback(kernel, callback, now) {
            Some(mut next_callback) => {
                next_callback.callback = Some(callback_fn);
                schedule_timer(kernel, next_callback);
            }
            None => {
                let _ = callback_fn.consume();
            }
        }

        // Require timer queue lock.
        timer_queue = kernel.get_timer_queue().lock(kernel);
    }
}
