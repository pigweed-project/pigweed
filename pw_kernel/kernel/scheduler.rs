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

//! Kernel Scheduler / Context Switching engine
//!
//! The module provides several interfaces for managing context switches. Each is
//! different in its contract:
//! - `block()` is used when a thread is taken out of the scheduler algorithm
//!   and is managed by another source such as a wait queue. These threads
//!   should be in the [`State::WaitingInterruptible`] or
//!   [`State::WaitingNonInterruptible`] state.
//! - `try_reschedule()` is used when there is a change to the scheduling state
//!   (such as waking a thread). Depending on the local thread's
//!   [`PreemptDisableGuard`] state and the scheduler state, this may trigger an
//!   immediate context switch or it may defer it. This is useful for code which
//!   may or may not be called in an interrupt context.
//! - `try_deferred_reschedule()` is used to trigger a context switch if one was
//!   deferred by `try_reschedule()`. Notably, this does not guarantee a context
//!   switch if the [`PreemptDisableGuard`] state is not active but does
//!   guarantee forward progress. This comes into play for architectures like
//!   M-profile ARM which handles the context switch in an exception handler
//!   after an interrupt has returned.

pub(crate) mod algorithm;
pub(crate) mod core;
pub(crate) mod locks;
pub(crate) mod priority;
pub(crate) mod priority_bitmask;
pub(crate) mod process;
pub(crate) mod thread;
pub(crate) mod timer;
pub(crate) mod wait_queue;

pub(crate) use core::block;
pub use core::{
    JoinResult, PreemptDisableGuard, ProcessJoinResult, ProcessTryJoinResult, SchedulerState,
    ThreadLocalState, TryJoinResult, add_process, bootstrap_scheduler, break_scheduler_lock,
    exit_thread, handle_terminal_exception, initialize, sleep_until, start_thread, tick,
    try_deferred_reschedule, yield_timeslice,
};

pub use algorithm::RescheduleReason;
pub use locks::{SchedLockGuard, WaitQueueLock, WaitQueueLockGuard};
pub use priority::Priority;
pub use process::{Process, ProcessHandle};
pub use thread::{
    Stack, StackStorage, StackStorageExt, State, Thread, ThreadArg, ThreadHandle, ThreadState,
    init_thread_in,
};
pub use timer::TimerQueue;
pub use wait_queue::{WaitQueue, WaitType, WakeResult};
