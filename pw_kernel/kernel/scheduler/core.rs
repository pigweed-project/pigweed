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

use core::cell::UnsafeCell;
use core::ptr::NonNull;
use core::sync::atomic::Ordering;

use exit_status::ExitStatus;
use foreign_box::ForeignBox;
use list::*;
use memory_config::MemoryConfig as _;
use pw_atomic::{
    AtomicAdd, AtomicCompareExchange, AtomicFalse, AtomicLoad, AtomicStore, AtomicSub, AtomicZero,
};
use pw_log::info;
use pw_status::{Error, Result};
use pw_time_core::Instant;

#[cfg(feature = "user_space")]
use crate::object::NullObjectTable;
use crate::scheduler::algorithm::{RescheduleReason, SchedulerAlgorithm};
use crate::scheduler::locks::WaitQueueLock;
use crate::scheduler::process::{Process, ProcessHandle, ProcessListAdapter, ProcessState};
use crate::scheduler::thread::{
    State, Thread, ThreadArg, ThreadHandle, ThreadListAdapter, ThreadOwner, ThreadState,
};
use crate::scheduler::wait_queue::WaitType;
use crate::sync::event::EventSignaler;
use crate::sync::spinlock::SpinLockGuard;
use crate::trace::trace_context_switch;
use crate::{Arch, Kernel};

const LOG_SCHEDULER_EVENTS: bool = false;

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

pub fn start_thread<K: Kernel>(kernel: K, mut thread: ForeignBox<Thread<K>>) -> ThreadHandle<K> {
    log_if::info_if!(
        LOG_SCHEDULER_EVENTS,
        "Starting thread '{}' ({:#010x})",
        thread.name as &str,
        thread.id() as usize
    );

    pw_assert::assert!(thread.state == State::Initial);

    thread.state = State::Ready;
    let thread_handle = thread.get_ref(kernel);

    let mut sched_state = kernel.get_scheduler().lock(kernel);

    sched_state
        .algorithm
        .schedule_thread(thread, RescheduleReason::Started);

    let _sched_state = sched_state.try_reschedule(kernel, RescheduleReason::Preempted);

    thread_handle
}

pub fn initialize<K: Kernel>(kernel: K) {
    let mut sched_state = kernel.get_scheduler().lock(kernel);

    // The kernel process needs to be initialized before any kernel threads so
    // that they can properly be parented underneath it.

    // SAFETY: Per the contract in `SchedulerState` this is the one place that
    // `kernel_process` is converted into a foreign box and its refcount is
    // incremented.
    unsafe {
        let kernel_process = sched_state.kernel_process.get();
        (*kernel_process).state = ProcessState::Active;
        (*kernel_process).manually_increment_ref_count();
        let kernel_process = ForeignBox::new(NonNull::new_unchecked(kernel_process));

        sched_state.add_process_to_list(kernel_process);
    }
}

pub fn add_process<K: Kernel>(kernel: K, mut process: ForeignBox<Process<K>>) -> ProcessHandle<K> {
    let mut sched_state = kernel.get_scheduler().lock(kernel);
    let process_handle = ProcessHandle::new(NonNull::from(&mut *process), kernel);

    // The only way for a `ForeignBox<Process<K>>` to exist outside the scheduler,
    // is for it to be newly created or to have been returned by the scheduler from a join.
    // In both cases, the process is in the `Inactive` state.
    pw_assert::assert!(process.state == ProcessState::Inactive);
    process.state = ProcessState::Active;

    // SAFETY: We are taking ownership of the ForeignBox and interacting with the
    // scheduler state which we have locked.
    sched_state.add_process_to_list(process);
    process_handle
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

    let _ = block(
        kernel,
        sched_state,
        Thread::<K>::null_id(),
        State::Terminated,
    );
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
    //
    // SAFETY: `kernel_process` must be converted to a `ForeignBox` exactly
    // once while the scheduler is initialized.  This initialization must also
    // increment the processes refcount.
    pub(crate) kernel_process: UnsafeCell<Process<K>>,

    pub(super) current_thread: Option<ForeignBox<Thread<K>>>,
    pub(super) current_arch_thread_state: *mut K::ThreadState,
    pub(super) process_list: ForeignList<Process<K>, ProcessListAdapter<K>>,

    /// The algorithm used for choosing the next thread to run.
    pub(super) algorithm: SchedulerAlgorithm<K>,

    pub(super) termination_queue: ForeignList<Thread<K>, ThreadListAdapter<K>>,
}

unsafe impl<K: Kernel> Sync for SchedulerState<K> {}
unsafe impl<K: Kernel> Send for SchedulerState<K> {}

impl<K: Kernel> SchedulerState<K> {
    #[allow(dead_code)]
    #[allow(clippy::new_without_default)]
    pub const fn new() -> Self {
        #[cfg(feature = "user_space")]
        static KERNEL_OBJECT_TABLE: NullObjectTable = NullObjectTable::new();
        Self {
            kernel_process: UnsafeCell::new(Process::new(
                "kernel",
                <K::ThreadState as ThreadState>::MemoryConfig::KERNEL_THREAD_MEMORY_CONFIG,
                // SAFETY: In the event of multiple scheduler objects, they will
                // refer to the same, immutable, instance of a zero sized type.
                #[cfg(feature = "user_space")]
                unsafe {
                    ForeignBox::new(NonNull::from_ref(&KERNEL_OBJECT_TABLE))
                },
            )),
            current_thread: None,
            current_arch_thread_state: core::ptr::null_mut(),
            process_list: ForeignList::new(),
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

    fn reschedule_current_thread(&mut self, reason: RescheduleReason) -> (usize, State) {
        let Some(mut current_thread) = self.current_thread.take() else {
            return (Thread::<K>::null_id(), State::New);
        };

        let current_thread_id = current_thread.id();
        current_thread.state = State::Ready;
        self.algorithm.schedule_thread(current_thread, reason);
        (current_thread_id, State::Ready)
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

    pub fn current_process_handle(&self) -> ProcessHandle<K> {
        self.current_thread().process()
    }

    /// # Safety
    ///
    /// This method has the same safety preconditions as
    /// [`UnsafeList::push_front_unchecked`].
    #[allow(dead_code)]
    #[inline(never)]
    pub fn add_process_to_list(&mut self, process: ForeignBox<Process<K>>) {
        self.process_list.push_front(process);
    }

    fn get_kernel_process_handle(&self, kernel: K) -> ProcessHandle<K> {
        let Some(process) = NonNull::new(self.kernel_process.get()) else {
            pw_assert::panic!("Kernel process is null");
        };
        ProcessHandle::new(process, kernel)
    }

    #[allow(dead_code)]
    pub fn dump(&self, kernel: K) {
        info!("List of all threads:");
        let _ = self
            .process_list
            .for_each(|process| -> core::result::Result<(), ()> {
                process.dump(kernel);
                Ok(())
            });
    }
}

pub enum JoinResult<K: Kernel> {
    Joined(ForeignBox<Thread<K>>, ExitStatus),
    Err {
        error: Error,
        thread: ThreadHandle<K>,
    },
}

pub enum TryJoinResult<K: Kernel> {
    Wait(ThreadHandle<K>),
    Joined(ForeignBox<Thread<K>>, ExitStatus),
    Err {
        error: Error,
        thread: ThreadHandle<K>,
    },
}

pub enum ProcessJoinResult<K: Kernel> {
    Joined(ForeignBox<Process<K>>, ExitStatus),
    Err {
        error: Error,
        process: ProcessHandle<K>,
    },
}

pub enum ProcessTryJoinResult<K: Kernel> {
    Wait(ProcessHandle<K>),
    Joined(ForeignBox<Process<K>>, ExitStatus),
    Err {
        error: Error,
        process: ProcessHandle<K>,
    },
}

// All thread lifecycle management is encapsulated in this `impl` block which
// ensures the scheduler lock is held while the state is manipulated.
impl<K: Kernel> SpinLockGuard<'_, K, SchedulerState<K>> {
    /// Reschedule if preemption is enabled
    #[must_use]
    pub(super) fn try_reschedule(mut self, kernel: K, reason: RescheduleReason) -> Self {
        if kernel
            .thread_local_state()
            .preempt_disable_count
            .load(Ordering::SeqCst)
            == 1
        {
            let (current_thread_id, current_thread_state) = self.reschedule_current_thread(reason);
            let (guard, _switched) =
                context_switch(kernel, self, current_thread_id, current_thread_state);
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
        mut process_handle: ProcessHandle<K>,
    ) -> Result<()> {
        thread.state = State::Initial;

        // SAFETY: *process is only accessed with the scheduler lock held.
        unsafe {
            let process = process_handle.process.as_mut();
            // Assert that the parent process is active.
            if process.state != ProcessState::Active {
                return Err(Error::FailedPrecondition);
            }
            // Add thread to processes thread list.
            process.add_to_thread_list(thread);
        }

        thread.set_process(process_handle);
        Ok(())
    }

    pub fn thread_initialize_kernel<A: ThreadArg>(
        &mut self,
        kernel: K,
        thread: &mut Thread<K>,
        entry_point: fn(K, A),
        arg: A,
    ) {
        pw_assert::assert!(thread.state == State::New);
        let process_handle = self.get_kernel_process_handle(kernel);
        let args = (entry_point as usize, kernel.into_usize(), arg.into_usize());

        thread.stack.initialize();

        unsafe {
            (*thread.arch_thread_state.get()).initialize_kernel_state(
                thread.stack,
                &raw const process_handle.process.as_ref().memory_config,
                Thread::<K>::trampoline::<A>,
                args,
            );
        }

        // The kernel process is bootstrapped as active, and any attempt to
        // terminate it will panic. Therefore, this initialization must always succeed.
        pw_assert::assert!(
            self.thread_initialize(kernel, thread, process_handle)
                .is_ok()
        );
    }

    pub fn thread_reinitialize_kernel<A: ThreadArg>(
        &mut self,
        kernel: K,
        thread: &mut Thread<K>,
        entry_point: fn(K, A),
        arg: A,
    ) {
        pw_assert::assert!(thread.state == State::Joined);
        let process_handle = self.get_kernel_process_handle(kernel);
        let args = (entry_point as usize, kernel.into_usize(), arg.into_usize());

        thread.stack.initialize();

        // SAFETY: The scheduler guarantees that a process' `memory_config`
        // remains valid while it has any child threads ensuring that the
        // `memory_config` is valid for the lifetime of the thread.
        unsafe {
            (*thread.arch_thread_state.get()).initialize_kernel_state(
                thread.stack,
                &raw const process_handle.process.as_ref().memory_config,
                Thread::<K>::trampoline::<A>,
                args,
            );
        }

        // The kernel process is bootstrapped as active, and any attempt to
        // terminate it will panic. Therefore, this initialization must always succeed.
        pw_assert::assert!(
            self.thread_initialize(kernel, thread, process_handle)
                .is_ok()
        );
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
        process_handle: ProcessHandle<K>,
        initial_pc: usize,
        initial_sp: usize,
        args: (usize, usize, usize),
    ) -> Result<()> {
        pw_assert::assert!(matches!(thread.state, State::New | State::Joined));

        thread.stack.initialize();

        // SAFETY: The scheduler guarantees that a process' `memory_config`
        // remains valid while it has any child threads.  This is because
        // each `Thread` holds a `ProcessHandle` to its parent process, ensuring
        // the `Process` and its `memory_config` are valid for the lifetime
        // of the thread.
        #[cfg(feature = "user_space")]
        unsafe {
            (*thread.arch_thread_state.get()).initialize_user_state(
                thread.stack,
                &raw const process_handle.process.as_ref().memory_config,
                initial_sp,
                initial_pc,
                args,
            )?;
        }

        self.thread_initialize(kernel, thread, process_handle)
    }

    pub(crate) fn thread_get_state(&self, thread: &ThreadHandle<K>) -> State {
        // SAFETY: we have exclusive access to thread by virtue of the scheduler lock being held.
        unsafe { thread.thread.as_ref().state }
    }

    /// Request termination of the referenced thread.
    ///
    /// This is an ASYNCHRONOUS operation. The thread is marked as terminating,
    /// but it may not exit immediately.
    pub fn thread_terminate(
        &mut self,
        thread_handle: &mut ThreadHandle<K>,
        status: ExitStatus,
    ) -> Result<()> {
        // SAFETY: we have exclusive access to thread by virtue of the scheduler lock being held.
        let thread = unsafe { thread_handle.thread.as_mut() };
        self.thread_terminate_internal(thread, status)
    }

    pub(crate) fn thread_terminate_internal(
        &mut self,
        thread: &mut Thread<K>,
        status: ExitStatus,
    ) -> Result<()> {
        thread.exit_status = Some(status);
        match &mut thread.owner {
            ThreadOwner::None => Err(Error::InvalidArgument),
            ThreadOwner::Scheduler => {
                // Mark thread as terminating so that it can clean itself up.
                thread.terminating = true;

                // Should we signal the scheduling algorithm to give it the
                // chance to move the thread to the front of the queue?

                Ok(())
            }
            ThreadOwner::WaitQueue { queue, wait_type } => {
                // First, mark the thread as being in the terminated state.
                // SAFETY: Threads access is guarded by the scheduler lock.
                thread.terminating = true;

                // Second, wake the thread if it is in an interruptible wait.
                if *wait_type == WaitType::Interruptible {
                    // SAFETY: All thread and wait queue accesses are guarded by
                    // the scheduler lock and the thread is guaranteed to be in
                    // the queue by the fact that the queue is the ThreadOwner.
                    let ret = unsafe {
                        queue
                            .as_mut()
                            .queue
                            .remove_element(NonNull::from_ref(thread))
                    };
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

    pub(crate) fn thread_signal_join(mut self, thread: &mut ThreadHandle<K>) -> Self {
        if let Some(signaler) = unsafe { thread.thread.as_mut() }.join_event.take() {
            self = signaler.signal_locked(self);
        }

        self
    }

    pub(crate) fn thread_try_join(
        mut self,
        kernel: K,
        mut thread_handle: ThreadHandle<K>,
        signaler: Option<EventSignaler<K>>,
    ) -> (Self, TryJoinResult<K>) {
        // SAFETY: Exclusive access to thread is guaranteed by scheduler lock
        let thread = unsafe { thread_handle.thread.as_mut() };

        // If the thread is terminated and `thread_handle` is the singular reference to it,
        // the thread is terminated
        if thread.state == State::Terminated && thread.ref_count.load(Ordering::SeqCst) == 1 {
            // SAFETY: Threads only enter the Terminated state through `exit_thread` which adds them
            // to the termination_queue.  Join is the only method by which they are removed from the
            // queue and the state is set to the Joined state when that happens.  Therefore, if the
            // thread is in the Terminated state, it *must* be in the termination_queue.
            thread.state = State::Joined;

            self = self.thread_remove_from_parent_process(kernel, thread);

            // Reset thread state.
            thread.terminating = false;

            // SAFETY: Thread is guaranteed to be in the termination queue by
            // the fact that it is in the terminated state.
            #[allow(unused_mut)]
            let mut thread = unsafe {
                self.termination_queue
                    .remove_element(thread_handle.thread)
                    .unwrap_unchecked()
            };

            // Clear JOINABLE signal.
            #[cfg(feature = "user_space")]
            if let Some(object) = thread.object.take() {
                self = object.signal_locked(kernel, self, |s| s - syscall_defs::Signals::JOINABLE);
            }

            let Some(status) = thread.exit_status.take() else {
                pw_assert::panic!("Thread joined with no exit status set");
            };
            return (self, TryJoinResult::Joined(thread, status));
        }

        // Only one call to join is allowed to wait thread termination
        if thread.join_event.is_some() {
            return (
                self,
                TryJoinResult::Err {
                    error: Error::AlreadyExists,
                    thread: thread_handle,
                },
            );
        }

        // Register the signaler and return None indicating the thread is not yet
        // joinable.
        if let Some(signaler) = signaler {
            thread.join_event = Some(signaler);
        }
        (self, TryJoinResult::Wait(thread_handle))
    }

    pub(crate) fn thread_cancel_try_join(&mut self, thread_handle: &mut ThreadHandle<K>) {
        // SAFETY: Exclusive access to thread is guaranteed by scheduler lock
        let thread = unsafe { thread_handle.thread.as_mut() };
        pw_assert::assert!(thread.join_event.is_some());
        thread.join_event = None;
    }

    /// Performs the low-level operations to exit the current thread.
    ///
    /// This method handles the actual state transitions and cleanup for thread exit.
    /// It expects the `exit_status` to have been set by the caller if necessary.
    ///
    /// # Differences from other exit/terminate functions:
    /// - **`exit_thread`**: A high-level wrapper that takes an explicit status,
    ///   sets it on the thread, and then calls this method. It does not return.
    /// - **`terminate`**: Asynchronous requests to terminate a thread/process.
    ///   They set the target's state to terminating but rely on `thread_exit`
    ///   being called later to perform the actual exit.
    pub(crate) fn thread_exit(mut self, kernel: K) {
        let mut current_thread = self.take_current_thread();
        let current_thread_id = current_thread.id();

        pw_assert::assert!(matches!(current_thread.owner, ThreadOwner::Scheduler));

        log_if::info_if!(
            LOG_SCHEDULER_EVENTS,
            "Exiting thread '{}' ({:#010x})",
            current_thread.name as &str,
            current_thread.id() as usize
        );

        // Since the current thread has already been removed from the scheduler,
        // a PreemptDisableGuard is taken to prevent the event wait from doing a
        // reschedule.
        let guard = PreemptDisableGuard::new(kernel);
        if let Some(signaler) = current_thread.as_mut().join_event.take() {
            self = signaler.signal_locked(self);
        }
        drop(guard);
        current_thread.state = State::Terminated;
        current_thread.terminating = true;

        #[cfg(feature = "user_space")]
        {
            let process_handle = current_thread.process();
            let thread_object = current_thread.object.as_ref().cloned();

            // It is important that we add the thread to the termination queue
            // before possibly auto-joining the thread as join expects the thread
            // to be in termination queue at that time.
            self.termination_queue.push_back(current_thread);

            if let Some(thread_object) = thread_object {
                // Ensure we set them as JOINABLE again if it transitions to Terminated
                self = thread_object
                    .signal_locked(kernel, self, |s| s | syscall_defs::Signals::JOINABLE);

                if process_handle.get_state_locked(&self) == ProcessState::Terminating {
                    (self, _) = thread_object.join_locked(kernel, self);
                }
            }
            self = process_handle.drop_locked(kernel, self);
        }
        #[cfg(not(feature = "user_space"))]
        {
            self.termination_queue.push_back(current_thread);
        }

        let _ = context_switch(kernel, self, current_thread_id, State::Terminated);

        // On some systems like M-profile ARM context switch is deferred if called
        // from an interrupt so this function may return.
    }

    pub(crate) fn thread_is_terminating(&self, thread: &ThreadHandle<K>) -> bool {
        // SAFETY: Exclusive access to thread is guaranteed by scheduler lock
        unsafe { thread.thread.as_ref().terminating }
    }

    fn thread_remove_from_parent_process(mut self, kernel: K, thread: &mut Thread<K>) -> Self {
        let mut process_handle = thread.take_process();

        // SAFETY: Thread is guaranteed to be in the process because the process
        // was just taken from the thread.
        self = unsafe { self.process_remove_from_thread_list(kernel, &mut process_handle, thread) };

        // ProcessHandle's drop takes the scheduler lock.  Since that is already
        // held, `drop_locked()` needs to be used instead to avoid recursively
        // locking the scheduler lock.
        process_handle.drop_locked(kernel, self)
    }
}

impl<K: Kernel> SpinLockGuard<'_, K, SchedulerState<K>> {
    /// Request termination of the referenced process and all its threads.
    ///
    /// This is an ASYNCHRONOUS operation. The process and its threads are marked
    /// as terminating, but they may not exit immediately.
    pub fn process_terminate(
        &mut self,
        _kernel: K,
        process_handle: &ProcessHandle<K>,
        status: ExitStatus,
    ) {
        let mut ptr = process_handle.process;
        pw_assert::assert!(
            ptr.as_ptr() != self.kernel_process.get(),
            "Tried to terminate the kernel process"
        );
        // SAFETY: `process_handle` guarantees that the pointed-to process is alive.
        // Holding the scheduler lock (via `&mut self`) guarantees exclusive mutable
        // access. The returned reference `proc` is local to this function
        // and does not outlive the lock.
        let proc = unsafe { ptr.as_mut() };

        if proc.state != ProcessState::Active {
            return;
        }

        // Set state to terminating
        proc.state = ProcessState::Terminating;
        proc.exit_status = Some(status);

        // Terminate all threads
        unsafe {
            proc.thread_list.filter(|thread| {
                log_if::info_if!(
                    LOG_SCHEDULER_EVENTS,
                    "Terminating thread '{}'",
                    thread.name as &str
                );
                let _ = self.thread_terminate_internal(thread, ExitStatus::ProcessTerminated);
                true // Keep in list
            });
        }
    }

    #[must_use]
    pub fn process_signal_join(mut self, kernel: K, process: &mut ProcessHandle<K>) -> Self {
        // SAFETY: inner process is protected by the scheduler lock and the
        // mutable reference does not live beyond the unsafe taking the signaler
        let signaler = unsafe { process.process.as_mut().join_event.take() };
        if let Some(signaler) = signaler {
            self = signaler.signal_locked(self);
        }

        // Raise JOINABLE signal.
        #[cfg(feature = "user_space")]
        if let Some(object) = unsafe { process.process.as_ref().object.as_ref() } {
            self = object.signal_locked(kernel, self, |s| s | syscall_defs::Signals::JOINABLE);
        }
        #[cfg(not(feature = "user_space"))]
        {
            // `kernel` is only used above when `user_space` is enabled.
            // Reference it here to suppress warning.
            let _ = kernel;
        }
        self
    }

    pub fn process_try_join(
        mut self,
        kernel: K,
        mut process_handle: ProcessHandle<K>,
        signaler: Option<EventSignaler<K>>,
    ) -> (Self, ProcessTryJoinResult<K>) {
        let process = unsafe { process_handle.process.as_ref() };

        // If the process is terminated and `process_handle` is the singular reference to it,
        // the process is terminated
        if process.state == ProcessState::Terminated
            && process.ref_count.load(Ordering::SeqCst) == 1
        {
            // SAFETY: The process is guaranteed to be in the process list
            // because it was passed in by ProcessHandle and the scheduler is
            // the only creator of those.
            #[allow(unused_mut)]
            let mut process_box = unsafe {
                self.process_list
                    .remove_element(process_handle.process)
                    .unwrap_unchecked()
            };
            process_box.state = ProcessState::Inactive;

            // Clear JOINABLE signal.
            #[cfg(feature = "user_space")]
            if let Some(object) = process_box.object.take() {
                self = object.signal_locked(kernel, self, |s| s - syscall_defs::Signals::JOINABLE);
            }
            #[cfg(not(feature = "user_space"))]
            {
                // `kernel` is only used above when `user_space` is enabled.
                // Reference it here to suppress warning.
                let _ = kernel;
            }
            let Some(status) = process_box.exit_status.take() else {
                pw_assert::panic!("Process joined with no exit status set");
            };
            return (self, ProcessTryJoinResult::Joined(process_box, status));
        }

        // Only one call to join is allowed to wait for process termination.  If
        // another caller is waiting, return an error.
        if process.join_event.is_some() {
            return (
                self,
                ProcessTryJoinResult::Err {
                    error: Error::AlreadyExists,
                    process: process_handle,
                },
            );
        }

        // Register the signaler and return None indicating the process is not yet
        // joinable.
        //
        // SAFETY: Process mutability guarded by scheduler lock.
        if let Some(signaler) = signaler {
            unsafe { process_handle.process.as_mut().join_event = Some(signaler) };
        }
        (self, ProcessTryJoinResult::Wait(process_handle))
    }

    pub fn process_cancel_try_join(&mut self, process_handle: &mut ProcessHandle<K>) {
        let process = unsafe { process_handle.process.as_mut() };
        pw_assert::assert!(process.join_event.is_some());
        process.join_event = None;
    }

    /// # Safety
    ///
    /// Caller must ensure that the thread is already in the processes thread list.
    unsafe fn process_remove_from_thread_list(
        mut self,
        kernel: K,
        process_handle: &mut ProcessHandle<K>,
        thread: &mut Thread<K>,
    ) -> Self {
        // SAFETY: Scheduler lock is held so it is safe to manipulate the process
        // list and the caller guarantees that the thread is in said list so it
        // is safe to remove it.

        // SAFETY: The scheduler lock ensures mutual exclusion and the mutable
        // reference does not live out side this function.
        let process = unsafe { process_handle.process.as_mut() };

        // SAFETY: Caller guarantees that the thread is in the process' thread list
        // and the list is protected by the scheduler lock.
        let empty = unsafe {
            process
                .thread_list
                .unlink_element_unchecked(NonNull::from(thread));

            process.thread_list.is_empty()
        };

        if process.state == ProcessState::Terminating && empty {
            process.state = ProcessState::Terminated;
            self = self.process_signal_join(kernel, process_handle);
        }
        self
    }

    pub fn process_add_thread(
        &self,
        process_handle: &mut ProcessHandle<K>,
        thread_handle: &mut ThreadHandle<K>,
    ) -> Result<()> {
        // SAFETY: Scheduler lock is held ensuring exclusive access to process_handle
        // and thread_handle as well as fulfilling the precondition of `add_to_thread_list`.
        unsafe {
            let process = process_handle.process.as_mut();
            if process.state != ProcessState::Active {
                return Err(Error::FailedPrecondition);
            }
            process.add_to_thread_list(thread_handle.thread.as_mut());
        }
        Ok(())
    }
}

#[must_use]
pub(crate) fn block<K: Kernel>(
    kernel: K,
    sched_state: SpinLockGuard<K, SchedulerState<K>>,
    current_thread_id: usize,
    current_thread_state: State,
) -> SpinLockGuard<K, SchedulerState<K>> {
    let (sched_state, switched) =
        context_switch(kernel, sched_state, current_thread_id, current_thread_state);
    pw_assert::assert!(switched, "Blocking requires a context switch");
    sched_state
}

#[must_use]
pub(crate) fn context_switch<K: Kernel>(
    kernel: K,
    mut sched_state: SpinLockGuard<K, SchedulerState<K>>,
    current_thread_id: usize,
    current_thread_state: State,
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

    trace_context_switch(
        kernel,
        current_thread_id,
        new_thread.id(),
        current_thread_state,
    );

    let old_thread_state = sched_state.current_arch_thread_state;
    let new_thread_state = new_thread.arch_thread_state.get();
    sched_state.set_current_thread(new_thread);
    unsafe { kernel.context_switch(sched_state, old_thread_state, new_thread_state) }
}

/// If try_reschedule() was called while preemption was disabled, try to
/// reschedule again after the preempt guard is dropped.
pub fn try_deferred_reschedule<K: Kernel>(kernel: K, reason: RescheduleReason) {
    if Ok(true)
        == kernel
            .thread_local_state()
            .needs_reschedule
            .compare_exchange(true, false, Ordering::SeqCst, Ordering::SeqCst)
    {
        let _ = kernel
            .get_scheduler()
            .lock(kernel)
            .try_reschedule(kernel, reason);
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
pub fn tick<K: Kernel>(
    kernel: K,
    now: Instant<K::Clock>,
    mut guard: crate::interrupt_controller::InterruptGuard<K>,
) -> crate::interrupt_controller::InterruptGuard<K> {
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
            >= 1,
        "scheduler::tick() called with preemption enabled"
    );

    // In lieu of a proper timer interface, the scheduler needs to be robust
    // to timer ticks arriving before it is initialized.
    if kernel.get_scheduler().lock(kernel).current_thread.is_none() {
        return guard;
    }

    crate::scheduler::timer::process_queue(kernel, now);

    guard.set_reschedule_reason(RescheduleReason::Ticked);
    let _ = kernel
        .get_scheduler()
        .lock(kernel)
        .try_reschedule(kernel, RescheduleReason::Ticked);

    guard
}

/// Break the global scheduler lock.
///
/// This method is used to forcibly release the scheduler lock without dropping the
/// guard. This is unsafe because it breaks the lock invariants and can
/// lead to data corruption if not used carefully.
///
/// # Safety
/// This method should only be called on thread start.
/// The caller must ensure that breaking the lock will not cause data corruption.
pub unsafe fn break_scheduler_lock<K: Kernel>(kernel: K) {
    unsafe {
        kernel.get_scheduler().break_lock();
    }
}

pub fn exit_thread<K: Kernel>(kernel: K, status: ExitStatus) -> ! {
    let mut sched = kernel.get_scheduler().lock(kernel);
    let current_thread = sched.current_thread_mut();
    current_thread.exit_status = Some(status);
    sched.thread_exit(kernel);

    // This thread should never get scheduled again at this point.  This assert
    // firing indicates a scheduler or context switch bug.
    pw_assert::panic!("thread_exit returned unexpectedly");
}

/// Handle a terminal exception in the current thread.
///
/// This should only be called from an architecture specific exception handler.
/// An InterruptGuard is passed in and out in order to allow it to take
/// architecturally specific action for thread termination.
///
/// If the exception occurred in user mode, the process will be terminated.
/// If the exception occurred in kernel mode, the kernel will panic.
pub fn handle_terminal_exception<K: Kernel>(
    kernel: K,
    guard: crate::interrupt_controller::InterruptGuard<K>,
) -> crate::interrupt_controller::InterruptGuard<K> {
    if !guard.from_userspace() {
        pw_assert::panic!("Terminal exception in kernel mode");
    }

    let mut sched = kernel.get_scheduler().lock(kernel);
    let process_handle = sched.current_thread().process();
    sched.process_terminate(kernel, &process_handle, ExitStatus::UnhandledException(0));

    guard
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
