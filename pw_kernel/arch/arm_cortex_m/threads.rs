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

use core::arch::asm;
use core::mem::{self, MaybeUninit};
use core::ptr::NonNull;

use cortex_m::peripheral::{SCB, *};
use kernel::interrupt_controller::InterruptController;
use kernel::scheduler::thread::Stack;
use kernel::scheduler::{self, SchedulerState, ThreadLocalState};
use kernel::sync::spinlock::SpinLockGuard;
use kernel::{Arch, Kernel};
use log_if::debug_if;
use memory_config::{MemoryConfig as _, MemoryRegionType};
use pw_cast::CastInto as _;
use pw_log::info;
use pw_status::{Error, Result};

use crate::exceptions::{
    ExcReturn, ExcReturnFrameType, ExcReturnMode, ExcReturnRegisterStacking, ExcReturnStack,
    ExceptionFrame, KernelExceptionFrame, RetPsrVal, exception,
};
use crate::protection::MemoryConfig;
use crate::regs::Regs;
use crate::regs::msr::{ControlVal, Spsel};
use crate::spinlock::BareSpinLock;
use crate::{in_interrupt_handler, nvic};

const LOG_THREAD_CREATE: bool = false;
const LOG_CONTEXT_SWITCH: bool = false;

const STACK_ALIGNMENT: usize = 8;

// Remember the thread that the cpu is currently running off of.
// NOTE this may lag behind the Scheduler's notion of current_thread due to the way
// pendsv may have a queuing effect in particular contexts, notably when preempting
// from a higher priority interrupt.
// Cleared inside the pendsv handler when the context switch actually happens.
static mut ACTIVE_THREAD: *mut ArchThreadState = core::ptr::null_mut();
unsafe fn get_active_thread() -> *mut ArchThreadState {
    unsafe { ACTIVE_THREAD }
}
unsafe fn set_active_thread(t: *mut ArchThreadState) {
    unsafe {
        ACTIVE_THREAD = t;
    }
}

static BOOT_THREAD_LOCAL_STATE: ThreadLocalState<crate::Arch> = ThreadLocalState::new();
static mut THREAD_LOCAL_STATE: NonNull<ThreadLocalState<crate::Arch>> =
    NonNull::from_ref(&BOOT_THREAD_LOCAL_STATE);

pub struct ArchThreadState {
    frame: *mut KernelExceptionFrame,
    memory_config: *const MemoryConfig,
    local: ThreadLocalState<crate::Arch>,
}

impl ArchThreadState {
    #[inline(never)]
    #[expect(clippy::too_many_arguments)]
    fn initialize_frame(
        &mut self,
        user_frame: *mut ExceptionFrame,
        kernel_frame: *mut KernelExceptionFrame,
        control: ControlVal,
        psp: u32,
        return_address: ExcReturn,
        initial_pc: usize,
        (r0, r1, r2, r3): (usize, usize, usize, usize),
    ) {
        // Clear the stack and set up the exception frame such that it would
        // return to the function passed in with arg0 and arg1 passed in the
        // first two argument slots.
        unsafe {
            (*user_frame) = mem::zeroed();
            (*user_frame).r0 = r0.cast_into();
            (*user_frame).r1 = r1.cast_into();
            (*user_frame).r2 = r2.cast_into();
            (*user_frame).r3 = r3.cast_into();
            (*user_frame).pc = initial_pc.cast_into();
            (*user_frame).psr = RetPsrVal(0).with_t(true);
            (*kernel_frame) = mem::zeroed();
            (*kernel_frame).psp = psp;
            (*kernel_frame).control = control;
            (*kernel_frame).return_address = return_address.bits().cast_into();
        }
        self.frame = kernel_frame;
    }
}

// Demonstration of zero over head register abstraction.
#[inline(never)]
fn get_num_mpu_regions(mpu: &mut crate::regs::Mpu) -> u8 {
    mpu._type.read().dregion()
}

impl Arch for crate::Arch {
    type ThreadState = ArchThreadState;
    type BareSpinLock = BareSpinLock;
    type Clock = super::timer::Clock;
    type AtomicBool = core::sync::atomic::AtomicBool;
    type AtomicUsize = core::sync::atomic::AtomicUsize;
    type SyscallArgs<'a> = crate::syscall::CortexMSyscallArgs<'a>;
    type InterruptController = nvic::Nvic;

    unsafe fn context_switch<'a>(
        self,
        mut sched_state: SpinLockGuard<'a, Self, SchedulerState<Self>>,
        old_thread_state: *mut ArchThreadState,
        new_thread_state: *mut ArchThreadState,
    ) -> (SpinLockGuard<'a, Self, SchedulerState<Self>>, bool) {
        pw_assert::assert!(unsafe {
            new_thread_state == sched_state.get_current_arch_thread_state()
        });
        // TODO - konkers: Allow $expr to be tokenized.

        // Remember active_thread only if it wasn't already set and trigger
        // a pendsv only the first time
        unsafe {
            if get_active_thread().is_null() {
                set_active_thread(old_thread_state);

                // Queue a PendSV
                SCB::set_pendsv();
            }
        }

        // Slightly different path based on if we're already inside an interrupt handler or not.
        if !in_interrupt_handler() {
            // A trick to reenable interrupts temporarily. Drop the passed in
            // spinlock guard temporarily enough to reenable interrupts and then
            // acquire immediately afterwards.

            // TODO: make sure this always drops interrupts, may need to force a cpsid here.
            drop(sched_state);
            pw_assert::debug_assert!(
                <crate::Arch as kernel::Arch>::InterruptController::interrupts_enabled()
            );

            // PendSV should fire and a context switch will happen.

            // ==== TEMPORAL ANOMALY HERE ====
            //
            // The next line of code is only executed in this context after the
            // old thread is context switched back to.

            sched_state = crate::Arch::get_scheduler(crate::Arch).lock(crate::Arch);
            (sched_state, true)
        } else {
            // in interrupt context the pendsv should have already triggered it
            pw_assert::assert!(SCB::is_pendsv_pending());
            (sched_state, false)
        }
    }

    fn thread_local_state(self) -> &'static ThreadLocalState<Self> {
        unsafe {
            #[allow(static_mut_refs)]
            THREAD_LOCAL_STATE.as_ref()
        }
    }

    fn now(self) -> time::Instant<super::timer::Clock> {
        use time::Clock as _;
        super::timer::Clock::now()
    }

    fn idle(self) {
        cortex_m::asm::wfi();
    }

    fn early_init(self) {
        info!("Cortex-M early initialization");
        // TODO: set up the cpu here:
        //  --interrupt vector table--
        //  irq priority levels
        //  clear pending interrupts
        //  FPU initial state
        //  enable cache (if present)
        //  enable cycle counter?
        let p: Peripherals;
        // TODO: davidroth - use expect wrapper when available.
        if let Some(val) = Peripherals::take() {
            p = val;
        } else {
            pw_assert::panic!("Peripherals already taken")
        }

        let cpu_id = Regs::get().scb.cpu_id.read();
        info!(
            "CPUID: revision={:#x}, part_number={:#x}, architecture={:#x}, variant={:#x}, implementor={:#x}",
            cpu_id.revision() as u32,
            cpu_id.part_no() as u32,
            cpu_id.architecture() as u32,
            cpu_id.variant() as u32,
            cpu_id.implementer() as u32
        );

        let mut r = crate::regs::Regs::get();
        info!("MPU regions: {}", get_num_mpu_regions(&mut r.mpu) as u8);

        unsafe {
            // Set the VTOR (assumes it exists)
            unsafe extern "C" {
                fn pw_boot_vector_table_addr();
            }
            let vector_table = pw_boot_vector_table_addr as *const ();
            p.SCB
                .vtor
                .write(vector_table.expose_provenance().cast_into());

            // Only the high two bits or the priority are guaranteed to be
            // implemented.  Values below are chosen accordingly.
            //
            // Note: Higher values have lower priority
            let mut scb = p.SCB;

            // Set SVCall (system calls) to the lowest priority.
            scb.set_priority(scb::SystemHandler::SVCall, 0b1111_1111);

            // Set PendSV (used by context switching) to just above SVCall so
            // that system calls can context switch.
            scb.set_priority(scb::SystemHandler::PendSV, 0b1011_1111);

            // Set IRQs to a priority above SVCall and PendSV so that they
            // can preempt them.
            scb.set_priority(scb::SystemHandler::SysTick, 0b0111_1111);

            // External IRQ priorities are set by the interrupt controller.

            scb.enable(scb::Exception::MemoryManagement);
            // TODO: configure BASEPRI, FAULTMASK
        } // unsafe

        // Set up PMP attr registers so that all PMP configs can reference them.
        #[cfg(feature = "user_space")]
        crate::protection::init();

        crate::timer::systick_early_init();

        // TEST: Intentionally trigger a hard fault to make sure the VTOR is working.
        // use core::arch::asm;
        // unsafe {
        //     asm!("bkpt");
        // }

        // TEST: Intentionally trigger a pendsv
        // use cortex_m::interrupt;
        // SCB::set_pendsv();
        // unsafe {
        //     interrupt::enable();
        // }
    }

    fn init(self) {
        info!("Cortex-M initialization");
        crate::timer::systick_init();
    }

    fn panic() -> ! {
        unsafe {
            asm!("bkpt");
        }
        #[expect(clippy::empty_loop)]
        loop {}
    }
}

impl kernel::scheduler::thread::ThreadState for ArchThreadState {
    type MemoryConfig = crate::protection::MemoryConfig;

    const NEW: Self = Self {
        frame: core::ptr::null_mut(),
        memory_config: core::ptr::null(),
        local: ThreadLocalState::new(),
    };

    unsafe fn initialize_kernel_frame(
        &mut self,
        kernel_stack: Stack,
        memory_config: *const MemoryConfig,
        initial_function: extern "C" fn(usize, usize, usize),
        args: (usize, usize, usize),
    ) {
        self.memory_config = memory_config;
        let user_frame: *mut ExceptionFrame =
            Stack::aligned_stack_allocation_mut(unsafe { kernel_stack.end_mut() }, STACK_ALIGNMENT);

        let kernel_frame: *mut KernelExceptionFrame = Stack::aligned_stack_allocation_mut(
            user_frame.cast(),
            // For kernel threads, kernel_frame needs to come immediately after
            // the user stack regardless of alignment because that is what the
            // exception wrapper assembly expects.
            size_of::<usize>(),
        );

        // TODO: This is unsound: `user_frame` and `kernel_frame` need to be
        // `*mut` to preserve the ability to mutate their referents in the Rust
        // memory model.
        self.initialize_frame(
            user_frame,
            kernel_frame.cast::<KernelExceptionFrame>(),
            ControlVal::default()
                .with_npriv(false)
                .with_spsel(Spsel::Main),
            0x0,
            ExcReturn::new(
                ExcReturnStack::MainSecure,
                ExcReturnRegisterStacking::Default,
                ExcReturnFrameType::Standard,
                ExcReturnMode::ThreadSecure,
            ),
            trampoline as *const () as usize,
            (initial_function as usize, args.0, args.1, args.2),
        );
    }

    #[cfg(feature = "user_space")]
    unsafe fn initialize_user_frame(
        &mut self,
        kernel_stack: Stack,
        memory_config: *const MemoryConfig,
        initial_sp: usize,
        initial_pc: usize,
        args: (usize, usize, usize),
    ) -> Result<()> {
        self.memory_config = memory_config;

        // In order to use an exception return to switch to user space, an
        // exception stack frame needs to be written to the user stack.  This
        // is first aligned then checked to make sure the process, through the
        // associated `MemoryConfig`, has access to the underlying memory.
        let user_frame: *mut ExceptionFrame = Stack::aligned_stack_allocation_mut(
            core::ptr::with_exposed_provenance_mut::<MaybeUninit<u8>>(initial_sp),
            STACK_ALIGNMENT,
        );

        if !unsafe { &*memory_config }.has_access(MemoryRegionType::ReadWriteData, user_frame) {
            return Err(Error::PermissionDenied);
        }

        let kernel_frame: *mut KernelExceptionFrame =
            Stack::aligned_stack_allocation_mut(unsafe { kernel_stack.end_mut() }, STACK_ALIGNMENT);

        self.initialize_frame(
            user_frame,
            kernel_frame,
            ControlVal::default()
                .with_npriv(true)
                .with_spsel(Spsel::Process),
            user_frame.expose_provenance().cast_into(),
            ExcReturn::new(
                ExcReturnStack::ThreadSecure,
                ExcReturnRegisterStacking::Default,
                ExcReturnFrameType::Standard,
                ExcReturnMode::ThreadSecure,
            ),
            initial_pc,
            (args.0, args.1, args.2, 0),
        );

        Ok(())
    }
}

extern "C" fn trampoline(
    initial_function: extern "C" fn(usize, usize, usize),
    arg0: usize,
    arg1: usize,
    arg2: usize,
) {
    debug_if!(
        LOG_THREAD_CREATE,
        "arm_cortex_m trampoline: initial function {:#x} arg0 {:#x} arg1 {:#x} arg2 {:#}",
        initial_function as usize,
        arg0 as usize,
        arg1 as usize,
        arg2 as usize,
    );

    pw_assert::assert!(<crate::Arch as kernel::Arch>::InterruptController::interrupts_enabled());

    // Call the actual initial function of the thread.
    initial_function(arg0, arg1, arg2);

    // Get a pointer to the current thread and call exit.
    // Note: must let the scope of the lock guard close,
    // since exit_thread() does not return.
    scheduler::exit_thread(crate::Arch);

    // Does not reach.
}

// Called by the pendsv handler, returns the new stack to switch to after
// performing some housekeeping.
#[exception(exception = "PendSV", disable_interrupts)]
#[unsafe(no_mangle)]
extern "C" fn pendsv_swap_sp(frame: *mut KernelExceptionFrame) -> *mut KernelExceptionFrame {
    // TODO:
    // save incoming frame to active_thread.archstate
    // clear active_thread
    // return a pointer to the current_thread's sp

    // Clear any local reservations on atomic addresses, in case we were context switched
    // in the middle of an atomic sequence.
    unsafe { asm!("clrex") };

    pw_assert::assert!(in_interrupt_handler());
    pw_assert::assert!(!<crate::Arch as kernel::Arch>::InterruptController::interrupts_enabled());

    // Save the incoming frame to the current active thread's arch state, that will function
    // as the context switch frame for when it is returned to later. Clear active thread
    // afterwards.
    let active_thread = unsafe { get_active_thread() };
    log_if::info_if!(
        LOG_CONTEXT_SWITCH,
        "PendSV: active thread {:#010x}",
        active_thread as usize
    );

    pw_assert::assert!(!active_thread.is_null());

    unsafe {
        (*active_thread).frame = frame;

        set_active_thread(core::ptr::null_mut());
    }

    // Return the arch frame for the current thread
    let mut sched_state = crate::Arch.get_scheduler().lock(crate::Arch);
    let new_thread = unsafe { sched_state.get_current_arch_thread_state() };
    log_if::info_if!(
        LOG_CONTEXT_SWITCH,
        "Context switch to thread '{}' ({:#010x})",
        sched_state.current_thread_name() as &str,
        sched_state.current_thread_id() as usize
    );

    // Memory context switch overhead is avoided for threads in the same
    // memory config space.
    #[cfg(feature = "user_space")]
    unsafe {
        if (*new_thread).memory_config != (*active_thread).memory_config {
            (*(*new_thread).memory_config).write();
        }
    }
    drop(sched_state);

    unsafe { THREAD_LOCAL_STATE = NonNull::from_ref(&(*new_thread).local) }

    unsafe { (*new_thread).frame }
}
