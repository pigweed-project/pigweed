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

use kernel::interrupt_controller::InterruptController;
use kernel::scheduler::thread::{Stack, ThreadState};
use kernel::scheduler::{PreemptDisableGuard, SchedulerState};
use kernel::sync::spinlock::SpinLockGuard;
use kernel::{Arch, Kernel, KernelState};
use memory_config::MemoryRegionType;
use pw_log::info;
use pw_status::{Error, Result};

mod spinlock;

#[derive(Copy, Clone, Default)]
pub struct HostArch;

kernel::impl_thread_arg_for_default_zst!(HostArch);

pub struct ArchThreadState;

impl Arch for HostArch {
    type ThreadState = ArchThreadState;
    type BareSpinLock = spinlock::BareSpinLock;
    type Clock = Clock;
    type AtomicBool = core::sync::atomic::AtomicBool;
    type AtomicUsize = core::sync::atomic::AtomicUsize;
    type SyscallArgs<'a> = HostSyscallArgs;
    type InterruptController = HostInterruptController;

    unsafe fn context_switch(
        self,
        _sched_state: SpinLockGuard<'_, Self, SchedulerState<Self>>,
        _old_thread_state: *mut ArchThreadState,
        _new_thread_state: *mut ArchThreadState,
    ) -> (SpinLockGuard<'_, Self, SchedulerState<Self>>, bool) {
        pw_assert::panic!("Unimplemented: context_switch");
    }

    fn thread_local_state(self) -> &'static kernel::scheduler::ThreadLocalState<Self> {
        pw_assert::panic!("Unimplemented: thread_local_state");
    }

    fn now(self) -> time::Instant<Clock> {
        use time::Clock as _;
        Clock::now()
    }

    fn early_init(self) {
        info!("Host architecture early initialization");
    }
    fn init(self) {
        info!("Host architecture initialization");
    }
}

impl Kernel for HostArch {
    fn get_state(self) -> &'static KernelState<HostArch> {
        static STATE: KernelState<HostArch> =
            KernelState::new(kernel::ArchState::new(HostInterruptController::new()));
        &STATE
    }
}

impl ThreadState for ArchThreadState {
    const NEW: Self = Self;
    type MemoryConfig = MemoryConfig;

    unsafe fn initialize_kernel_frame(
        &mut self,
        _kernel_stack: Stack,
        _memory_config: *const MemoryConfig,
        _initial_function: extern "C" fn(usize, usize, usize),
        _args: (usize, usize, usize),
    ) {
        pw_assert::panic!("Unimplemented: initialize_kernel_frame");
    }

    #[cfg(feature = "user_space")]
    unsafe fn initialize_user_frame(
        &mut self,
        _kernel_stack: Stack,
        _memory_config: *const MemoryConfig,
        _initial_sp: usize,
        _initial_pc: usize,
        _args: (usize, usize, usize),
    ) -> Result<()> {
        pw_assert::panic!("Unimplemented: initialize_user_frame");
    }
}

pub struct Clock;

impl time::Clock for Clock {
    const TICKS_PER_SEC: u64 = 1_000_000;

    fn now() -> time::Instant<Self> {
        time::Instant::from_ticks(0)
    }
}

pub struct MemoryConfig;

impl memory_config::MemoryConfig for MemoryConfig {
    const KERNEL_THREAD_MEMORY_CONFIG: Self = Self;

    fn range_has_access(
        &self,
        _access_type: MemoryRegionType,
        _start_addr: usize,
        _end_addr: usize,
    ) -> bool {
        false
    }
}

pub struct HostSyscallArgs;
impl<'a> kernel::syscall::SyscallArgs<'a> for HostSyscallArgs {
    fn next_usize(&mut self) -> Result<usize> {
        Err(Error::Unimplemented)
    }

    fn next_u64(&mut self) -> Result<u64> {
        Err(Error::Unimplemented)
    }
}

pub struct HostInterruptController {}

impl HostInterruptController {
    #[must_use]
    pub const fn new() -> Self {
        Self {}
    }
}

impl InterruptController for HostInterruptController {
    fn early_init(&self) {
        pw_assert::panic!("Unimplemented: early_init");
    }

    fn enable_interrupt(_irq: u32) {
        pw_assert::panic!("Unimplemented: enable_interrupt");
    }

    fn disable_interrupt(_irq: u32) {
        pw_assert::panic!("Unimplemented: disable_interrupt");
    }

    fn userspace_interrupt_ack(_irq: u32) {
        pw_assert::panic!("Unimplemented: userspace_interrupt_ack");
    }

    fn userspace_interrupt_handler_enter<K: Kernel>(
        _kernel: K,
        _irq: u32,
    ) -> PreemptDisableGuard<K> {
        pw_assert::panic!("Unimplemented: userspace_interrupt_handler_enter");
    }

    fn userspace_interrupt_handler_exit<K: Kernel>(
        _kernel: K,
        _irq: u32,
        _preempt_guard: PreemptDisableGuard<K>,
    ) {
        pw_assert::panic!("Unimplemented: userspace_interrupt_handler_exit");
    }

    fn kernel_interrupt_handler_enter<K: Kernel>(_kernel: K, _irq: u32) -> PreemptDisableGuard<K> {
        pw_assert::panic!("Unimplemented: kernel_interrupt_handler_enter");
    }

    fn kernel_interrupt_handler_exit<K: Kernel>(
        _kernel: K,
        _irq: u32,
        _preempt_guard: PreemptDisableGuard<K>,
    ) {
        pw_assert::panic!("Unimplemented: kernel_interrupt_handler_exit");
    }

    fn enable_interrupts() {
        pw_assert::panic!("Unimplemented: enable_interrupts");
    }

    fn disable_interrupts() {
        pw_assert::panic!("Unimplemented: disable_interrupts");
    }

    fn interrupts_enabled() -> bool {
        pw_assert::panic!("Unimplemented: interrupts_enabled");
    }

    fn trigger_interrupt(_irq: u32) {
        pw_assert::panic!("Unimplemented: trigger_interrupt");
    }
}
