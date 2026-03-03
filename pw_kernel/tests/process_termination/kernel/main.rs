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
#![no_std]

use core::mem::MaybeUninit;
use core::ptr::NonNull;

use foreign_box::ForeignBox;
#[cfg(feature = "user_space")]
use kernel::object::{KernelObject, ObjectTable};
use kernel::scheduler::thread::{self, Process, ProcessRef, StackStorage, Thread};
use kernel::{Duration, Instant, Kernel, Priority, StackStorageExt};
use memory_config::MemoryConfig;
use pw_log::info;
use pw_status::{Error, Result};

const TEST_THREAD_STACK_SIZE: usize = 2048;

const NUM_THREADS: usize = 3;

pub struct TestState<K: Kernel> {
    termination_from_outside_threads: [Thread<K>; NUM_THREADS],
    termination_from_outside_stacks: [StackStorage<TEST_THREAD_STACK_SIZE>; NUM_THREADS],
    termination_from_outside_process: MaybeUninit<Process<K>>,

    termination_from_inside_threads: [Thread<K>; NUM_THREADS],
    termination_from_inside_stacks: [StackStorage<TEST_THREAD_STACK_SIZE>; NUM_THREADS],
    termination_from_inside_process: MaybeUninit<Process<K>>,
}

#[cfg(feature = "user_space")]
struct NullObjectTable;

#[cfg(feature = "user_space")]
impl<K: Kernel> ObjectTable<K> for NullObjectTable {
    fn get_object(
        &self,
        _kernel: K,
        _handle: u32,
    ) -> Option<foreign_box::ForeignRc<K::AtomicUsize, dyn KernelObject<K>>> {
        None
    }
}

#[cfg(feature = "user_space")]
static mut NULL_OBJECT_TABLE: NullObjectTable = NullObjectTable;

impl<K: Kernel> TestState<K> {
    pub const fn new(_kernel: K) -> Self {
        Self {
            termination_from_outside_threads: [const {
                Thread::new("outside test thread", Priority::DEFAULT_PRIORITY)
            }; NUM_THREADS],
            termination_from_outside_stacks: [StackStorage::ZEROED; NUM_THREADS],
            termination_from_outside_process: MaybeUninit::uninit(),
            termination_from_inside_threads: [const {
                Thread::new("inside test thread", Priority::DEFAULT_PRIORITY)
            }; NUM_THREADS],
            termination_from_inside_stacks: [StackStorage::ZEROED; NUM_THREADS],
            termination_from_inside_process: MaybeUninit::uninit(),
        }
    }
}

pub fn run_test<K: Kernel, F: FnOnce() -> Result<()>>(test_name: &str, test_fn: F) -> Result<()> {
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
    run_test::<K, _>("Termination from Outside", || {
        test_termination_from_outside(
            kernel,
            &mut state.termination_from_outside_process,
            &mut state.termination_from_outside_threads,
            &state.termination_from_outside_stacks,
        )
    })?;

    run_test::<K, _>("Termination from Inside", || {
        test_termination_from_inside(
            kernel,
            &mut state.termination_from_inside_process,
            &mut state.termination_from_inside_threads,
            &state.termination_from_inside_stacks,
        )
    })?;

    Ok(())
}

fn test_termination_from_outside<K: Kernel>(
    kernel: K,
    process_storage: &'static mut MaybeUninit<Process<K>>,
    threads: &'static mut [Thread<K>],
    stacks: &[StackStorage<TEST_THREAD_STACK_SIZE>],
) -> Result<()> {
    info!("🔄 ├─ Starting termination from outside test");

    // 1. Register the process
    #[cfg(feature = "user_space")]
    let object_table_ptr: *mut dyn ObjectTable<K> = &raw mut NULL_OBJECT_TABLE;

    // Initialize process
    let process: &mut Process<K> = process_storage.write(Process::new(
        "test process outside",
        <<K::ThreadState as thread::ThreadState>::MemoryConfig as MemoryConfig>::KERNEL_THREAD_MEMORY_CONFIG,
        #[cfg(feature = "user_space")]
        unsafe { ForeignBox::new(NonNull::new_unchecked(object_table_ptr)) },
    ));

    process.register(kernel);

    // 2. Create a ProcessRef
    let process_ref = ProcessRef::new(NonNull::from(&*process), kernel);

    // 3. Initialize threads in this process
    for (i, thread) in threads.iter_mut().enumerate() {
        thread.initialize_kernel_thread_for_process(
            kernel,
            thread::Stack::from_slice(&stacks[i].stack),
            process_ref.clone(),
            thread_entry,
            0,
        );
    }

    // 4. Start threads
    let mut thread_refs: [Option<thread::ThreadRef<K>>; NUM_THREADS] =
        [const { None }; NUM_THREADS];

    for (i, thread) in threads.iter_mut().enumerate() {
        let thread_box = ForeignBox::from(thread);
        thread_refs[i] = Some(kernel::start_thread(kernel, thread_box));
    }

    info!("🔄 ├─ Threads started in separate process");

    // 5. Terminate process
    info!("🔄 ├─ Terminating process");
    pw_assert::assert!(process_ref.terminate(kernel).is_ok());

    // 6. Wait for threads to be terminated
    let deadline = kernel.now() + Duration::from_millis(1000);
    loop {
        let all_terminated = thread_refs.iter().fold(true, |acc, thread| {
            let Some(t) = thread.as_ref() else {
                pw_assert::panic!("Thread ref is None unexpectedly");
            };
            acc && t.is_terminating(kernel)
        });

        if all_terminated {
            break;
        }

        if kernel.now() > deadline {
            info!("❌ ├─ Timeout waiting for thread termination");
            return Err(Error::DeadlineExceeded);
        }
        let _ = kernel::sleep_until(kernel, kernel.now() + Duration::from_millis(10));
    }

    info!("🔄 ├─ All threads terminated");

    // 7. Join threads to clean up
    for thread in &mut thread_refs {
        let Some(thread_ref) = thread.take() else {
            pw_assert::panic!("Thread ref is None during join");
        };
        let thread = thread_ref.join(kernel)?;
        let _ = thread.consume();
    }

    // 8. Verify process is terminated
    pw_assert::assert!(process_ref.get_state() == thread::ProcessState::Terminated);

    Ok(())
}

fn test_termination_from_inside<K: Kernel>(
    kernel: K,
    process_storage: &'static mut MaybeUninit<Process<K>>,
    threads: &'static mut [Thread<K>],
    stacks: &[StackStorage<TEST_THREAD_STACK_SIZE>],
) -> Result<()> {
    info!("🔄 ├─ Starting termination from inside test");

    // 1. Register the process
    #[cfg(feature = "user_space")]
    let object_table_ptr: *mut dyn ObjectTable<K> = &raw mut NULL_OBJECT_TABLE;

    // Initialize process
    let process: &mut Process<K> = process_storage.write(Process::new(
        "test process inside",
        <<K::ThreadState as thread::ThreadState>::MemoryConfig as MemoryConfig>::KERNEL_THREAD_MEMORY_CONFIG,
        #[cfg(feature = "user_space")]
        unsafe { ForeignBox::new(NonNull::new_unchecked(object_table_ptr)) },
    ));

    process.register(kernel);

    // 2. Create a ProcessRef
    let process_ref = ProcessRef::new(NonNull::from(&*process), kernel);

    // 3. Initialize threads in this process
    for (i, thread) in threads.iter_mut().enumerate() {
        if i == 0 {
            // This thread will trigger termination.
            // Pass process_ref address as argument.
            // SAFETY: process_ref lives on our stack until we return, which happens after join.
            let arg = &process_ref as *const _ as usize;
            thread.initialize_kernel_thread_for_process(
                kernel,
                thread::Stack::from_slice(&stacks[i].stack),
                process_ref.clone(),
                thread_entry_terminator,
                arg,
            );
        } else {
            thread.initialize_kernel_thread_for_process(
                kernel,
                thread::Stack::from_slice(&stacks[i].stack),
                process_ref.clone(),
                thread_entry,
                0,
            );
        }
    }

    // 4. Start threads
    let mut thread_refs: [Option<thread::ThreadRef<K>>; NUM_THREADS] =
        [const { None }; NUM_THREADS];

    for (i, thread) in threads.iter_mut().enumerate() {
        let thread_box = ForeignBox::from(thread);
        thread_refs[i] = Some(kernel::start_thread(kernel, thread_box));
    }

    info!("🔄 ├─ Threads started in separate process");

    // 5. Wait for threads to be terminated
    let deadline = kernel.now() + Duration::from_millis(1000);
    loop {
        let all_terminated = thread_refs.iter().fold(true, |acc, thread| {
            let Some(t) = thread.as_ref() else {
                pw_assert::panic!("Thread ref is None unexpectedly");
            };
            acc && t.is_terminating(kernel)
        });

        if all_terminated {
            break;
        }

        if kernel.now() > deadline {
            info!("❌ ├─ Timeout waiting for thread termination");
            return Err(Error::DeadlineExceeded);
        }
        let _ = kernel::sleep_until(kernel, kernel.now() + Duration::from_millis(10));
    }

    info!("🔄 ├─ All threads terminated");

    // 6. Join threads to clean up
    for thread in &mut thread_refs {
        let Some(thread_ref) = thread.take() else {
            pw_assert::panic!("Thread ref is None during join");
        };
        let thread = thread_ref.join(kernel)?;
        let _ = thread.consume();
    }

    // 7. Verify process is terminated
    pw_assert::assert!(process_ref.get_state() == thread::ProcessState::Terminated);

    Ok(())
}

fn thread_entry<K: Kernel>(kernel: K, _arg: usize) {
    info!("🔄 ├─ Test thread running, sleeping forever");
    let res = kernel::sleep_until(kernel, Instant::MAX);
    pw_assert::assert!(res == Err(Error::Cancelled));
}

fn thread_entry_terminator<K: Kernel>(kernel: K, arg: usize) {
    info!("🔄 ├─ Terminator thread running, terminating process");
    let process_ref = unsafe { &*(arg as *const ProcessRef<K>) };
    pw_assert::assert!(process_ref.terminate(kernel).is_ok());

    // Sleep until terminated
    let res = kernel::sleep_until(kernel, Instant::MAX);
    pw_assert::assert!(res == Err(Error::Cancelled));
}
