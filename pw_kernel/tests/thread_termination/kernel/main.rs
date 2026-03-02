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

use foreign_box::ForeignBox;
use kernel::scheduler::thread::{self, StackStorage, StackStorageExt as _, Thread, ThreadRef};
use kernel::sync::event::{Event, EventConfig};
use kernel::sync::mutex::Mutex;
use kernel::{Duration, Instant, Kernel, Priority};
use pw_log::info;
use pw_status::{Error, Result};

// Test cases needed:
// * Thread in interruptible wait (event/sleep) returns immediately upon being terminated.
// * Thread in non-interruptible wait (mutex) does not return immediately.
// * Interuptible waits will immediately return when thread is in terminated state.
// * non-Interuptible waits will still wait when thread is in terminated state.

const TEST_THREAD_STACK_SIZE: usize = 2048;
pub struct TestState<K: Kernel> {
    thread: Thread<K>,
    stack: StackStorage<TEST_THREAD_STACK_SIZE>,
    utility_thread: Thread<K>,
    utility_stack: StackStorage<TEST_THREAD_STACK_SIZE>,
    event: Event<K>,
    mutex: Mutex<K, u32>,
    thread_ref_event: Event<K>,
}

impl<K: Kernel> TestState<K> {
    pub const fn new(kernel: K) -> Self {
        Self {
            thread: Thread::new("test thread", Priority::DEFAULT_PRIORITY),
            stack: StackStorage::ZEROED,
            utility_thread: Thread::new("utility thread", Priority::DEFAULT_PRIORITY),
            utility_stack: StackStorage::ZEROED,
            event: Event::new(kernel, EventConfig::ManualReset),
            mutex: Mutex::new(kernel, 0),
            thread_ref_event: Event::new(kernel, EventConfig::ManualReset),
        }
    }
}

pub fn wait_for_thread_state<K: Kernel>(
    kernel: K,
    thread: &ThreadRef<K>,
    state: thread::State,
) -> Result<()> {
    let deadline = kernel.now() + Duration::from_millis(500);
    while thread.get_state(kernel) != state {
        let now = kernel.now();
        if now > deadline {
            return Err(Error::DeadlineExceeded);
        }
        let _ = kernel::sleep_until(kernel, now + Duration::from_millis(10));
    }

    Ok(())
}

pub fn run_test<K: Kernel, F: FnOnce() -> Result<ForeignBox<Thread<K>>>>(
    test_name: &str,
    test_fn: F,
) -> Result<ForeignBox<Thread<K>>> {
    info!("🔄 [{}] RUNNING", test_name as &str);
    let res = test_fn();

    match &res {
        Ok(_) => info!("✅ └─ PASSED", test_name as &str),
        Err(e) => {
            info!("❌ ├─ FAILED", test_name as &str);
            info!("❌ └─ status code: {}", *e as u32);
        }
    }

    res
}

pub fn test_main<K: Kernel>(kernel: K, state: &'static mut TestState<K>) -> Result<()> {
    let thread = run_test("Terminate Sleep", || {
        terminate_sleep_test(kernel, &mut state.thread, &mut state.stack)
    })?;

    let thread = run_test("Signaled Termination", || {
        signaled_termination_test(kernel, thread, &state.event)
    })?;

    let thread = run_test("Mutex", || mutex_test(kernel, thread, &state.mutex))?;

    let thread = run_test("Tread Ref Drop", || {
        thread_ref_drop_test(
            kernel,
            thread,
            &mut state.utility_thread,
            &mut state.utility_stack,
            &mut state.thread_ref_event,
        )
    })?;

    thread.consume();

    Ok(())
}

// Spawns a thread that sleeps indefinitely then terminates it.  Sleeps
// use interruptible waits so this tests:
// - Terminating a thread causes an interruptible wait to exit with `Error::Canceled`.
// - Once in the terminating state, and interruptible wait will return immediately
//   with `Error::Canceled`.
fn terminate_sleep_test<K: Kernel>(
    kernel: K,
    thread: &'static mut Thread<K>,
    stack: &'static mut StackStorage<TEST_THREAD_STACK_SIZE>,
) -> Result<ForeignBox<Thread<K>>> {
    // As the first test, this initializes the thread for the first time as
    // as opposed to taking a `ForeignBox<Thread<K>>` like the other tests.
    let thread = thread::init_thread_in(
        kernel,
        thread,
        stack,
        "Termination Thread",
        Priority::DEFAULT_PRIORITY,
        terminate_sleep_thread_entry,
        0,
    );
    let mut thread_ref = kernel::start_thread(kernel, thread);

    // Spin until the termination thread is in the sleep's wait queue.
    wait_for_thread_state(kernel, &thread_ref, thread::State::WaitingInterruptible)?;
    info!("🔄 ├─ Termination thread observed in waiting state, terminating");

    thread_ref.terminate(kernel)?;

    info!("🔄 ├─ Joining thread");

    let thread = thread_ref.join(kernel)?;
    info!("🔄 ├─ Joined");

    Ok(thread)
}

fn terminate_sleep_thread_entry<K: Kernel>(kernel: K, _arg: usize) {
    // Sleep forever.  This ensure that this thread is in a wait queue.
    info!("🔄 ├─ Termination thread starting and sleeping");
    let res = kernel::sleep_until(kernel, Instant::MAX);
    pw_assert::assert!(res == Err(Error::Cancelled));

    // Successive sleeps should return `Error::Canceled` immediately.
    info!("🔄 ├─ Attempting second sleep");
    let res = kernel::sleep_until(kernel, Instant::MAX);
    pw_assert::assert!(res == Err(Error::Cancelled));

    info!("🔄 ├─ Termination thread sleep canceled, exiting");
}

// Spawns a thread that waits on an event then signals that event causing the
// thread to exit normally.  This tests:
// - Normal thread exit by returning from its entry function.
fn signaled_termination_test<K: Kernel>(
    kernel: K,
    mut thread: ForeignBox<Thread<K>>,
    event: &'static Event<K>,
) -> Result<ForeignBox<Thread<K>>> {
    info!("🔄 ├─ Starting signaled thread");
    thread.re_initialize_kernel_thread(kernel, signaled_sleep_entry, event);
    let thread_ref = kernel::start_thread(kernel, thread);

    wait_for_thread_state(kernel, &thread_ref, thread::State::WaitingInterruptible)?;

    info!("🔄 ├─ Signaling signaled thread");
    event.get_signaler().signal();

    info!("🔄 ├─ Joining signaled thread");
    let thread = thread_ref.join(kernel)?;
    info!("🔄 ├─ Joined");

    Ok(thread)
}

fn signaled_sleep_entry<K: Kernel>(_kernel: K, event: &'static Event<K>) {
    info!("🔄 ├─ Signaled thread starting, waiting");
    let res = event.wait();
    pw_assert::assert!(res.is_ok());

    info!("🔄 ├─ Signaled thread exiting");
}

fn mutex_test<K: Kernel>(
    kernel: K,
    mut thread: ForeignBox<Thread<K>>,
    mutex: &'static Mutex<K, u32>,
) -> Result<ForeignBox<Thread<K>>> {
    info!("🔄 ├─ Acquiring mutex");
    let guard = mutex.lock();

    info!("🔄 ├─ Starting mutex thread");
    thread.re_initialize_kernel_thread(kernel, mutex_entry, mutex);
    let mut thread_ref = kernel::start_thread(kernel, thread);

    wait_for_thread_state(kernel, &thread_ref, thread::State::WaitingNonInterruptible)?;
    info!("🔄 ├─  Observed in waiting state, terminating");

    thread_ref.terminate(kernel)?;

    // Mutexes are non-interruptible so the thread should still be in the waiting
    // state after the termination request.
    pw_assert::assert!(thread_ref.get_state(kernel) == thread::State::WaitingNonInterruptible);

    info!("🔄 ├─ Releasing mutex");
    drop(guard);

    info!("🔄 ├─ Joining mutex thread");
    let thread = thread_ref.join(kernel)?;
    info!("🔄 ├─ Joined");

    // Confirm that the mutex thread was able to acquire the mutex after the
    // termination request and change its enclosed value.
    let val = *mutex.lock();
    pw_assert::eq!(val as u32, 1 as u32);

    Ok(thread)
}

fn mutex_entry<K: Kernel>(kernel: K, mutex: &'static Mutex<K, u32>) {
    // Since this thread is started with the mutex held, this should block.
    info!("🔄 ├─ Mutex thread attempting to lock mutex");
    let guard = mutex.lock();
    info!("🔄 ├─ Mutex thread acquired mutex");
    drop(guard);

    // Sleeps are interruptible so this will either early exit or be interrupted
    // when the test requests the threads termination.
    info!("🔄 ├─ Mutex thread waiting to be terminated");
    let res = kernel::sleep_until(kernel, Instant::MAX);
    pw_assert::assert!(res == Err(Error::Cancelled));

    // Mutexes are non-interruptible so the thread can still acquire it while
    // in the termination state.
    *mutex.lock() = 1;
}

struct ThreadRefUtilityArgs<'a, K: Kernel> {
    test_thread_ref: Option<ThreadRef<K>>,
    event: &'a mut Event<K>,
}

fn thread_ref_drop_test<K: Kernel>(
    kernel: K,
    mut test_thread: ForeignBox<Thread<K>>,
    utility_thread: &'static mut Thread<K>,
    utility_stack: &'static mut StackStorage<TEST_THREAD_STACK_SIZE>,
    event: &'static mut Event<K>,
) -> Result<ForeignBox<Thread<K>>> {
    info!("🔄 ├─ Starting signaled thread");
    test_thread.re_initialize_kernel_thread(kernel, thread_ref_drop_entry, 0);
    let test_thread_ref = kernel::start_thread(kernel, test_thread);

    let signaler = event.get_signaler();
    let mut utility_args = ThreadRefUtilityArgs {
        test_thread_ref: Some(test_thread_ref.clone()),
        event,
    };
    let utility_thread = thread::init_thread_in(
        kernel,
        utility_thread,
        utility_stack,
        "Utility Thread",
        Priority::DEFAULT_PRIORITY,
        thread_ref_drop_utility_entry,
        &mut utility_args,
    );
    let utility_thread_ref = kernel::start_thread(kernel, utility_thread);

    // Wait for the test thread to exit and be in the termination queue waiting
    // to be joined.
    wait_for_thread_state(kernel, &test_thread_ref, thread::State::Terminated)?;

    // The thread should also be marked as terminating.
    pw_assert::assert!(test_thread_ref.is_terminating(kernel));

    // An immediate attempt to join should time out because the utility thread
    // still holds a reference to it.
    info!("🔄 ├─ Attempting initial join of test thread");
    let test_thread_ref =
        match test_thread_ref.join_until(kernel, kernel.now() + Duration::from_millis(500)) {
            kernel::scheduler::JoinResult::Joined(_) => {
                pw_assert::panic!("Initial join should have timed out");
            }
            kernel::scheduler::JoinResult::Err { error, thread } => {
                pw_assert::assert!(error == Error::DeadlineExceeded);
                thread
            }
        };
    info!("🔄 ├─ Join timed out as expected. Signaling utility thread to drop ref");
    signaler.signal();

    info!("🔄 ├─ Joining test thread");
    let test_thread = test_thread_ref.join(kernel)?;
    info!("🔄 ├─ Joined");

    info!("🔄 ├─ Joining utility thread");
    let utility_thread = utility_thread_ref.join(kernel)?;
    utility_thread.consume();
    info!("🔄 ├─ Joined");

    Ok(test_thread)
}

fn thread_ref_drop_entry<K: Kernel>(_kernel: K, _arg: usize) {
    info!("🔄 ├─ Test thread started and exiting");
}

fn thread_ref_drop_utility_entry<K: Kernel>(kernel: K, args: &mut ThreadRefUtilityArgs<K>) {
    info!("🔄 ├─ Utility thread waiting for signal");
    let _ = args.event.wait();
    info!("🔄 ├─ Got signal to drop thread ref.  Waiting for test to enter join()");
    let _ = kernel::sleep_until(kernel, kernel.now() + Duration::from_millis(500));

    info!("🔄 ├─ Dropping thread ref");
    let _ = args.test_thread_ref.take();
    info!("🔄 ├─ Exiting");
}
