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
//!   should be in the [`State::Waiting`] state.
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
use core::cell::UnsafeCell;
use core::ptr::NonNull;
use core::sync::atomic::Ordering;

use foreign_box::ForeignBox;
use list::*;
use memory_config::MemoryConfig as _;
use pw_atomic::{
    AtomicAdd, AtomicCompareExchange, AtomicFalse, AtomicLoad, AtomicStore, AtomicSub, AtomicZero,
};
use pw_log::info;
use pw_status::{Error, Result};
use time::Instant;

use crate::object::NullObjectTable;
use crate::scheduler::timer::Timer;
use crate::sync::event::EventSignaler;
use crate::sync::spinlock::SpinLockGuard;
use crate::{Arch, Kernel};

mod algorithm;
mod locks;
mod priority;
pub mod priority_bitmask;
pub mod thread;
pub mod timer;

use algorithm::{RescheduleReason, SchedulerAlgorithm};
pub use locks::{SchedLockGuard, WaitQueueLock};
pub use priority::Priority;
use thread::*;

const LOG_SCHEDULER_EVENTS: bool = false;
const WAIT_QUEUE_DEBUG: bool = false;
macro_rules! wait_queue_debug {
  ($($args:expr),*) => {{
    log_if::debug_if!(WAIT_QUEUE_DEBUG, $($args),*)
  }}
}

// generic on `arch` instead of `kernel` as it appears in the `arch` interface.
pub struct ThreadLocalState<A: Arch> {
    pub(crate) preempt_disable_count: A::AtomicUsize,
    pub(crate) needs_reschedule: A::AtomicBool,
}

impl<A: Arch> ThreadLocalState<A> {
    #[must_use]
    pub const fn new() -> Self {
        Self {
            preempt_disable_count: A::AtomicUsize::ZERO,
            needs_reschedule: A::AtomicBool::FALSE,
        }
    }
}

pub fn start_thread<K: Kernel>(kernel: K, mut thread: ForeignBox<Thread<K>>) -> ThreadRef<K> {
    info!(
        "Starting thread '{}' ({:#010x})",
        thread.name as &str,
        thread.id() as usize
    );

    pw_assert::assert!(thread.state == State::Initial);

    thread.state = State::Ready;
    let thread_ref = thread.get_ref(kernel);

    let mut sched_state = kernel.get_scheduler().lock(kernel);

    // If there is a current thread, insert it into the scheduler.
    let id = if let Some(mut current_thread) = sched_state.current_thread.take() {
        let id = current_thread.id();
        current_thread.state = State::Ready;
        sched_state
            .algorithm
            .schedule_thread(current_thread, RescheduleReason::Preempted);
        id
    } else {
        Thread::<K>::null_id()
    };

    sched_state
        .algorithm
        .schedule_thread(thread, RescheduleReason::Started);

    // Now that we've added the new thread, trigger a context switch.
    let (_sched_state, _switched) = context_switch(kernel, sched_state, id);

    thread_ref
}

pub fn initialize<K: Kernel>(kernel: K) {
    let mut sched_state = kernel.get_scheduler().lock(kernel);

    // The kernel process needs be to initialized before any kernel threads so
    // that they can properly be parented underneath it.
    unsafe {
        let kernel_process = sched_state.kernel_process.get();
        sched_state.add_process_to_list(NonNull::new_unchecked(kernel_process));
    }
}

pub fn bootstrap_scheduler<K: Kernel>(
    kernel: K,
    preempt_guard: PreemptDisableGuard<K>,
    mut thread: ForeignBox<Thread<K>>,
) -> ! {
    let mut sched_state = kernel.get_scheduler().lock(kernel);

    // TODO: assert that this is called exactly once at bootup to switch
    // to this particular thread.
    pw_assert::assert!(thread.state == State::Initial);
    thread.state = State::Ready;

    sched_state
        .algorithm
        .schedule_thread(thread, RescheduleReason::Started);

    info!("Context switching to first thread");

    // Special case where we're switching from a non-thread to something real
    let mut temp_arch_thread_state = K::ThreadState::NEW;
    sched_state.current_arch_thread_state = &raw mut temp_arch_thread_state;

    drop(preempt_guard);

    let _ = block(kernel, sched_state, Thread::<K>::null_id());
    pw_assert::panic!("Bootstrap scheduler returned unexpectedly");
}

pub struct PreemptDisableGuard<K: Kernel>(K);

impl<K: Kernel> PreemptDisableGuard<K> {
    pub fn new(kernel: K) -> Self {
        let prev_count = kernel
            .thread_local_state()
            .preempt_disable_count
            .fetch_add(1, core::sync::atomic::Ordering::SeqCst);

        // atomics have wrapping semantics so overflow is explicitly checked.
        if prev_count == usize::MAX {
            pw_assert::debug_panic!("PreemptDisableGuard: preempt_disable_count overflow")
        }

        Self(kernel)
    }
}

impl<K: Kernel> Drop for PreemptDisableGuard<K> {
    fn drop(&mut self) {
        let prev_count = self
            .0
            .thread_local_state()
            .preempt_disable_count
            .fetch_sub(1, core::sync::atomic::Ordering::SeqCst);

        if prev_count == 0 {
            pw_assert::debug_panic!("PreemptDisableGuard: preempt_disable_count underflow")
        }
    }
}

// Global scheduler state (single processor for now)
#[allow(dead_code)]
pub struct SchedulerState<K: Kernel> {
    // The scheduler owns the kernel process from which all kernel threads
    // are parented.
    kernel_process: UnsafeCell<Process<K>>,

    current_thread: Option<ForeignBox<Thread<K>>>,
    current_arch_thread_state: *mut K::ThreadState,
    process_list: UnsafeList<Process<K>, ProcessListAdapter<K>>,

    /// The algorithm used for choosing the next thread to run.
    algorithm: SchedulerAlgorithm<K>,

    termination_queue: ForeignList<Thread<K>, ThreadListAdapter<K>>,
}

unsafe impl<K: Kernel> Sync for SchedulerState<K> {}
unsafe impl<K: Kernel> Send for SchedulerState<K> {}

impl<K: Kernel> SchedulerState<K> {
    #[allow(dead_code)]
    #[allow(clippy::new_without_default)]
    pub const fn new() -> Self {
        static KERNEL_OBJECT_TABLE: NullObjectTable = NullObjectTable::new();
        Self {
            kernel_process: UnsafeCell::new(Process::new(
                "kernel",
                <K::ThreadState as ThreadState>::MemoryConfig::KERNEL_THREAD_MEMORY_CONFIG,
                // SAFETY: In the event of multiple scheduler objects, they will
                // refer to the same, immutable, instance of a zero sized type.
                unsafe { ForeignBox::new(NonNull::from_ref(&KERNEL_OBJECT_TABLE)) },
            )),
            current_thread: None,
            current_arch_thread_state: core::ptr::null_mut(),
            process_list: UnsafeList::new(),
            algorithm: SchedulerAlgorithm::new(),
            termination_queue: ForeignList::new(),
        }
    }

    /// Returns a pointer to the current threads architecture thread state
    /// struct.
    ///
    /// Only meant to be called from within an architecture implementation.
    ///
    /// # Safety
    ///
    /// Must be called with the scheduler lock held.  Pointer is only valid
    /// while the current threads remains the current thread.
    #[allow(dead_code)]
    #[doc(hidden)]
    pub unsafe fn get_current_arch_thread_state(&mut self) -> *mut K::ThreadState {
        self.current_arch_thread_state
    }

    fn reschedule_current_thread(&mut self, reason: RescheduleReason) -> usize {
        let mut current_thread = self.take_current_thread();
        let current_thread_id = current_thread.id();
        current_thread.state = State::Ready;
        self.algorithm.schedule_thread(current_thread, reason);
        current_thread_id
    }

    fn set_current_thread(&mut self, thread: ForeignBox<Thread<K>>) {
        self.current_arch_thread_state = thread.arch_thread_state.get();
        self.current_thread = Some(thread);
    }

    pub fn current_thread_id(&self) -> usize {
        match &self.current_thread {
            Some(thread) => thread.id(),
            None => Thread::<K>::null_id(),
        }
    }

    #[allow(dead_code)]
    pub fn current_thread_name(&self) -> &'static str {
        match &self.current_thread {
            Some(thread) => thread.name,
            None => "none",
        }
    }

    pub fn take_current_thread(&mut self) -> ForeignBox<Thread<K>> {
        let Some(thread) = self.current_thread.take() else {
            pw_assert::panic!("No current thread");
        };
        thread
    }

    #[allow(dead_code)]
    pub fn current_thread(&self) -> &Thread<K> {
        let Some(thread) = &self.current_thread else {
            pw_assert::panic!("No current thread");
        };
        thread
    }

    #[allow(dead_code)]
    pub fn current_thread_mut(&mut self) -> &mut Thread<K> {
        let Some(thread) = &mut self.current_thread else {
            pw_assert::panic!("No current thread");
        };
        thread
    }

    /// # Safety
    ///
    /// This method has the same safety preconditions as
    /// [`UnsafeList::push_front_unchecked`].
    #[allow(dead_code)]
    #[inline(never)]
    pub unsafe fn add_process_to_list(&mut self, process: NonNull<Process<K>>) {
        unsafe { self.process_list.push_front_unchecked(process) };
    }

    #[allow(dead_code)]
    pub fn dump_all_threads(&self) {
        info!("List of all threads:");
        unsafe {
            let _ = self
                .process_list
                .for_each(|process| -> core::result::Result<(), ()> {
                    process.dump();
                    Ok(())
                });
        }
    }
}

pub enum JoinResult<K: Kernel> {
    Joined(ForeignBox<Thread<K>>),
    Err { error: Error, thread: ThreadRef<K> },
}

enum TryJoinResult<K: Kernel> {
    Wait(ThreadRef<K>),
    Joined(ForeignBox<Thread<K>>),
    Err { error: Error, thread: ThreadRef<K> },
}

// All thread lifecycle management is encapsulated in this `impl` block which
// ensures the scheduler lock is held while the state is manipulated.
impl<K: Kernel> SpinLockGuard<'_, K, SchedulerState<K>> {
    /// Reschedule if preemption is enabled
    #[must_use]
    fn try_reschedule(mut self, kernel: K, reason: RescheduleReason) -> Self {
        if kernel
            .thread_local_state()
            .preempt_disable_count
            .load(Ordering::SeqCst)
            == 1
        {
            let current_thread_id = self.reschedule_current_thread(reason);
            let (guard, _switched) = context_switch(kernel, self, current_thread_id);
            guard
        } else {
            kernel
                .thread_local_state()
                .needs_reschedule
                .store(true, Ordering::SeqCst);
            self
        }
    }

    fn thread_initialize(
        &self,
        _kernel: K,
        thread: &mut Thread<K>,
        process: *mut Process<K>,
        kernel_stack: Stack,
    ) {
        thread.stack = kernel_stack;
        thread.process = process;

        thread.state = State::Initial;

        // SAFETY: *process is only accessed with the scheduler lock held.
        unsafe {
            // Assert that the parent process is added to the scheduler.
            pw_assert::assert!(
                (*process).link.is_linked(),
                "Tried to add a Thread to an unregistered Process"
            );
            // Add thread to processes thread list.
            (*process).add_to_thread_list(thread);
        }
    }

    pub fn thread_initialize_kernel<A: ThreadArg>(
        &mut self,
        kernel: K,
        thread: &mut Thread<K>,
        kernel_stack: Stack,
        entry_point: fn(K, A),
        arg: A,
    ) {
        pw_assert::assert!(thread.state == State::New);
        let process = self.kernel_process.get();
        let args = (entry_point as usize, kernel.into_usize(), arg.into_usize());

        kernel_stack.initialize();

        // SAFETY: The scheduler guarantees that a process' `memory_config`
        // remains valid while it has any child threads ensuring that the
        // `memory_config` is valid for the lifetime of the thread.
        unsafe {
            (*thread.arch_thread_state.get()).initialize_kernel_frame(
                kernel_stack,
                &raw const (*process).memory_config,
                Thread::<K>::trampoline::<A>,
                args,
            );
        }

        self.thread_initialize(kernel, thread, process, kernel_stack)
    }

    pub fn thread_reinitialize_kernel<A: ThreadArg>(
        &mut self,
        kernel: K,
        thread: &mut Thread<K>,
        entry_point: fn(K, A),
        arg: A,
    ) {
        pw_assert::assert!(thread.state == State::Joined);
        let process = self.kernel_process.get();
        let args = (entry_point as usize, kernel.into_usize(), arg.into_usize());

        thread.stack.initialize();

        // SAFETY: The scheduler guarantees that a process' `memory_config`
        // remains valid while it has any child threads ensuring that the
        // `memory_config` is valid for the lifetime of the thread.
        unsafe {
            (*thread.arch_thread_state.get()).initialize_kernel_frame(
                thread.stack,
                &raw const (*process).memory_config,
                Thread::<K>::trampoline::<A>,
                args,
            );
        }

        self.thread_initialize(kernel, thread, process, thread.stack)
    }

    #[cfg(feature = "user_space")]
    /// # Safety
    /// It is up to the caller to ensure that *process is valid.
    /// Initialize the mutable parts of the non privileged thread, must be
    /// called once per thread prior to starting it
    #[allow(clippy::too_many_arguments)]
    pub unsafe fn thread_initialize_non_priv(
        &mut self,
        kernel: K,
        thread: &mut Thread<K>,
        kernel_stack: Stack,
        initial_sp: usize,
        process: *mut Process<K>,
        initial_pc: usize,
        args: (usize, usize, usize),
    ) -> Result<()> {
        pw_assert::assert!(thread.state == State::New);

        kernel_stack.initialize();

        // SAFETY: The scheduler guarantees that a process' `memory_config`
        // remains valid while it has any child threads ensuring that the
        // `memory_config` is valid for the lifetime of the thread.
        unsafe {
            (*thread.arch_thread_state.get()).initialize_user_frame(
                kernel_stack,
                &raw const (*process).memory_config,
                // be passed in from user space.
                initial_sp,
                initial_pc,
                args,
            )?;
        }
        self.thread_initialize(kernel, thread, process, kernel_stack);
        Ok(())
    }

    fn thread_get_state(&self, thread: &ThreadRef<K>) -> thread::State {
        // SAFETY: we have exclusive access to thread by virtue of the scheduler lock being held.
        unsafe { thread.thread.as_ref().state }
    }

    fn thread_terminate(&mut self, thread_ref: &mut ThreadRef<K>) -> Result<()> {
        // SAFETY: we have exclusive access to thread by virtue of the scheduler lock being held.
        let thread = unsafe { thread_ref.thread.as_mut() };
        match &mut thread.owner {
            ThreadOwner::None => Err(Error::InvalidArgument),
            ThreadOwner::Scheduler => {
                if thread.state == thread::State::Running {
                    // thread is currently running.  This thread?  For now return an error
                    return Err(Error::InvalidArgument);
                }

                // Mark thread as terminating so that it can clean itself up.
                thread.terminating = true;

                // Should we signal the scheduling algorithm to give it the
                // chance to move the thread to the front of the queue?

                Ok(())
            }
            ThreadOwner::WaitQueue { queue, wait_type } => {
                // First, mark the thread as being in the terminated state.
                // SAFETY: Threads access is guarded by the scheduler lock.
                unsafe { (*thread_ref.thread.as_ptr()).terminating = true }

                // Second, wake the thread if it is in an interruptible wait.
                if *wait_type == WaitType::Interruptible {
                    // SAFETY: All thread and wait queue accesses are guarded by
                    // the scheduler lock.
                    let ret = unsafe { queue.as_mut().queue.remove_element(thread_ref.thread) };
                    let Some(thread_box) = ret else {
                        // The a thread's owner is `ThreadOwner::WaitQueue` it will always
                        // be in the `WaitQueue`'s queue.  A failure to remove here would
                        // be the result of a bug or corruption.
                        pw_assert::panic!("Could not remove thread from its owning WaitQueue");
                    };
                    thread.state = State::Ready;
                    self.algorithm
                        .schedule_thread(thread_box, RescheduleReason::Woken);
                }
                Ok(())
            }
        }
    }

    fn thread_signal_join(&self, thread: &mut ThreadRef<K>) {
        if let Some(signaler) = unsafe { thread.thread.as_mut() }.join_event.take() {
            signaler.signal();
        }
    }

    fn thread_try_join(
        &mut self,
        mut thread_ref: ThreadRef<K>,
        signaler: EventSignaler<K>,
    ) -> TryJoinResult<K> {
        // SAFETY: Exclusive access to thread is guaranteed by scheduler lock
        let thread = unsafe { thread_ref.thread.as_mut() };

        // If the thread is terminated and `thread_ref` is the singular reference to it,
        // the thread is terminated
        if thread.state == thread::State::Terminated && thread.ref_count.load(Ordering::SeqCst) == 1
        {
            // SAFETY: Threads only enter the Terminated state through `exit_thread` which adds them
            // to the termination_queue.  Join is the only method by which they are removed from the
            // queue and the state is set to the Joined state when that happens.  Therefore, if the
            // thread is in the Terminated state, it *must* be in the termination_queue.
            thread.state = thread::State::Joined;

            unsafe { thread.remove_from_parent_process() };
            // Reset thread state.
            thread.terminating = false;
            *thread.arch_thread_state.get_mut() = K::ThreadState::NEW;

            return TryJoinResult::Joined(unsafe {
                self.termination_queue
                    .remove_element(thread_ref.thread)
                    .unwrap_unchecked()
            });
        }

        // Only one call to join is allowed to wait thread termination
        if thread.join_event.is_some() {
            return TryJoinResult::Err {
                error: Error::AlreadyExists,
                thread: thread_ref,
            };
        }

        // Register the signaler and return None indicating the thread is not yet
        // joinable.
        thread.join_event = Some(signaler);
        TryJoinResult::Wait(thread_ref)
    }

    fn thread_cancel_try_join(&mut self, thread_ref: &mut ThreadRef<K>) {
        // SAFETY: Exclusive access to thread is guaranteed by scheduler lock
        let thread = unsafe { thread_ref.thread.as_mut() };
        pw_assert::assert!(thread.join_event.is_some());
        thread.join_event = None;
    }

    fn thread_exit(mut self, kernel: K) -> ! {
        let mut current_thread = self.take_current_thread();
        let current_thread_id = current_thread.id();

        pw_assert::assert!(matches!(current_thread.owner, ThreadOwner::Scheduler));

        info!(
            "Exiting thread '{}' ({:#010x})",
            current_thread.name as &str,
            current_thread.id() as usize
        );

        // Since the current thread has already been removed from the scheduler,
        // a PreemptDisableGuard is taken to prevent the event wait from doing a
        // reschedule.
        let guard = PreemptDisableGuard::new(kernel);
        if let Some(signaler) = current_thread.as_mut().join_event.take() {
            signaler.signal();
        }
        drop(guard);

        current_thread.state = State::Terminated;
        current_thread.terminating = true;

        self.termination_queue.push_back(current_thread);

        let _ = block(kernel, self, current_thread_id);

        pw_assert::panic!("thread_exit returned unexpectedly");
    }

    fn thread_is_terminating(&self, thread: &ThreadRef<K>) -> bool {
        // SAFETY: Exclusive access to thread is guaranteed by scheduler lock
        unsafe { thread.thread.as_ref().terminating }
    }
}

#[must_use]
fn block<K: Kernel>(
    kernel: K,
    sched_state: SpinLockGuard<K, SchedulerState<K>>,
    current_thread_id: usize,
) -> SpinLockGuard<K, SchedulerState<K>> {
    let (sched_state, switched) = context_switch(kernel, sched_state, current_thread_id);
    pw_assert::assert!(switched, "Blocking requires a context switch");
    sched_state
}

#[must_use]
fn context_switch<K: Kernel>(
    kernel: K,
    mut sched_state: SpinLockGuard<K, SchedulerState<K>>,
    current_thread_id: usize,
) -> (SpinLockGuard<K, SchedulerState<K>>, bool) {
    // Caller to reschedule is responsible for removing current thread and
    // put it in the correct run/wait queue.
    pw_assert::assert!(sched_state.current_thread.is_none());

    // Validate that the only mechanism disabling preemption is the scheduler
    // lock which is passed in to this function.
    pw_assert::assert!(
        kernel
            .thread_local_state()
            .preempt_disable_count
            .load(Ordering::SeqCst)
            <= 1,
        "Preemption count greater than 1"
    );

    // Pop a new thread off the head of the run queue.
    // At the moment cannot handle an empty queue, so will panic in that case.
    // TODO: Implement either an idle thread or a special idle routine for that case.
    let Some(mut new_thread) = sched_state.algorithm.get_next_thread() else {
        pw_assert::panic!("Run queue empty: no runnable threads (idle thread missing or blocked?)");
    };

    pw_assert::assert!(
        new_thread.state == State::Ready,
        "<{}>({:#010x}) not ready",
        new_thread.name as &str,
        new_thread.id() as usize,
    );
    new_thread.state = State::Running;

    if current_thread_id == new_thread.id() {
        sched_state.current_thread = Some(new_thread);
        return (sched_state, false);
    }

    let old_thread_state = sched_state.current_arch_thread_state;
    let new_thread_state = new_thread.arch_thread_state.get();
    sched_state.set_current_thread(new_thread);
    unsafe { kernel.context_switch(sched_state, old_thread_state, new_thread_state) }
}

/// If try_reschedule() was called while preemption was disabled, try to
/// reschedule again after the preempt guard is dropped.
pub fn try_deferred_reschedule<K: Kernel>(kernel: K) {
    if Ok(true)
        == kernel
            .thread_local_state()
            .needs_reschedule
            .compare_exchange(true, false, Ordering::SeqCst, Ordering::SeqCst)
    {
        let _ = kernel
            .get_scheduler()
            .lock(kernel)
            .try_reschedule(kernel, RescheduleReason::Preempted);
    }
}

#[allow(dead_code)]
pub fn yield_timeslice<K: Kernel>(kernel: K) {
    let sched_state = kernel.get_scheduler().lock(kernel);
    log_if::info_if!(
        LOG_SCHEDULER_EVENTS,
        "Yielding thread '{}' ({:#010x})",
        sched_state.current_thread_name() as &str,
        sched_state.current_thread_id() as usize
    );
    let _ = sched_state.try_reschedule(kernel, RescheduleReason::Ticked);
}

// Tick that is called from a timer handler. The scheduler will evaluate if the current thread
// should be preempted or not
#[allow(dead_code)]
pub fn tick<K: Kernel>(kernel: K, now: Instant<K::Clock>) {
    log_if::info_if!(
        LOG_SCHEDULER_EVENTS,
        "Scheduler tick at {}",
        now.ticks() as u64
    );
    pw_assert::assert!(
        kernel
            .thread_local_state()
            .preempt_disable_count
            .load(Ordering::SeqCst)
            == 0,
        "scheduler::tick() called with preemption disabled"
    );

    let guard = PreemptDisableGuard::new(kernel);

    // In lieu of a proper timer interface, the scheduler needs to be robust
    // to timer ticks arriving before it is initialized.
    if kernel.get_scheduler().lock(kernel).current_thread.is_none() {
        return;
    }

    timer::process_queue(kernel, now);
    drop(guard);

    let _ = kernel
        .get_scheduler()
        .lock(kernel)
        .try_reschedule(kernel, RescheduleReason::Ticked);
}

/// Exit the currently running thread.
///
/// The thread will enter `State::Terminated`, wait for all outstanding
/// references to be dropped, then wait to be joined.
#[allow(dead_code)]
pub fn exit_thread<K: Kernel>(kernel: K) -> ! {
    kernel.get_scheduler().lock(kernel).thread_exit(kernel)
}

pub fn sleep_until<K: Kernel>(kernel: K, deadline: Instant<K::Clock>) -> Result<()> {
    let wait_queue = WaitQueueLock::new(kernel, ());

    let (_, ret) = wait_queue
        .lock()
        .wait_until(WaitType::Interruptible, deadline);
    match ret {
        // DeadlineExceeded is the expected result so return OK in that case.
        Err(Error::DeadlineExceeded) => Ok(()),

        // In all other cases, pass on the return value.
        _ => ret,
    }
}

pub struct WaitQueue<K: Kernel> {
    queue: ForeignList<Thread<K>, ThreadListAdapter<K>>,
}

unsafe impl<K: Kernel> Sync for WaitQueue<K> {}
unsafe impl<K: Kernel> Send for WaitQueue<K> {}

impl<K: Kernel> WaitQueue<K> {
    #[allow(dead_code, clippy::new_without_default)]
    #[must_use]
    pub const fn new() -> Self {
        Self {
            queue: ForeignList::new(),
        }
    }
}

#[derive(Clone, Copy, Eq, PartialEq)]
pub enum WaitType {
    Interruptible,
    NonInterruptible,
}

#[derive(Clone, Copy, Eq, PartialEq)]
pub enum WakeResult {
    Woken,
    QueueEmpty,
}

impl<K: Kernel> SchedLockGuard<'_, K, WaitQueue<K>> {
    fn add_to_queue_and_reschedule(
        mut self,
        mut thread: ForeignBox<Thread<K>>,
        wait_type: WaitType,
    ) -> Self {
        let current_thread_id = thread.id();
        let current_thread_name = thread.name;
        thread.state = State::Waiting;
        thread.owner = ThreadOwner::WaitQueue {
            queue: NonNull::from_ref(&self),
            wait_type,
        };
        self.queue.push_back(thread);
        wait_queue_debug!(
            "WaitQueue: thread '{}' ({:#010x}) rescheduling",
            current_thread_name as &str,
            current_thread_id as usize
        );
        self.block(current_thread_id)
    }

    // Safety:
    // Caller guarantees that thread is non-null, valid, and process_timeout
    // has exclusive access to `waiting_thread`.
    unsafe fn process_timeout(&mut self, waiting_thread: *mut Thread<K>) -> Option<Error> {
        if unsafe { (*waiting_thread).state } != State::Waiting {
            // Thread has already been woken.
            return None;
        }

        let Some(mut thread) = (unsafe {
            self.queue
                .remove_element(NonNull::new_unchecked(waiting_thread))
        }) else {
            pw_assert::panic!("Thread no longer in wait queue");
        };

        wait_queue_debug!(
            "WaitQueue: timeout for thread '{}' ({:#010x})",
            thread.name as &str,
            thread.id() as usize
        );
        thread.state = State::Ready;
        self.sched_mut()
            .algorithm
            .schedule_thread(thread, RescheduleReason::Woken);
        Some(Error::DeadlineExceeded)
    }

    #[allow(clippy::must_use_candidate)]
    pub fn wake_one(mut self) -> (Self, WakeResult) {
        let Some(mut thread) = self.queue.pop_head() else {
            return (self, WakeResult::QueueEmpty);
        };
        wait_queue_debug!(
            "WaitQueue: waking thread '{}' ({:#010x})",
            thread.name as &str,
            thread.id() as usize
        );
        thread.state = State::Ready;
        self.sched_mut()
            .algorithm
            .schedule_thread(thread, RescheduleReason::Woken);

        (
            self.try_reschedule(RescheduleReason::Preempted),
            WakeResult::Woken,
        )
    }

    #[allow(clippy::return_self_not_must_use, clippy::must_use_candidate)]
    pub fn wake_all(mut self) -> Self {
        loop {
            let result;
            (self, result) = self.wake_one();
            if result == WakeResult::QueueEmpty {
                return self;
            }
        }
    }

    #[allow(clippy::return_self_not_must_use, clippy::must_use_candidate)]
    pub fn wait(mut self, wait_type: WaitType) -> (Self, Result<()>) {
        // If the current thread is terminating and this is an interruptible wait,
        // return early with an error.
        if self.sched().current_thread().terminating && wait_type == WaitType::Interruptible {
            return (self, Err(Error::Cancelled));
        }

        let thread = self.sched_mut().take_current_thread();
        wait_queue_debug!(
            "WaitQueue: thread '{}' ({:#010x}) waiting",
            thread.name as &str,
            thread.id() as usize
        );
        self = self.add_to_queue_and_reschedule(thread, wait_type);
        wait_queue_debug!(
            "WaitQueue: thread '{}' ({:#010x}) resumed",
            self.sched().current_thread_name() as &str,
            self.sched().current_thread_id() as usize
        );

        // Return `Error::Cancelled` if the wait was interrupted because this
        // thread was terminated while it was waiting.
        if self.sched().current_thread().terminating && wait_type == WaitType::Interruptible {
            (self, Err(Error::Cancelled))
        } else {
            (self, Ok(()))
        }
    }

    pub fn wait_until(
        mut self,
        wait_type: WaitType,
        deadline: Instant<K::Clock>,
    ) -> (Self, Result<()>) {
        // If the current thread is terminating and this is an interruptible wait,
        // return early with an error.
        if self.sched().current_thread().terminating && wait_type == WaitType::Interruptible {
            return (self, Err(Error::Cancelled));
        }

        let mut thread = self.sched_mut().take_current_thread();
        wait_queue_debug!(
            "WaitQueue: thread '{}' ({:#010x}) wait_until",
            thread.name as &str,
            thread.id() as usize
        );

        // Smuggle references to the thread and wait queue into the callback.
        // Safety:
        // * The thread will always be active for the lifetime of the wait as
        //   it can not be joined while in a WaitQueue.
        // * The wait queue will outlive the callback because it will either
        //   fire while the thread is in the wait queue or will be the timer
        //   will be canceled before this function returns.
        // * All access to thread_ptr and wait_queue_ptr in the callback are
        //   done while the wait queue lock is held.
        let thread_ptr = unsafe { thread.as_mut_ptr() };
        let smuggled_wait_queue = unsafe { self.smuggle() };

        // Safety:
        // * Only accessed while the wait_queue_lock is held;
        let result: UnsafeCell<Result<()>> = UnsafeCell::new(Ok(()));
        let result_ptr = result.get();

        // Timeout callback will remove the thread from the wait queue and put
        // it back on the run queue.
        let mut callback_closure = move |_kernel, callback: ForeignBox<Timer<K>>, _now| {
            // Safety: wait queue lock is valid for the lifetime of the callback.
            let mut wait_queue = unsafe { smuggled_wait_queue.lock() };

            // Safety: the wait queue lock protects access to the thread.
            wait_queue_debug!(
                "WaitQueue: timeout callback for thread '{}' ({:#010x}) (state: {})",
                unsafe { (*thread_ptr).name } as &str,
                (unsafe { (*thread_ptr).id() }) as usize,
                unsafe { thread::to_string((*thread_ptr).state) } as &str
            );

            // Safety: We know that thread_ptr is valid for the life of `wait_until`
            // and this callback will either be called or canceled before `wait_until`
            // returns.
            if let Some(error) = unsafe { wait_queue.process_timeout(thread_ptr) } {
                // Safety: Acquisition of the wait queue lock at the beginning of
                // the callback ensures mutual exclusion with accesses from the
                // body of `wait_until`.
                unsafe { result_ptr.write_volatile(Err(error)) };
            }

            let _ = callback.consume();
            None // Don't re-arm
        };

        let mut callback = Timer::new(deadline, unsafe {
            ForeignBox::new_from_ptr(&raw mut callback_closure)
        });
        let callback_ptr = &raw mut callback;
        timer::schedule_timer(self.kernel, unsafe {
            ForeignBox::new_from_ptr(callback_ptr)
        });

        // Safety: It is important hold on to the WaitQueue lock that is returned
        // from reschedule as the pointers needed by the timer canceling code
        // below rely on it for correctness.
        self = self.add_to_queue_and_reschedule(thread, wait_type);

        wait_queue_debug!(
            "WaitQueue: thread '{}' ({:#010x}) resumed",
            self.sched().current_thread_name() as &str,
            self.sched().current_thread_id() as usize
        );

        // Cancel timeout callback if has not already fired.
        //
        // Safety: callback_ptr is valid until callback goes out of scope.
        unsafe {
            timer::cancel_and_consume_timer(self.kernel, NonNull::new_unchecked(callback_ptr))
        };

        wait_queue_debug!(
            "WaitQueue: thread '{}' ({:#010x}) exiting wait_until",
            self.sched().current_thread_name() as &str,
            self.sched().current_thread_id() as usize
        );

        // Safety:
        //
        // At this point the thread will be in the run queue by virtue of
        // `reschedule()` return and the timer callback will have fired or be
        // canceled.  This leaves no dangling references to our "smuggled"
        // pointers.
        //
        // It is also now safe to read the result UnsafeCell

        // Return `Error::Cancelled` if the wait was interrupted because this
        // thread was terminated while it was waiting.
        if self.sched().current_thread().terminating && wait_type == WaitType::Interruptible {
            (self, Err(Error::Cancelled))
        } else {
            (self, unsafe { result.get().read_volatile() })
        }
    }
}
