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

use core::arch::{asm, naked_asm};
use core::mem;
use core::ptr::NonNull;

use kernel::Arch;
use kernel::interrupt_controller::InterruptController;
use kernel::scheduler::thread::Stack;
use kernel::scheduler::{self, SchedulerState, ThreadLocalState};
use kernel::sync::spinlock::SpinLockGuard;
use log_if::debug_if;
use pw_status::Result;

use crate::protection::MemoryConfig;
use crate::regs::{MStatusVal, PrivilegeLevel};
use crate::spinlock::BareSpinLock;

const LOG_CONTEXT_SWITCH: bool = false;
const LOG_THREAD_CREATE: bool = false;

static BOOT_THREAD_LOCAL_STATE: ThreadLocalState<crate::Arch> = ThreadLocalState::new();
static mut THREAD_LOCAL_STATE: NonNull<ThreadLocalState<crate::Arch>> =
    NonNull::from_ref(&BOOT_THREAD_LOCAL_STATE);
#[repr(C)]
struct ContextSwitchFrame {
    ra: usize,
    s0: usize,
    s1: usize,
    s2: usize,
    s3: usize,
    s4: usize,
    s5: usize,
    s6: usize,
    s7: usize,
    s8: usize,
    s9: usize,
    s10: usize,
    s11: usize,
}

pub struct ArchThreadState {
    frame: *mut ContextSwitchFrame,
    #[cfg(feature = "user_space")]
    pub(crate) memory_config: *const MemoryConfig,
    local: ThreadLocalState<crate::Arch>,
}

impl ArchThreadState {
    #[inline(never)]
    fn initialize_frame(
        &mut self,
        kernel_stack: Stack,
        trampoline: extern "C" fn(),
        initial_mstatus: MStatusVal,
        initial_sp: usize,
        (s0, s1, s2, s3): (usize, usize, usize, usize),
    ) {
        let frame: *mut ContextSwitchFrame =
            Stack::aligned_stack_allocation_mut(unsafe { kernel_stack.end_mut() }, 8);

        unsafe {
            // Clear the stack and set up the exception frame such that it would
            // return to the function passed in with arg0 and arg1 passed in the
            // first two argument slots.
            (*frame) = mem::zeroed();
            (*frame).ra = trampoline as usize;
            // The `s` registers are used here instead of the ABI prescribed `a`
            // registers because the `a` registers are in the exception frame,
            // not the context switch frame.
            (*frame).s0 = s0;
            (*frame).s1 = s1;
            (*frame).s2 = s2;
            (*frame).s3 = s3;
            (*frame).s5 = initial_sp;
            (*frame).s6 = initial_mstatus.0;
            if cfg!(feature = "exceptions_reload_pmp") {
                // If we need to reload the PMP to for exceptions, this also means we
                // do not perform the PMP load in the context switch function.  We
                // defer it instead to the trampoline function.  Stash the pointers
                // we'll need to make the call in the trampoline.
                (*frame).s7 = self.memory_config as usize;
                (*frame).s8 = memory_config_write as *const () as usize;
            } else {
                (*frame).s7 = 0;
                (*frame).s8 = 0;
            }
        }

        self.frame = frame
    }
}

impl Arch for super::Arch {
    type ThreadState = ArchThreadState;
    type BareSpinLock = BareSpinLock;
    type Clock = super::timer::Clock;
    #[cfg(not(feature = "disable_interrupts_atomic"))]
    type AtomicBool = core::sync::atomic::AtomicBool;
    #[cfg(not(feature = "disable_interrupts_atomic"))]
    type AtomicUsize = core::sync::atomic::AtomicUsize;
    #[cfg(feature = "disable_interrupts_atomic")]
    type AtomicBool = crate::disable_interrupts_atomic::AtomicBool;
    #[cfg(feature = "disable_interrupts_atomic")]
    type AtomicUsize = crate::disable_interrupts_atomic::AtomicUsize;
    type SyscallArgs<'a> = crate::exceptions::RiscVSyscallArgs<'a>;
    type InterruptController = crate::InterruptController;

    #[inline(never)]
    unsafe fn context_switch<'a>(
        self,
        sched_state: SpinLockGuard<'a, Self, SchedulerState<Self>>,
        old_thread_state: *mut ArchThreadState,
        new_thread_state: *mut ArchThreadState,
    ) -> (SpinLockGuard<'a, Self, SchedulerState<Self>>, bool) {
        debug_if!(
            LOG_CONTEXT_SWITCH,
            "context switch from frame {:#08x} to frame {:#08x}",
            unsafe { (*old_thread_state).frame } as usize,
            unsafe { (*new_thread_state).frame } as usize,
        );

        // Memory config is swapped before the context switch instead of after.
        // This avoids needing a special case in the user mode thread init to
        // initialize the config.
        //
        // Memory context switch overhead is avoided for threads in the same
        // memory config space.
        //
        // In the case where the target needs to reload the PMP upon exception
        // handler entry and exit, we skip loading the PMP here and perform the
        // load in the exception handler and the assembly trampoline, deferring
        // the PMP load to very close to the return to userspace.  This keeps
        // kernel memory mappings active until nearly the last moment before
        // returning into userspace.
        #[cfg(all(feature = "user_space", not(feature = "exceptions_reload_pmp")))]
        if unsafe { (*new_thread_state).memory_config }
            != unsafe { (*old_thread_state).memory_config }
        {
            unsafe { (*(*new_thread_state).memory_config).write() };
        }

        unsafe { THREAD_LOCAL_STATE = NonNull::from_ref(&(*new_thread_state).local) }

        // Note: there is a small window of time where the new memory configuration
        // is active (above) and the new thread is active (below).  Since this code
        // always executes in M-Mode, it bypasses the memory config and by the
        // time control is returned to user space, the memory config is correct.
        let old_thread_frame = unsafe { &mut (*old_thread_state).frame };
        let new_thread_frame = unsafe { (*new_thread_state).frame };
        riscv_context_switch(old_thread_frame, new_thread_frame);

        (sched_state, true)
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
        riscv::asm::wfi();
    }

    fn early_init(self) {
        // Make sure interrupts are disabled
        <Self as Arch>::InterruptController::disable_interrupts();

        crate::exceptions::early_init();

        crate::timer::early_init();
    }

    fn init(self) {
        crate::timer::init();
    }

    fn panic() -> ! {
        unsafe {
            asm!("ebreak");
        }
        #[allow(clippy::empty_loop)]
        loop {}
    }
}

impl kernel::scheduler::thread::ThreadState for ArchThreadState {
    type MemoryConfig = crate::protection::MemoryConfig;
    const NEW: Self = Self {
        frame: core::ptr::null_mut(),
        #[cfg(feature = "user_space")]
        memory_config: core::ptr::null(),
        local: ThreadLocalState::new(),
    };

    #[inline(never)]
    unsafe fn initialize_kernel_frame(
        &mut self,
        kernel_stack: Stack,
        memory_config: *const MemoryConfig,
        initial_function: extern "C" fn(usize, usize, usize),
        args: (usize, usize, usize),
    ) {
        self.memory_config = memory_config;
        self.initialize_frame(
            kernel_stack,
            asm_trampoline,
            MStatusVal::default(),
            0x0,
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
        let mstatus = MStatusVal::default()
            .with_mpie(true)
            .with_spie(true)
            .with_mpp(PrivilegeLevel::User);
        self.initialize_frame(
            kernel_stack,
            asm_user_trampoline,
            mstatus,
            initial_sp,
            (initial_pc, args.0, args.1, args.2),
        );

        Ok(())
    }
}

#[unsafe(no_mangle)]
#[unsafe(naked)]
extern "C" fn riscv_context_switch(
    old_frame: *mut *mut ContextSwitchFrame,
    new_frame: *mut ContextSwitchFrame,
) {
    naked_asm!(
        "
                // Push all of the s registers + ra onto the stack,
                // save the stack pointer into the old frame pointer.
                addi sp, sp, -52
                sw  ra, 0(sp)
                sw  s0, 4(sp)
                sw  s1, 8(sp)
                sw  s2, 12(sp)
                sw  s3, 16(sp)
                sw  s4, 20(sp)
                sw  s5, 24(sp)
                sw  s6, 28(sp)
                sw  s7, 32(sp)
                sw  s8, 36(sp)
                sw  s9, 40(sp)
                sw  s10, 44(sp)
                sw  s11, 48(sp)
                sw  sp, (a0)

                // Pop ra and s registers off the stack.
                lw  ra, 0(a1)
                lw  s0, 4(a1)
                lw  s1, 8(a1)
                lw  s2, 12(a1)
                lw  s3, 16(a1)
                lw  s4, 20(a1)
                lw  s5, 24(a1)
                lw  s6, 28(a1)
                lw  s7, 32(a1)
                lw  s8, 36(a1)
                lw  s9, 40(a1)
                lw  s10, 44(a1)
                lw  s11, 48(a1)
                addi sp, a1, 52

                ret
            "
    )
}

unsafe extern "C" fn memory_config_write(memory_config: *const MemoryConfig) {
    unsafe {
        (*memory_config).write();
    }
}

// Since the context switch frame does not contain the function arg registers,
// pass the initial function and arguments via two of the saved s registers.
#[unsafe(no_mangle)]
#[unsafe(naked)]
extern "C" fn asm_user_trampoline() {
    naked_asm!(
        "
                // If we have a stashed memory config pointer in s7,
                // then perorm the call memory_config_write(memory_config)
                // via the stashed function address in s8.
                beqz    s7, 1f
                mv      a0, s7
                jalr    ra, s8, 0

                // Store the kernel stack pointer in mscratch.
            1:
                csrw    mscratch, sp

                // Set initial SP as passed in by `initialize_frame()`.
                mv      sp, s5

                // Set args for function call.  `s` registers are offset by 1
                // because `initial_pc` is stored in s0.
                mv      a0, s1
                mv      a1, s2
                mv      a2, s3

                // Mstatus and Mepc are set up for a return to U-Mode.
                csrw    mstatus, s6
                csrw    mepc, s0
                mret
            "
    )
}

// Since the context switch frame does not contain the function arg registers,
// pass the initial function and arguments via two of the saved s registers.
#[unsafe(no_mangle)]
#[unsafe(naked)]
extern "C" fn asm_trampoline() {
    naked_asm!(
        "
                // Zero out mscratch to signify that this is a kernel thread.
                csrw    mscratch, zero

                // Set args for function call.
                mv a0, s0
                mv a1, s1
                mv a2, s2
                mv a3, s3

                tail trampoline
            "
    )
}

#[allow(unused)]
#[unsafe(no_mangle)]
extern "C" fn trampoline(
    initial_function: extern "C" fn(usize, usize, usize),
    arg0: usize,
    arg1: usize,
    arg2: usize,
) {
    debug_if!(
        LOG_THREAD_CREATE,
        "riscv trampoline: initial function {:#x} arg0 {:#x} arg1 {:#x} arg2 {:#}",
        initial_function as usize,
        arg0 as usize,
        arg1 as usize,
        arg2 as usize,
    );

    // Enable interrupts
    <crate::Arch as Arch>::InterruptController::enable_interrupts();

    // TODO: figure out how to drop the scheduler lock here?

    // Call the actual initial function of the thread.
    initial_function(arg0, arg1, arg2);

    // Get a pointer to the current thread and call exit.
    // Note: must let the scope of the lock guard close,
    // since exit_thread() does not return.
    scheduler::exit_thread(crate::Arch);

    // Does not reach.
}
