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

use core::mem::ManuallyDrop;
use core::ops::Range;
use core::ptr::NonNull;
use core::sync::atomic::Ordering;

use exit_status::ExitStatus;
#[cfg(not(feature = "user_space"))]
use foreign_box::ForeignBox;
#[cfg(feature = "user_space")]
use foreign_box::{ForeignBox, ForeignRc};
use list::*;
use memory_config::{MemoryConfig as _, MemoryRegionType};
use pw_atomic::{AtomicAdd, AtomicSub, AtomicZero};
use pw_log::info;
use pw_status::Result;
use pw_time_core::Instant;

use crate::Kernel;
#[cfg(feature = "user_space")]
use crate::object::{KernelObject, ObjectTable};
use crate::scheduler::SchedulerState;
use crate::scheduler::thread::{Thread, ThreadHandle, ThreadState};
use crate::sync::event::{Event, EventConfig, EventSignaler};
use crate::sync::spinlock::SpinLockGuard;

/// Runtime state of a Process.
#[derive(Copy, Clone, PartialEq)]
pub enum ProcessState {
    /// Process is registered with the scheduled because it is either new or has been joined.
    Inactive,

    /// Process is registered with the scheduler and participates in execution.
    Active,

    /// Process termination has been requested; threads are being terminated.
    Terminating,

    /// Process has no active threads and is terminated, waiting to be joined.
    Terminated,
}

pub(super) fn process_state_to_string(s: ProcessState) -> &'static str {
    match s {
        ProcessState::Inactive => "Inactive",
        ProcessState::Active => "Active",
        ProcessState::Terminating => "Terminating",
        ProcessState::Terminated => "Terminated",
    }
}

pub struct Process<K: Kernel> {
    // List of the processes in the system
    pub link: Link,

    // TODO - konkers: allow this to be tokenized.
    pub name: &'static str,

    pub(crate) memory_config: <K::ThreadState as ThreadState>::MemoryConfig,

    #[cfg(feature = "user_space")]
    object_table: ForeignBox<dyn ObjectTable<K>>,

    #[cfg(feature = "user_space")]
    pub object: Option<foreign_box::ForeignRc<K::AtomicUsize, crate::object::ProcessObject<K>>>,

    pub(super) thread_list: UnsafeList<Thread<K>, ProcessThreadListAdapter<K>>,

    pub(super) ref_count: K::AtomicUsize,
    pub(super) state: ProcessState,
    pub(super) join_event: Option<EventSignaler<K>>,
    pub(super) exit_status: Option<ExitStatus>,
}

// Safety: Process internal state is protected by the scheduler lock or atomic operations.
unsafe impl<K: Kernel> Send for Process<K> {}
unsafe impl<K: Kernel> Sync for Process<K> {}

list::define_adapter!(pub ProcessListAdapter<K: Kernel> => Process<K>::link);
list::define_adapter!(pub ProcessThreadListAdapter<K: Kernel> => Thread<K>::process_link);

impl<K: Kernel> Process<K> {
    /// Creates a new, empty, unregistered process.
    #[must_use]
    pub const fn new(
        name: &'static str,
        memory_config: <K::ThreadState as ThreadState>::MemoryConfig,
        #[cfg(feature = "user_space")] object_table: ForeignBox<dyn ObjectTable<K>>,
    ) -> Self {
        Self {
            link: Link::new(),
            name,
            memory_config,
            #[cfg(feature = "user_space")]
            object_table,
            #[cfg(feature = "user_space")]
            object: None,
            thread_list: UnsafeList::new(),
            ref_count: K::AtomicUsize::ZERO,
            state: ProcessState::Inactive,
            join_event: None,
            exit_status: None,
        }
    }

    /// Manually increment the processes refcount
    ///
    /// SAFETY: This should only be done by the scheduler to maintain a
    /// virtual, reference on the kernel process during initialization.
    pub(super) unsafe fn manually_increment_ref_count(&mut self) {
        self.ref_count.fetch_add(1, Ordering::Acquire);
    }

    #[cfg(feature = "user_space")]
    pub fn get_object(
        &self,
        kernel: K,
        handle: u32,
    ) -> Option<ForeignRc<K::AtomicUsize, dyn KernelObject<K>>> {
        self.object_table.get_object(kernel, handle)
    }

    #[cfg(feature = "user_space")]
    pub fn reset_objects(&self, kernel: K) -> Result<()> {
        self.object_table.reset_all(kernel)
    }

    /// # Safety
    /// Caller must ensure exclusive access to the process and `thread` and all
    /// threads in the process' thread list.  This can be accomplished by holding
    /// the scheduler lock.
    pub unsafe fn add_to_thread_list(&mut self, thread: &mut Thread<K>) {
        // SAFETY: Caller guarantees exclusive access to the process and `thread`
        // and all threads in the process' thread list.
        unsafe {
            let thread = NonNull::from(thread);
            pw_assert::assert!(!self.thread_list.is_element_linked(thread));
            self.thread_list.push_front_unchecked(thread);
        }
    }

    pub fn range_has_access(&self, access_type: MemoryRegionType, range: Range<usize>) -> bool {
        self.memory_config
            .range_has_access(access_type, range.start, range.end)
    }

    /// A simple ID for debugging purposes, currently the pointer to the thread
    /// structure itself.
    ///
    /// # Safety
    ///
    /// The returned value should not be relied upon as being a valid pointer.
    /// Even in the current implementation, `id` does not expose the pointer's
    /// provenance.
    #[must_use]
    pub fn id(&self) -> usize {
        // Note: any changes to this also need to be reflected in
        // `const_id_from_process_address`.
        core::ptr::from_ref(self).addr()
    }

    /// Computes the ID of a process from its address.
    ///
    /// # Safety
    ///
    /// This exists to allow process annotations to be generated at compile time
    /// for static processes that are initialized at runtime. This should not be
    /// called in any other circumstance.
    #[must_use]
    pub const unsafe fn const_id_from_process_address(addr: *const ()) -> *const () {
        addr
    }

    pub fn dump(&self, kernel: K) {
        info!(
            "Process '{}' ({:#010x}) state: {}",
            self.name as &str,
            self.id() as usize,
            process_state_to_string(self.state) as &str
        );
        #[cfg(feature = "user_space")]
        {
            self.object_table.dump(kernel);
        }
        #[cfg(not(feature = "user_space"))]
        {
            // Suppress unused warning when user_space not enabled.
            let _ = kernel;
        }
        unsafe {
            let _ = self
                .thread_list
                .for_each(|thread| -> core::result::Result<(), ()> {
                    thread.dump();
                    Ok(())
                });
        }
    }
}

pub struct ProcessHandle<K: Kernel> {
    pub(crate) process: NonNull<Process<K>>,
    kernel: K,
}

// Safety: ProcessHandle is a handle to a reference counted process.  All operations
// on the process are protected by the scheduler lock or atomic operations.
unsafe impl<K: Kernel> Send for ProcessHandle<K> {}
unsafe impl<K: Kernel> Sync for ProcessHandle<K> {}

impl<K: Kernel> ProcessHandle<K> {
    // `ProcessHandle`s should only be created when adding a process to the
    // scheduler.
    pub(super) fn new(process: NonNull<Process<K>>, kernel: K) -> Self {
        unsafe {
            process.as_ref().ref_count.fetch_add(1, Ordering::Acquire);
        }
        Self { process, kernel }
    }

    /// Request termination of the process.
    ///
    /// Sets the process state to Terminating and requests termination for all
    /// threads in the process.
    ///
    /// This is an ASYNCHRONOUS operation.
    pub fn terminate(&self, kernel: K, status: ExitStatus) {
        kernel
            .get_scheduler()
            .lock(kernel)
            .process_terminate(kernel, self, status);
    }

    /// Returns the current state of the process.
    #[must_use]
    pub fn get_state_locked(
        &self,
        _sched: &SpinLockGuard<'_, K, SchedulerState<K>>,
    ) -> ProcessState {
        // SAFETY: `self.process` is guaranteed to be valid because it is reference
        // counted.  The state is guaranteed to be coherent because the sched lock
        // is taken.
        unsafe { self.process.as_ref().state }
    }

    pub fn join(self, kernel: K) -> Result<(ForeignBox<Process<K>>, ExitStatus)> {
        match self.join_until(kernel, Instant::<K::Clock>::MAX) {
            crate::scheduler::ProcessJoinResult::Joined(process, status) => {
                #[cfg(feature = "user_space")]
                process.reset_objects(kernel)?;
                Ok((process, status))
            }
            crate::scheduler::ProcessJoinResult::Err { error, .. } => Err(error),
        }
    }

    /// Attempts to join the referenced process.
    ///
    /// If `ProcessTryJoinResult::Joined` is returned, the caller is responsible
    /// for resetting the process's objects using `process.reset_objects(kernel)`.
    pub fn try_join(self, kernel: K) -> crate::scheduler::ProcessTryJoinResult<K> {
        let (_, res) = kernel
            .get_scheduler()
            .lock(kernel)
            .process_try_join(kernel, self, None);
        res
    }

    /// Join the referenced process with a deadline
    ///
    /// Waits until the all other references to the process are dropped and the
    /// process terminates.
    pub fn join_until(
        mut self,
        kernel: K,
        deadline: Instant<K::Clock>,
    ) -> crate::scheduler::ProcessJoinResult<K> {
        let join_event = Event::new(kernel, EventConfig::ManualReset);
        loop {
            let (_, res) = kernel.get_scheduler().lock(kernel).process_try_join(
                kernel,
                self,
                Some(join_event.get_signaler()),
            );
            match res {
                crate::scheduler::ProcessTryJoinResult::Err {
                    error: e,
                    process: process_handle,
                } => {
                    return crate::scheduler::ProcessJoinResult::Err {
                        error: e,
                        process: process_handle,
                    };
                }
                crate::scheduler::ProcessTryJoinResult::Joined(process_box, status) => {
                    return crate::scheduler::ProcessJoinResult::Joined(process_box, status);
                }
                crate::scheduler::ProcessTryJoinResult::Wait(process_handle) => {
                    self = process_handle
                }
            };

            if let Err(e) = join_event.wait_until(deadline) {
                kernel
                    .get_scheduler()
                    .lock(kernel)
                    .process_cancel_try_join(&mut self);

                return crate::scheduler::ProcessJoinResult::Err {
                    error: e,
                    process: self,
                };
            }
        }
    }

    /// Drop the `ProcessHandle` while the scheduler lock is held.
    ///
    /// This is an internal version for use by the scheduler while it is holding
    /// the scheduler lock.
    pub(super) fn drop_locked(
        mut self,
        kernel: K,
        mut sched: SpinLockGuard<'_, K, SchedulerState<K>>,
    ) -> SpinLockGuard<'_, K, SchedulerState<K>> {
        unsafe {
            let prev_value = self
                .process
                .as_ref()
                .ref_count
                .fetch_sub(1, Ordering::Release);

            // If this ref was one of two outstanding references to the process,
            // the other reference may be attempting to join.  Let the scheduler
            // notify the join request if it is outstanding.
            if prev_value == 2 {
                sched = sched.process_signal_join(kernel, &mut self);
            }
        }

        // Ensure that the trait version of drop is not run.
        let _ = ManuallyDrop::new(self);
        sched
    }

    pub fn add_thread(&mut self, kernel: K, thread_handle: &mut ThreadHandle<K>) -> Result<()> {
        kernel
            .get_scheduler()
            .lock(kernel)
            .process_add_thread(self, thread_handle)
    }

    pub fn range_has_access_locked(
        &self,
        _sched: &SpinLockGuard<'_, K, SchedulerState<K>>,
        region_type: MemoryRegionType,
        range: Range<usize>,
    ) -> bool {
        // SAFETY: `self.process` is valid because `ProcessHandle` guarantees the
        // lifetime of the pointed-to `Process`.
        unsafe { self.process.as_ref().range_has_access(region_type, range) }
    }
}

impl<K: Kernel> Clone for ProcessHandle<K> {
    fn clone(&self) -> Self {
        unsafe {
            self.process
                .as_ref()
                .ref_count
                .fetch_add(1, Ordering::Acquire);
        }

        Self {
            process: self.process,
            kernel: self.kernel,
        }
    }
}

impl<K: Kernel> Drop for ProcessHandle<K> {
    fn drop(&mut self) {
        unsafe {
            let prev_value = self
                .process
                .as_ref()
                .ref_count
                .fetch_sub(1, Ordering::Release);

            // If this ref was one of two outstanding references to the process,
            // the other reference may be attempting to join.  Let the scheduler
            // notify the join request if it is outstanding.
            if prev_value == 2 {
                let _ = self
                    .kernel
                    .get_scheduler()
                    .lock(self.kernel)
                    .process_signal_join(self.kernel, self);
            }
        }
    }
}

/// Add debug annotations for a static process
///
/// This macro is called by `init_non_priv_process` and should not be called
/// directly.
#[macro_export]
macro_rules! annotate_process_from_address {
    ($name:expr, $arch:ty, $process_address:expr) => {{
        #[repr(C, packed(1))]
        struct ProcessAnnotation {
            name: &'static str,
            id: *const (),
        }
        unsafe impl Sync for ProcessAnnotation {};

        #[unsafe(link_section = ".pw_kernel.annotations.process")]
        #[used]
        static _PROCESS_ANNOTATION: ProcessAnnotation = ProcessAnnotation {
            name: $name,
            id: unsafe {
                $crate::Process::<$arch>::const_id_from_process_address(
                    $process_address as *const (),
                )
            },
        };
    }};
}

/// Declare static storage for a non-privileged process.
///
/// Creates a static in named `storage_ident` in the calling scope which holds
/// the storage for a static process.
#[cfg(feature = "user_space")]
#[macro_export]
macro_rules! declare_non_priv_process_storage {
    ($storage_ident:ident) => {
        static $storage_ident: $crate::__private::foreign_box::StaticStorage<
            $crate::scheduler::Process<arch::Arch>,
        > = $crate::__private::foreign_box::StaticStorage::new();
    };
}

/// Get the constant process ID of a non-privileged process.
#[cfg(feature = "user_space")]
#[macro_export]
macro_rules! non_priv_process_id {
    ($storage:expr) => {{
        use $crate::scheduler::Process;
        unsafe {
            Process::<arch::Arch>::const_id_from_process_address(unsafe { $storage.address() })
        }
    }};
}

/// Declares a new [`Process`] in a previously declared static process storage.
///
/// # Safety
///
/// Each invocation of `declare_non_priv_process!` must be executed at most once at
/// run time.
#[cfg(feature = "user_space")]
#[macro_export]
macro_rules! declare_non_priv_process {
    ($storage:expr, $name:literal, $memory_config:expr, $object_table:expr $(,)?) => {{
        use $crate::__private::foreign_box::StaticStorage;
        use $crate::Kernel;
        use $crate::object::ObjectTable;
        use $crate::scheduler::Process;

        /// SAFETY: This must be executed at most once at run time.  See
        /// StaticStorage::init() for more details.
        #[allow(clippy::mut_from_ref)]
        unsafe fn __declare_non_priv_process(
            storage: &'static StaticStorage<Process<arch::Arch>>,
            object_table: ForeignBox<dyn ObjectTable<arch::Arch>>,
        ) -> ForeignBox<Process<arch::Arch>> {
            use pw_log::info;
            use $crate::__private::foreign_box::ForeignBox;
            info!(
                "Allocating non-privileged process '{}'",
                $name as &'static str
            );

            // SAFETY: The caller promises that this function will be executed
            // at most once.
            let proc = unsafe { storage.init(Process::new($name, $memory_config, object_table)) };
            ForeignBox::from(proc)
        }

        $crate::annotate_process_from_address!($name, arch::Arch, unsafe { $storage.address() });
        __declare_non_priv_process(&$storage, $object_table)
    }};
}
