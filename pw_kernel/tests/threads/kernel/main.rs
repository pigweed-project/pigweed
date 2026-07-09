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
#![no_std]

use kernel::scheduler::{self, Priority, StackStorage, StackStorageExt as _, Thread};
use kernel::sync::event::{Event, EventConfig, EventSignaler};
use kernel::sync::mutex::Mutex;
use kernel::{Duration, Kernel};
use kernel_config::{KernelConfig, KernelConfigInterface};
use pw_status::Result;

pub struct AppState<K: Kernel> {
    thread: Thread<K>,
    stack: StackStorage<{ KernelConfig::KERNEL_STACK_SIZE_BYTES }>,
    test_counter: Mutex<K, u64>,
    thread_a_done_event: Event<K>,
}

impl<K: Kernel> AppState<K> {
    pub const fn new(kernel: K) -> AppState<K> {
        AppState {
            thread: Thread::new(
                "",
                Priority::DEFAULT_PRIORITY,
                kernel::scheduler::Stack::new(),
            ),
            stack: StackStorage::ZEROED,
            test_counter: Mutex::new(kernel, 0),
            thread_a_done_event: Event::new(kernel, EventConfig::ManualReset),
        }
    }
}

struct ThreadAArgs<'a, K: Kernel> {
    test_counter: &'a Mutex<K, u64>,
    done_signaler: EventSignaler<K>,
}

pub fn main<K: Kernel>(kernel: K, state: &'static mut AppState<K>) -> Result<()> {
    test_logger::start("Kernel Threads Test");

    let thread_b_args = ThreadAArgs {
        test_counter: &state.test_counter,
        done_signaler: state.thread_a_done_event.get_signaler(),
    };

    let thread_b = scheduler::init_thread_in(
        kernel,
        &mut state.thread,
        &mut state.stack,
        "B",
        Priority::DEFAULT_PRIORITY,
        test_thread_entry_b,
        &thread_b_args,
    );

    kernel::start_thread(kernel, thread_b);

    test_logger::info!("Thread A re-using bootstrap thread");
    thread_a(kernel, &state.test_counter);

    let wait_result = state
        .thread_a_done_event
        .wait_until(kernel.now() + Duration::from_secs(1));

    match wait_result {
        Ok(()) => test_logger::passed("Kernel Threads Test"),
        Err(_err) => test_logger::failed("Kernel Threads Test"),
    }

    wait_result
}

fn test_thread_entry_b<K: Kernel>(kernel: K, args: &ThreadAArgs<K>) {
    test_logger::info!("Thread B starting");
    thread_b(kernel, args);
}

fn thread_a<K: Kernel>(kernel: K, test_counter: &Mutex<K, u64>) {
    // Verify that lock_until can be called from the same thread when already locked,
    // provided the deadline has already passed (instant/expired timeout).
    {
        let _guard = test_counter.lock();
        let res = test_counter.lock_until(kernel.now());
        pw_assert::assert!(res.is_err());
    }

    for _ in 0..3 {
        let mut counter = test_counter.lock();
        let _ = kernel::sleep_until(kernel, kernel.now() + Duration::from_secs(1));
        test_logger::info!("Thread A: Incrementing counter");
        *counter = (*counter).saturating_add(1);
    }
    test_logger::info!("Thread A: Done");
}

fn thread_b<K: Kernel>(kernel: K, args: &ThreadAArgs<K>) {
    for _ in 0..4 {
        let deadline = kernel.now() + Duration::from_millis(600);
        let Ok(counter) = args.test_counter.lock_until(deadline) else {
            test_logger::info!("Thread B: Timeout");
            continue;
        };
        test_logger::info!("Thread B: Counter value {}", *counter as u64);
        pw_assert::assert!(*counter < 4);

        drop(counter);
        // Give Thread A a chance to acquire the mutex.
        kernel::yield_timeslice(kernel);
    }
    test_logger::info!("Thread B: Done");
    args.done_signaler.signal();
}
