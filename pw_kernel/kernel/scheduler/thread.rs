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

use core::cell::UnsafeCell;
use core::mem::MaybeUninit;
use core::ptr::NonNull;
use core::sync::atomic::Ordering;

use exit_status::ExitStatus;
#[cfg(not(feature = "user_space"))]
use foreign_box::ForeignBox;
#[cfg(feature = "user_space")]
use foreign_box::{ForeignBox, ForeignRc};
use list::*;
use pw_atomic::{AtomicAdd, AtomicSub, AtomicZero};
use pw_log::info;
use pw_status::Result;
use time::Instant;

use crate::Kernel;
#[cfg(feature = "user_space")]
use crate::object::KernelObject;
use crate::scheduler::algorithm::SchedulerAlgorithmThreadState;
use crate::scheduler::process::ProcessHandle;
use crate::scheduler::{JoinResult, Priority, SchedulerState, TryJoinResult, WaitQueue, WaitType};
use crate::sync::event::{Event, EventConfig, EventSignaler};
use crate::sync::spinlock::SpinLockGuard;

/// The memory backing a thread's stack before it has been started.
///
/// Stacks are aligned to 8 bytes for broad ABI compatibility.
///
/// After a thread has been started, ownership of its stack's memory is (from
/// the Rust Abstract Machine (AM) perspective) relinquished, and so the type we
/// use to represent that memory is irrelevant.
///
/// However, while we are initializing a thread in preparation for starting it,
/// we are operating on that memory as a normal Rust variable, and so its type
/// is important.
///
/// Using `MaybeUninit<u8>` instead of `u8` is important for two reasons:
/// - It ensures that it is sound to write values which are not entirely
///   initialized (e.g., which contain padding bytes).
/// - It ensures that pointers written to the stack [retain
///   provenance][provenance].
///
/// [provenance]: https://github.com/rust-lang/unsafe-code-guidelines/issues/286#issuecomment-2837585644
#[repr(align(8))]
pub struct StackStorage<const N: usize> {
    pub stack: [MaybeUninit<u8>; N],
}

pub trait StackStorageExt {
    const ZEROED: Self;
}

impl<const N: usize> StackStorageExt for StackStorage<N> {
    const ZEROED: StackStorage<N> = StackStorage {
        stack: [MaybeUninit::new(0); N],
    };
}

#[derive(Clone, Copy)]
pub struct Stack {
    // Starting (lowest) address of the stack.  Inclusive.
    start: *const MaybeUninit<u8>,

    // Ending (highest) address of the stack.  Exclusive.
    end: *const MaybeUninit<u8>,
}

#[allow(dead_code)]
impl Stack {
    #[must_use]
    pub const fn from_slice(slice: &[MaybeUninit<u8>]) -> Self {
        let start: *const MaybeUninit<u8> = slice.as_ptr();
        // Safety: offset based on known size of slice.
        let end = unsafe { start.add(slice.len()) };
        Self { start, end }
    }

    #[must_use]
    pub const fn new() -> Self {
        Self {
            start: core::ptr::null(),
            end: core::ptr::null(),
        }
    }

    #[must_use]
    pub fn start(&self) -> *const MaybeUninit<u8> {
        self.start
    }

    #[must_use]
    pub fn end(&self) -> *const MaybeUninit<u8> {
        self.end
    }

    /// # Safety
    /// Caller must ensure exclusive mutable access to underlying data
    #[must_use]
    pub unsafe fn end_mut(&self) -> *mut MaybeUninit<u8> {
        self.end as *mut MaybeUninit<u8>
    }

    #[must_use]
    pub fn contains(&self, ptr: *const MaybeUninit<u8>) -> bool {
        ptr >= self.start && ptr < self.end
    }

    #[must_use]
    pub fn aligned_stack_allocation_mut<T: Sized>(
        sp: *mut MaybeUninit<u8>,
        alignment: usize,
    ) -> *mut T {
        let sp = sp.wrapping_byte_sub(size_of::<T>());
        let offset = sp.align_offset(alignment);
        if offset > 0 {
            sp.wrapping_byte_sub(alignment - offset).cast()
        } else {
            sp.cast()
        }
    }

    pub fn aligned_stack_allocation<T: Sized>(
        sp: *mut MaybeUninit<u8>,
        alignment: usize,
    ) -> *const T {
        Self::aligned_stack_allocation_mut::<*mut T>(sp, alignment).cast()
    }

    /// Initialize the stack for thread execution.
    ///
    /// Initializes the stack to a known pattern to avoid leaking data between
    /// thread invocations as well as to provide a signature for calculating
    /// high water stack usage.
    pub fn initialize(&self) {
        let mut ptr = self.start as *mut u32;
        let end = self.end as *mut u32;

        pw_assert::assert!(ptr.is_aligned(), "Stack start is not aligned");
        pw_assert::assert!(end.is_aligned(), "Stack end is not aligned");

        while ptr < end {
            // SAFETY: `ptr` is always aligned to 4 bytes and is contained within
            // the specified stack memory region.  Additionally, only writes
            // are performed for the MaybeUninit constraint is not violated.
            unsafe {
                ptr.write_volatile(magic_values::UNUSED_STACK_PATTERN);
                ptr = ptr.add(1);
            }
        }
    }
}

/// Runtime state of a Thread.
// TODO: want to name this ThreadState, but collides with ArchThreadstate
#[derive(Copy, Clone, PartialEq)]
#[repr(u8)]
pub enum State {
    /// Thread has been created but not initialized.
    New = 0,

    /// Thread has been initialized and added to its parent process but has not
    /// been added to the scheduler.
    Initial = 1,

    /// Thread is ready to run and owned by the scheduling algorithm.
    Ready = 2,

    /// Thread is currently running on a CPU core.
    Running = 3,

    /// Thread has been successfully terminated and is waiting to be joined.
    Terminated = 4,

    /// Thread has been joined, removed from its parent process, and no longer
    /// participates in the system scheduler.
    Joined = 5,

    /// Thread is interruptibly waiting in a [`WaitQueue`].
    WaitingInterruptible = 6,

    /// Thread is non-interruptibly waiting in a [`WaitQueue`].
    WaitingNonInterruptible = 7,
}

impl State {
    #[must_use]
    pub fn is_waiting(&self) -> bool {
        matches!(
            self,
            Self::WaitingInterruptible | Self::WaitingNonInterruptible
        )
    }
}

// TODO: use From or Into trait (unclear how to do it with 'static str)
pub(super) fn to_string(s: State) -> &'static str {
    match s {
        State::New => "New",
        State::Initial => "Initial",
        State::Ready => "Ready",
        State::Running => "Running",
        State::Terminated => "Terminated",
        State::Joined => "Joined",
        State::WaitingInterruptible => "WaitingInterruptible",
        State::WaitingNonInterruptible => "WaitingNonInterruptible",
    }
}

pub trait ThreadState: 'static + Sized {
    const NEW: Self;

    // TODO: Maybe have a `MemoryConfigContext` super-trait of `ThreadState`?
    type MemoryConfig: memory_config::MemoryConfig;

    /// Initialize the default frame of a kernel thread
    ///
    /// Arranges for the thread to start at `initial_function` with arguments
    /// passed in the first two argument slots.  The stack pointer of the thread
    /// is set to the top of the kernel stack.
    ///
    /// # Safety
    /// Caller guarantees that the `memory_config` pointer remains valid for the
    /// lifetime of the thread.
    ///
    /// # Implementation Note
    /// The implementation is responsible for ensuring that `initial_function`
    /// is called with interrupts enabled and without the scheduler lock held.
    /// This call must initialize the `ThreadLocalState` returned by
    /// `thread_local_state()`.
    unsafe fn initialize_kernel_state(
        &mut self,
        kernel_stack: Stack,
        memory_config: *const Self::MemoryConfig,
        initial_function: extern "C" fn(usize, usize, usize),
        args: (usize, usize, usize),
    );

    /// Initialize the default frame of a user thread
    ///
    /// Arranges for the thread to start at `initial_function` with arguments
    /// passed in the first two argument slots
    ///
    /// # Safety
    /// Caller guarantees that the `memory_config` pointer remains valid for the
    /// lifetime of the thread.
    ///
    /// # Implementation Note
    /// The implementation is responsible for ensuring that `initial_function`
    /// is called with interrupts enabled, memory_config enabled, non-privileged
    /// mode, and without the scheduler lock held.
    /// This call must initialize the `ThreadLocalState` returned by
    /// `thread_local_state()`.
    #[cfg(feature = "user_space")]
    unsafe fn initialize_user_state(
        &mut self,
        kernel_stack: Stack,
        memory_config: *const Self::MemoryConfig,
        initial_sp: usize,
        initial_pc: usize,
        args: (usize, usize, usize),
    ) -> Result<()>;
}

pub enum ThreadOwner<K: Kernel> {
    None,
    Scheduler,
    WaitQueue {
        queue: NonNull<WaitQueue<K>>,
        wait_type: WaitType,
    },
}

/// A reference counted reference to a [`Thread`].
///
/// A `ThreadHandle` can has a limited set of APIs that are safe to call without
/// holding the scheduler lock.
pub struct ThreadHandle<K: Kernel> {
    pub(crate) thread: NonNull<Thread<K>>,
    kernel: K,
}

// Safety: ThreadHandle is a handle to a reference counted thread.  All operations
// on the thread are protected by the scheduler lock or atomic operations.
unsafe impl<K: Kernel> Send for ThreadHandle<K> {}
unsafe impl<K: Kernel> Sync for ThreadHandle<K> {}

impl<K: Kernel> ThreadHandle<K> {
    /// Join the referenced thread.
    ///
    /// Waits until the all other references to the thread are dropped and the
    /// thread terminates.  Returns a `ForeignBox<Thread<K>>` which can be used
    /// to restart the thread.
    pub fn join(self, kernel: K) -> Result<(ForeignBox<Thread<K>>, ExitStatus)> {
        match self.join_until(kernel, Instant::<K::Clock>::MAX) {
            JoinResult::Joined(thread, status) => Ok((thread, status)),
            JoinResult::Err { error, .. } => Err(error),
        }
    }

    pub fn try_join(self, kernel: K) -> crate::scheduler::TryJoinResult<K> {
        let sched = kernel.get_scheduler().lock(kernel);
        let (_sched, res) = self.try_join_locked(kernel, sched);
        res
    }

    pub fn try_join_locked<'a>(
        self,
        kernel: K,
        sched: SpinLockGuard<'a, K, SchedulerState<K>>,
    ) -> (
        SpinLockGuard<'a, K, SchedulerState<K>>,
        crate::scheduler::TryJoinResult<K>,
    ) {
        sched.thread_try_join(kernel, self, None)
    }

    /// Join the referenced thread with a deadline
    ///
    /// Waits until the all other references to the thread are dropped and the
    /// thread terminates.
    ///
    /// Returns:
    /// - `Ok(thread)`: Success. `thread` can be used to restart the thread.
    /// - `Err(Error::DeadlineExceeded: The thread did not enter a joinable state
    ///   before `deadline` was passed.
    pub fn join_until(mut self, kernel: K, deadline: Instant<K::Clock>) -> JoinResult<K> {
        let join_event = Event::new(kernel, EventConfig::ManualReset);
        loop {
            let (_, res) = kernel.get_scheduler().lock(kernel).thread_try_join(
                kernel,
                self,
                Some(join_event.get_signaler()),
            );
            match res {
                TryJoinResult::Err {
                    error: e,
                    thread: thread_handle,
                } => {
                    return JoinResult::Err {
                        error: e,
                        thread: thread_handle,
                    };
                }
                TryJoinResult::Joined(thread_box, status) => {
                    return JoinResult::Joined(thread_box, status);
                }
                TryJoinResult::Wait(thread_handle) => self = thread_handle,
            };

            if let Err(e) = join_event.wait_until(deadline) {
                kernel
                    .get_scheduler()
                    .lock(kernel)
                    .thread_cancel_try_join(&mut self);

                return JoinResult::Err {
                    error: e,
                    thread: self,
                };
            }
        }
    }

    /// Request termination of the thread.
    ///
    /// Asynchronously requests the referenced thread to terminate.  While in the terminating
    /// state:
    /// - The thread's `terminating` field will be set to `true`.
    /// - Any active interruptible waits will be canceled with `Error::Cancelled`.
    /// - Any new interruptible wait will immediately return `Error::Cancelled`.
    /// - All non-interruptible waits will work as normal.
    ///
    /// This is an ASYNCHRONOUS operation. The thread is marked as terminating,
    /// but it may not exit immediately.
    ///
    /// To wait for the thread to terminate, call [`ThreadHandle::join()`] or
    /// [`ThreadHandle::join_until()`].
    pub fn terminate(&mut self, kernel: K, status: ExitStatus) -> Result<()> {
        kernel
            .get_scheduler()
            .lock(kernel)
            .thread_terminate(self, status)
    }

    /// Returns the current state of the thread.
    pub fn get_state(&self, kernel: K) -> State {
        kernel.get_scheduler().lock(kernel).thread_get_state(self)
    }

    /// Returns true if the thread is in the terminating state.
    ///
    /// Note: This is a parallel state to the state returned by [`ThreadHandle::is_terminating()`].
    pub fn is_terminating(&self, kernel: K) -> bool {
        kernel
            .get_scheduler()
            .lock(kernel)
            .thread_is_terminating(self)
    }
}

impl<K: Kernel> Clone for ThreadHandle<K> {
    fn clone(&self) -> Self {
        unsafe {
            self.thread
                .as_ref()
                .ref_count
                .fetch_add(1, Ordering::Acquire);
        }

        Self {
            thread: self.thread,
            kernel: self.kernel,
        }
    }
}

impl<K: Kernel> Drop for ThreadHandle<K> {
    fn drop(&mut self) {
        unsafe {
            let prev_value = self
                .thread
                .as_ref()
                .ref_count
                .fetch_sub(1, Ordering::Release);

            // If this ref was one of two outstanding references to the thread,
            // the other reference may be attempting to join.  Let the scheduler
            // notify the join request if it is outstanding.
            if prev_value == 2 {
                self.kernel
                    .get_scheduler()
                    .lock(self.kernel)
                    .thread_signal_join(self);
            }
        };
    }
}

pub struct Thread<K: Kernel> {
    // List of threads in a given process.
    pub process_link: Link,

    // Active state link (run queue, wait queue, etc)
    pub active_link: Link,

    process: Option<ProcessHandle<K>>,

    pub(super) state: State,
    pub(super) stack: Stack,

    // Architecturally specific thread state, saved on context switch
    pub arch_thread_state: UnsafeCell<K::ThreadState>,

    pub(super) owner: ThreadOwner<K>,
    pub(super) ref_count: K::AtomicUsize,
    pub(super) terminating: bool,
    pub(super) join_event: Option<EventSignaler<K>>,
    pub(super) exit_status: Option<ExitStatus>,

    // TODO - konkers: allow this to be tokenized.
    pub name: &'static str,

    /// The state for the scheduler algorithm.
    pub algorithm_state: SchedulerAlgorithmThreadState,

    #[cfg(feature = "user_space")]
    pub object: Option<foreign_box::ForeignRc<K::AtomicUsize, crate::object::ThreadObject<K>>>,
}

// SAFETY: Thread internal state is protected by the scheduler lock or atomic operations.
unsafe impl<K: Kernel> Send for Thread<K> {}
unsafe impl<K: Kernel> Sync for Thread<K> {}

list::define_adapter!(pub ThreadListAdapter<K: Kernel> => Thread<K>::active_link);

impl<K: Kernel> Thread<K> {
    pub(super) fn take_process(&mut self) -> ProcessHandle<K> {
        let Some(process) = self.process.take() else {
            pw_assert::panic!("Thread does not have a process");
        };
        process
    }

    pub fn get_exit_status(&self) -> Option<ExitStatus> {
        self.exit_status
    }

    pub fn is_terminating(&self) -> bool {
        self.terminating
    }

    pub fn set_stack(&mut self, stack: Stack) {
        pw_assert::assert!(matches!(self.state, State::New | State::Joined));
        self.stack = stack;
    }

    // Create an empty, uninitialized thread
    #[must_use]
    pub const fn new(name: &'static str, priority: Priority, stack: Stack) -> Self {
        Thread {
            process_link: Link::new(),
            active_link: Link::new(),
            process: None,
            state: State::New,
            arch_thread_state: UnsafeCell::new(K::ThreadState::NEW),
            owner: ThreadOwner::None,
            stack,
            ref_count: K::AtomicUsize::ZERO,
            terminating: false,
            join_event: None,
            exit_status: None,
            name,
            algorithm_state: SchedulerAlgorithmThreadState::new(priority),
            #[cfg(feature = "user_space")]
            object: None,
        }
    }

    #[cfg(feature = "user_space")]
    pub fn get_object(
        &self,
        kernel: K,
        handle: u32,
    ) -> Option<ForeignRc<K::AtomicUsize, dyn KernelObject<K>>> {
        // SAFETY: `self.process` will always outlive `self`.
        unsafe {
            self.process
                .as_ref()?
                .process
                .as_ref()
                .get_object(kernel, handle)
        }
    }

    pub(super) extern "C" fn trampoline<A1: ThreadArg>(
        entry_point: usize,
        arg0: usize,
        arg1: usize,
    ) {
        let entry_point = core::ptr::with_exposed_provenance::<()>(entry_point);
        // SAFETY: This function is only ever passed to the
        // architecture-specific call to `initialize_frame` below. It is
        // never called directly. In `initialize_frame`, the first argument
        // is `entry_point as usize`. `entry_point` is a `fn(usize)`. Thus,
        // this transmute preserves validity, and the preceding
        // `with_exposed_provenance` ensures that the resulting `fn(usize)`
        // has valid provenance for its referent.
        let entry_point: fn(K, A1) = unsafe { core::mem::transmute(entry_point) };
        let kernel = unsafe { K::from_usize(arg0) };
        let arg1 = unsafe { A1::from_usize(arg1) };
        entry_point(kernel, arg1);
    }

    pub fn re_initialize_kernel_thread<A: ThreadArg>(
        &mut self,
        kernel: K,
        entry_point: fn(K, A),
        arg: A,
    ) {
        kernel
            .get_scheduler()
            .lock(kernel)
            .thread_reinitialize_kernel(kernel, self, entry_point, arg)
    }

    pub fn initialize_kernel_thread<A: ThreadArg>(
        &mut self,
        kernel: K,
        entry_point: fn(K, A),
        arg: A,
    ) {
        kernel
            .get_scheduler()
            .lock(kernel)
            .thread_initialize_kernel(kernel, self, entry_point, arg)
    }

    pub(super) fn set_process(&mut self, process: ProcessHandle<K>) {
        self.process = Some(process);
    }

    #[cfg(feature = "user_space")]
    /// # Safety
    /// It is up to the caller to ensure that *process is valid.
    /// Initialize the mutable parts of the non privileged thread, must be
    /// called once per thread prior to starting it
    pub unsafe fn initialize_non_priv_thread(
        &mut self,
        kernel: K,
        process: ProcessHandle<K>,
        initial_pc: usize,
        initial_sp: usize,
        args: (usize, usize, usize),
    ) -> Result<()> {
        unsafe {
            kernel
                .get_scheduler()
                .lock(kernel)
                .thread_initialize_non_priv(kernel, self, process, initial_pc, initial_sp, args)
        }
    }

    #[allow(dead_code)]
    pub fn dump(&self) {
        info!(
            "  - Thread '{}' ({:#010x}) state: {}",
            self.name as &str,
            self.id() as usize,
            to_string(self.state) as &str
        );
    }

    /// Returns a reference to the thread's parent process.
    pub fn process(&self) -> ProcessHandle<K> {
        // SAFETY: The returned process references is bound to an immutable
        // borrow of the thread the `process` pointer can not change.
        let Some(process_handle) = self.process.as_ref() else {
            pw_assert::panic!("Thread does not have a process");
        };
        process_handle.clone()
    }

    /// Return a reference counted `ThreadHandle` for this thread.
    pub(super) fn get_ref(&self, kernel: K) -> ThreadHandle<K> {
        self.ref_count.fetch_add(1, Ordering::Acquire);
        ThreadHandle {
            thread: NonNull::from_ref(self),
            kernel,
        }
    }

    /// A simple ID for debugging purposes
    ///
    /// Currently this is a pointer to the architecture specific thread state
    /// allowing it to match debugging output from the architecture implementation.
    ///
    /// # Safety
    ///
    /// The returned value should not be relied upon as being a valid pointer.
    /// Even in the current implementation, `id` does not expose the pointer's
    /// provenance.
    #[must_use]
    pub fn id(&self) -> usize {
        // Note: any changes to this also need to be reflected in
        // `const_id_from_thread_address`.
        self.arch_thread_state.get().addr()
    }

    /// Computes the ID of a thread from its address.
    ///
    /// # Safety
    ///
    /// This exists to allow thread annotations to be generated at compile time
    /// for static threads that are initialized at runtime. This should not be
    /// called in any other circumstance.
    #[must_use]
    pub const unsafe fn const_id_from_thread_address(addr: *const ()) -> *const () {
        unsafe { addr.byte_add(core::mem::offset_of!(Self, arch_thread_state)) }
    }

    // An ID that can not be assigned to any thread in the system.
    #[must_use]
    pub const fn null_id() -> usize {
        // `core::ptr::null::<Self>() as usize` can not be evaluated at const time
        // and a null pointer is defined to be at address 0 (see
        // https://doc.rust-lang.org/beta/core/ptr/fn.null.html).
        0usize
    }
}

pub use arg::ThreadArg;
mod arg {
    pub trait ThreadArg {
        fn into_usize(self) -> usize;

        /// # Safety
        ///
        /// `u` must have previously been returned by [`x.into_usize()`]. The
        /// returned `Self` is guaranteed to be equal to `x`.
        ///
        /// [`x.into_usize()`]: ThreadArg::into_usize
        unsafe fn from_usize(u: usize) -> Self;
    }

    impl ThreadArg for usize {
        fn into_usize(self) -> usize {
            self
        }

        unsafe fn from_usize(u: usize) -> Self {
            u
        }
    }

    impl<T> ThreadArg for *const T {
        fn into_usize(self) -> usize {
            self.expose_provenance()
        }

        unsafe fn from_usize(u: usize) -> Self {
            core::ptr::with_exposed_provenance(u)
        }
    }

    impl<T> ThreadArg for *mut T {
        fn into_usize(self) -> usize {
            self.expose_provenance()
        }

        unsafe fn from_usize(u: usize) -> Self {
            core::ptr::with_exposed_provenance_mut(u)
        }
    }

    #[allow(clippy::needless_lifetimes)]
    impl<'a, T> ThreadArg for &'a T {
        fn into_usize(self) -> usize {
            let s: *const T = self;
            s.into_usize()
        }

        unsafe fn from_usize(u: usize) -> Self {
            // SAFETY: The caller promises that `u` was previously returned by
            // `into_usize`, which is implemented as `<*const T as
            // ThreadArg>::into_usize`. Thus, `u` was previously returned by
            // `<*const T as ThreadArg>::into_usize`.
            let ptr = unsafe { <*const T>::from_usize(u) };
            // SAFETY: By the preceding safety comment, `ptr` is equal to `self
            // as *const T` where `self: &'a T`, including provenance.
            unsafe { &*ptr }
        }
    }

    #[allow(clippy::needless_lifetimes)]
    impl<'a, T> ThreadArg for &'a mut T {
        fn into_usize(self) -> usize {
            let s: *mut T = self;
            s.into_usize()
        }

        unsafe fn from_usize(u: usize) -> Self {
            // SAFETY: The caller promises that `u` was previously returned by
            // `into_usize`, which is implemented as `<*mut T as
            // ThreadArg>::into_usize`. Thus, `u` was previously returned by
            // `<*mut T as ThreadArg>::into_usize`.
            let ptr = unsafe { <*mut T>::from_usize(u) };
            // SAFETY: By the preceding safety comment, `ptr` is equal to `self
            // as *mut T` where `self: &'a mut T`, including provenance. Since
            // `into_usize` consumes `self` by value, no other references to the
            // same referent exist, and so mutable aliasing is satisfied.
            unsafe { &mut *ptr }
        }
    }

    #[macro_export]
    macro_rules! impl_thread_arg_for_default_zst {
        ($t:ty) => {
            const _: () = assert!(size_of::<$t>() == 0);
            impl $crate::scheduler::ThreadArg for $t {
                fn into_usize(self) -> usize {
                    0
                }

                unsafe fn from_usize(_u: usize) -> Self {
                    // SAFETY: We asserted above that `size_of::<$t>() == 0`, so
                    // there is only one value of `Self`. Thus, this
                    // implementation of `from_usize` returns the same value
                    // passed to any call to `into_usize`, as there is only one
                    // possible such value.
                    <$t as Default>::default()
                }
            }
        };
    }
}

/// Add debug annotations for a static thread
///
/// This macro is called by `init_non_priv_thread` and should not be called
/// directly.
#[macro_export]
macro_rules! annotate_thread_from_address {
    ($name:expr, $arch:ty, $thread_address:expr, $parent_id: expr) => {{
        #[repr(C, packed(1))]
        struct ThreadAnnotation {
            name: &'static str,
            id: *const (),
            parent_id: *const (),
        }
        unsafe impl Sync for ThreadAnnotation {};

        #[unsafe(link_section = ".pw_kernel.annotations.thread")]
        #[used]
        static _THREAD_ANNOTATION: ThreadAnnotation = ThreadAnnotation {
            name: $name,
            id: unsafe {
                $crate::Thread::<$arch>::const_id_from_thread_address($thread_address as *const ())
            },
            parent_id: $parent_id as *const (),
        };
    }};
}

/// Add debug annotations for a stack.
///
/// This macro is called by `init_process` and should not be called directly.
#[macro_export]
macro_rules! annotate_stack {
    ($name:expr, $addr:expr, $size:expr) => {{
        #[repr(C, packed(1))]
        struct StackAnnotation {
            name: &'static str,
            addr: *const (),
            size: usize,
        }
        unsafe impl Sync for StackAnnotation {};

        #[unsafe(link_section = ".pw_kernel.annotations.stack")]
        #[used]
        static _STACK_ANNOTATION: StackAnnotation = StackAnnotation {
            name: $name,
            addr: $addr as *const (),
            size: $size,
        };
    }};
}

/// Declare a static stack.
///
/// This macro is called by `init_process` and should not be called directly.
#[macro_export]
macro_rules! static_stack {
    ($thread_name:expr, $stack_size:expr) => {{
        use core::cell::UnsafeCell;
        use core::mem::MaybeUninit;

        use kernel::{StackStorage, StackStorageExt};

        static mut __STATIC: UnsafeCell<MaybeUninit<StackStorage<$stack_size>>> =
            UnsafeCell::new(core::mem::MaybeUninit::uninit());
        $crate::annotate_stack!($thread_name, unsafe { __STATIC.get() }, $stack_size);

        unsafe { (*__STATIC.get()).write(StackStorageExt::ZEROED) }
    }};
}

/// Constructs a new [`Thread`] in global static storage.
///
/// # Safety
///
/// Each invocation of `init_thread!` must be executed at most once at run time.
// TODO: davidroth - Add const assertions to ensure stack sizes aren't too
// small, once the sizing analysis has been done to understand what a reasonable
// minimum is.
#[macro_export]
macro_rules! init_thread {
    ($name:literal, $priority:expr, $entry:expr, $stack_size:expr $(,)?) => {{
        use $crate::__private::foreign_box::ForeignBox;
        use $crate::scheduler::{Stack, StackStorage, StackStorageExt, Thread};
        use $crate::static_mut_ref;

        /// SAFETY: This must be executed at most once at run time.
        unsafe fn __init_thread() -> ForeignBox<Thread<arch::Arch>> {
            info!("Initializing thread '{}'", $name as &'static str);
            let stack = $crate::static_stack!($name, $stack_size);
            // SAFETY: The caller promises that this function will be
            // executed at most once.
            let stack = Stack::from_slice(&stack.stack);

            let thread = unsafe { static_mut_ref!(Thread<arch::Arch> = Thread::new($name, $priority, stack)) };
            let mut thread = ForeignBox::from(thread);

            thread.initialize_kernel_thread(
                $crate::arch::Arch,
                $entry,
                0
            );

            thread
        }

        __init_thread()
    }};
}

/// Constructs a new [`Thread`] in global static storage and registers it.
///
/// # Safety
///
/// Each invocation of `declare_non_priv_thread!` must be executed at most once at
/// run time.
#[cfg(feature = "user_space")]
#[macro_export]
macro_rules! declare_non_priv_thread {
    ($name:literal, $priority:expr, $process_id:expr, $kernel_stack_size:expr  $(,)?) => {{
        use core::mem::MaybeUninit;

        use $crate::__private::foreign_box::ForeignBox;
        use $crate::scheduler::{Stack, StackStorage, StackStorageExt, Thread};

        /// SAFETY: This must be executed at most once at run time.
        unsafe fn __init_non_priv_thread() -> (ForeignBox<Thread<arch::Arch>>, Stack) {
            use pw_log::info;
            info!(
                "Allocating non-privileged thread '{}'",
                $name as &'static str
            );
            // SAFETY: The caller promises that this function will be executed
            // at most once.
            use $crate::__private::foreign_box::StaticStorage;
            static mut __STATIC: StaticStorage<Thread<arch::Arch>> = StaticStorage::new();
            $crate::annotate_thread_from_address!(
                $name,
                arch::Arch,
                unsafe { __STATIC.address() },
                $process_id
            );
            let stack = $crate::static_stack!($name, $kernel_stack_size);
            // SAFETY: The caller promises that this function will be
            // executed at most once.
            let stack = Stack::from_slice(&stack.stack);

            let thread = __STATIC.init(Thread::new($name, $priority, stack));
            let mut thread = ForeignBox::from(thread);

            (thread, stack)
        }

        __init_non_priv_thread()
    }};
}

/// Initializes a thread in the given storage.
pub fn init_thread_in<K: Kernel, T: ThreadArg, const STACK_SIZE: usize>(
    kernel: K,
    thread: &'static mut Thread<K>,
    stack: &'static mut StackStorage<STACK_SIZE>,
    name: &'static str,
    priority: Priority,
    entry: fn(K, T),
    arg: T,
) -> ForeignBox<Thread<K>> {
    let stack_obj = Stack::from_slice(&stack.stack);
    *thread = Thread::new(name, priority, stack_obj);
    let mut thread = ForeignBox::from(thread);
    thread.initialize_kernel_thread(kernel, entry, arg);
    thread
}
