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

#[cfg(feature = "exceptions_reload_pmp")]
use kernel::Kernel;
use kernel::syscall::{SyscallArgs, handle_syscall};
use kernel_config::{ExceptionMode, KernelConfig, RiscVKernelConfigInterface};
use log_if::debug_if;
#[cfg(feature = "exceptions_reload_pmp")]
use memory_config::MemoryConfig as _;
use pw_cast::CastInto as _;
use pw_log::info;
use pw_status::{Error, Result};
#[cfg(not(feature = "user_space"))]
pub(crate) use riscv_macro::kernel_only_exception as exception;
#[cfg(feature = "user_space")]
pub(crate) use riscv_macro::user_space_exception as exception;

#[cfg(feature = "exceptions_reload_pmp")]
use crate::MemoryConfig;
use crate::regs::{
    Cause, Exception, Interrupt, MCause, MCauseVal, MStatus, MtVal, MtVec, MtVecMode,
};
use crate::{plic, timer};

const LOG_EXCEPTIONS: bool = false;

pub struct RiscVSyscallArgs<'a> {
    frame: &'a TrapFrame,
    cur_index: usize,
}

impl<'a> RiscVSyscallArgs<'a> {
    fn new(frame: &'a TrapFrame) -> Self {
        Self {
            frame,
            cur_index: 0,
        }
    }
}

impl<'a> SyscallArgs<'a> for RiscVSyscallArgs<'a> {
    fn next_usize(&mut self) -> Result<usize> {
        let value = self.frame.a(self.cur_index)?;
        self.cur_index += 1;
        Ok(value)
    }

    #[inline(always)]
    fn next_u64(&mut self) -> Result<u64> {
        // Note: This follows the PSABI calling convention[1] which differs from
        // the outdated (but still returned in search results) ABI from the ISA[2].
        // The PSABI is what both gcc and llvm based toolchains implement.
        //
        // 1: https://github.com/riscv-non-isa/riscv-elf-psabi-doc/blob/main/riscv-cc.adoc#integer-calling-convention
        // 2: https://riscv.org/wp-content/uploads/2024/12/riscv-calling.pdf
        let low: u64 = self.next_usize()?.cast_into();
        let high: u64 = self.next_usize()?.cast_into();

        Ok(low | high << 32)
    }
}

pub fn early_init() {
    // Explicitly set up MTVEC to point to the kernel's handler to ensure
    // that it is set to the correct mode.
    let (base, mode) = match KernelConfig::get_exception_mode() {
        ExceptionMode::Direct => (_start_trap as *const () as usize, MtVecMode::Direct),
        ExceptionMode::Vectored(vec_table) => (vec_table, MtVecMode::Vectored),
    };
    MtVec::write(MtVec::read().with_base(base).with_mode(mode));
}

/// Type of all exception handlers.
#[allow(dead_code)]
pub type ExceptionHandler =
    unsafe extern "C" fn(mcause: MCauseVal, mepc: usize, frame: &mut TrapFrame);

#[inline(never)]
fn dump_exception_frame(frame: &TrapFrame) {
    info!(
        "Exception frame {:#08x}:",
        core::ptr::addr_of!(frame) as usize
    );

    info!(
        "ra  {:#010x} t0 {:#010x} t1  {:#010x} t2  {:#010x}",
        frame.ra as usize, frame.t0 as usize, frame.t1 as usize, frame.t2 as usize
    );
    info!(
        "t3  {:#010x} t4 {:#010x} t5  {:#010x} t6  {:#010x}",
        frame.t3 as usize, frame.t4 as usize, frame.t5 as usize, frame.t6 as usize
    );
    info!(
        "a0  {:#010x} a1 {:#010x} a2  {:#010x} a3  {:#010x}",
        frame.a0 as usize, frame.a1 as usize, frame.a2 as usize, frame.a3 as usize
    );
    info!(
        "a4  {:#010x} a5 {:#010x} a6  {:#010x} a7  {:#010x}",
        frame.a4 as usize, frame.a5 as usize, frame.a6 as usize, frame.a7 as usize
    );
    info!(
        "tp  {:#010x} gp {:#010x} sp  {:#010x}",
        frame.tp as usize, frame.gp as usize, frame.sp as usize
    );
    info!("mstatus {:#010x}", frame.status as usize);
    info!("mcause {:#010x}", MCause::read().0 as usize);
    info!("mtval {:#010x}", MtVal::read().0 as usize);
    info!("epc {:#010x}", frame.epc as usize);
}

// Pulls arguments out of the trap frame and calls the arch-independent syscall
// handler.
fn handle_ecall(frame: &mut TrapFrame) {
    // Explicitly truncate the syscall ID to 16 bits per syscall ABI.
    #[expect(clippy::cast_possible_truncation)]
    let id = frame.t0 as u16;
    let args = RiscVSyscallArgs::new(frame);
    let ret_val = handle_syscall(super::Arch, id, args);
    frame.a0 = ret_val.value[0];
    frame.a1 = ret_val.value[1];

    // ECALL exceptions do not "retire the instruction" requiring the advancing
    // of the PC past the ECALL instruction.  ECALLs are encoded as 4 byte
    // instructions.
    //
    // Use a wrapping add, as section 1.4 of the RISC-V unprivileged spec states:
    // "...memory address computations done by the hardware ignore overflow
    // and instead wrap around modulo 2^XLEN"
    frame.epc = frame.epc.wrapping_add(4);
}

fn exception_handler(exception: Exception, mepc: usize, frame: &mut TrapFrame) {
    // For now, always dump the exception we've received and halt.
    debug_if!(
        LOG_EXCEPTIONS,
        "Exception: exception_number={:#010x}, mepc={:#010x}, mstatus={:#010x}",
        exception as usize,
        mepc as usize,
        MStatus::read().0 as usize,
    );

    match exception {
        Exception::EnvironmentCallFromUMode | Exception::EnvironmentCallFromMMode => {
            handle_ecall(frame);
        }
        Exception::Breakpoint => {
            dump_exception_frame(frame);
            #[allow(clippy::empty_loop)]
            loop {}
        }
        _ => {
            dump_exception_frame(frame);
            pw_assert::panic!(
                "Unhandled exception: exception_number={:#010x}",
                exception as usize
            );
        }
    };
}

// Default interrupt handler
unsafe fn interrupt_handler(interrupt: Interrupt, mepc: usize, frame: &TrapFrame) {
    debug_if!(
        LOG_EXCEPTIONS,
        "Interrupt: interrupt_number={:#010x}, mepc={:#010x}, frame={:#010x}, mstatus={:#010x}",
        interrupt as usize,
        mepc as usize,
        core::ptr::addr_of!(frame) as usize,
        frame.status as usize,
    );

    match interrupt {
        Interrupt::MachineTimer => {
            timer::mtimer_tick();
        }
        Interrupt::MachineExternal => {
            plic::interrupt();
        }
        _ => {
            pw_assert::panic!(
                "Unhandled interrupt: interrupt_number={:#010x}",
                interrupt as usize
            );
        }
    }
}

#[exception(exception = "_start_trap")]
#[unsafe(no_mangle)]
unsafe extern "C" fn trap_handler(mcause: MCauseVal, mepc: usize, frame: &mut TrapFrame) {
    #[cfg(feature = "exceptions_reload_pmp")]
    unsafe {
        // Before we do anything, configure ePMP for kernel access.
        MemoryConfig::KERNEL_THREAD_MEMORY_CONFIG.write();
    }

    match mcause.cause() {
        Cause::Interrupt(interrupt) => unsafe { interrupt_handler(interrupt, mepc, frame) },
        Cause::Exception(exception) => exception_handler(exception, mepc, frame),
    }

    #[cfg(feature = "exceptions_reload_pmp")]
    {
        // When we exit the trap, get the current thread and load its memory
        // config into ePMP.
        let mut scheduler = crate::Arch.get_scheduler().lock(crate::Arch);
        unsafe {
            let tstate = scheduler.get_current_arch_thread_state();
            if tstate != core::ptr::null_mut() {
                (*(*tstate).memory_config).write();
            }
        }
    }
}

#[repr(C)]
pub struct TrapFrame {
    // Note: offsets are for 32bit sized usize
    epc: usize,    // 0x00
    status: usize, // 0x04
    ra: usize,     // 0x08

    // SAFETY: the `a()` accessor requires these to be in order.
    a0: usize, // 0x0c
    a1: usize, // 0x10
    a2: usize, // 0x14
    a3: usize, // 0x18
    a4: usize, // 0x1c
    a5: usize, // 0x20
    a6: usize, // 0x24
    a7: usize, // 0x28

    t0: usize, // 0x2c
    t1: usize, // 0x30
    t2: usize, // 0x34
    t3: usize, // 0x38
    t4: usize, // 0x3c
    t5: usize, // 0x40
    t6: usize, // 0x44

    // These are only set when handling a trap from user space.
    tp: usize, // 0x48
    gp: usize, // 0x4c
    sp: usize, // 0x50

    // Per RISC-V calling convention, stacks must always be 16 byte aligned.
    pad: [usize; 3], // 0x54-0x5f
}

impl TrapFrame {
    pub fn a(&self, index: usize) -> Result<usize> {
        if index > 7 {
            return Err(Error::InvalidArgument);
        }
        // Pointer math is used here instead of a match statement as it results
        // in significantly smaller code.
        //
        // SAFETY: Index is range checked above and the a* fields are in consecutive
        // positions in the `TrapFrame` struct.
        let usize0 = &raw const self.a0;
        Ok(unsafe { *usize0.byte_add(index * 4) })
    }
}
const _: () = assert!(core::mem::size_of::<TrapFrame>() == 0x60);
