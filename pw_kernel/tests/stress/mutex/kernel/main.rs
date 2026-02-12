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

//! # Kernel Mutex Stress Test
//!
//! Spawns three threads which continuously acquire a `Mutex<K, u64>`, increment
//! the counter, and release the mutex.  The main thread continuously acquire's
//! the mutex, reads the counter, and prints a message if the value is a multiple
//! of 1000.
//!
//! This stress tests:
//! - Mutexes
//! - WaitQueues
//! - Common and arch specific context switching code
//!
//! Expected Results: Test continues to make forward progress and does not crash
//! or panic.
#![no_std]

use kernel::scheduler::Priority;
use kernel::scheduler::thread::{self, StackStorage, StackStorageExt as _, Thread};
use kernel::sync::mutex::Mutex;
use kernel::{Duration, Kernel};
use kernel_config::{KernelConfig, KernelConfigInterface};
use pw_log::{error, info};
use pw_status::Result;

pub struct TestThread<K: Kernel> {
    thread: Thread<K>,
    stack: StackStorage<{ KernelConfig::KERNEL_STACK_SIZE_BYTES }>,
}

pub struct AppState<K: Kernel> {
    thread_1: TestThread<K>,
    thread_2: TestThread<K>,
    thread_3: TestThread<K>,
    test_counter: Mutex<K, u64>,
}

impl<K: Kernel> AppState<K> {
    pub const fn new(kernel: K) -> AppState<K> {
        AppState {
            thread_1: TestThread {
                thread: Thread::new("mutex thread 1", Priority::DEFAULT_PRIORITY),
                stack: StackStorage::ZEROED,
            },
            thread_2: TestThread {
                thread: Thread::new("mutex thread 2", Priority::DEFAULT_PRIORITY),
                stack: StackStorage::ZEROED,
            },
            thread_3: TestThread {
                thread: Thread::new("mutex thread 3", Priority::DEFAULT_PRIORITY),
                stack: StackStorage::ZEROED,
            },
            test_counter: Mutex::new(kernel, 0),
        }
    }
}

#[derive(Clone, Copy)]
struct TestThreadArgs<'a, K: Kernel> {
    thread_index: usize,
    counter: &'a Mutex<K, u64>,
}

pub fn main<K: Kernel>(kernel: K, state: &'static mut AppState<K>) -> Result<()> {
    let thread_1_args = TestThreadArgs {
        thread_index: 1,
        counter: &state.test_counter,
    };
    let thread_1 = thread::init_thread_in(
        kernel,
        &mut state.thread_1.thread,
        &mut state.thread_1.stack,
        "1",
        Priority::DEFAULT_PRIORITY,
        increment_thread_entry,
        &thread_1_args,
    );

    let thread_2_args = TestThreadArgs {
        thread_index: 2,
        counter: &state.test_counter,
    };
    let thread_2 = thread::init_thread_in(
        kernel,
        &mut state.thread_2.thread,
        &mut state.thread_2.stack,
        "2",
        Priority::DEFAULT_PRIORITY,
        increment_thread_entry,
        &thread_2_args,
    );

    let thread_3_args = TestThreadArgs {
        thread_index: 3,
        counter: &state.test_counter,
    };
    let thread_3 = thread::init_thread_in(
        kernel,
        &mut state.thread_3.thread,
        &mut state.thread_3.stack,
        "3",
        Priority::DEFAULT_PRIORITY,
        increment_thread_entry,
        &thread_3_args,
    );

    kernel::start_thread(kernel, thread_1);
    kernel::start_thread(kernel, thread_2);
    kernel::start_thread(kernel, thread_3);

    loop {
        let deadline = kernel.now() + Duration::from_millis(600);
        let Ok(counter) = state.test_counter.lock_until(deadline) else {
            error!("Observer: Timeout");
            continue;
        };
        if *counter % 1000 == 0 {
            info!("Counter value {}", *counter as u64);
        }

        drop(counter);
    }
}

fn increment_thread_entry<K: Kernel>(_kernel: K, args: &TestThreadArgs<K>) {
    info!("Increment Thread {}", args.thread_index as usize);
    loop {
        let mut counter = args.counter.lock();
        *counter = (*counter).wrapping_add(1);
        drop(counter);
    }
}
