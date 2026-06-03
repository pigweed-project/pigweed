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

use foreign_box::ForeignRc;
use memory_config::MemoryRegionType;
use pw_cast::CastInto as _;
use pw_log::info;
use pw_status::{Error, Result};
use syscall_defs::{ExitStatus, Signals, SysCallId, SysCallReturnValue, WaitReturn};
use time::{Clock, Instant};

use crate::Kernel;
use crate::interrupt_controller::InterruptController;
use crate::object::{KernelObject, SyscallBuffer};
const SYSCALL_DEBUG: bool = false;

/// An arch-specific collection of syscall arguments
///
/// Since architectures ABI, calling, and syscall conventions can differ, this
/// trait provides a common API for extracting syscall arguments.  The API does
/// not provide any index based methods for retrieving arguments because, on some
/// architectures, the location of an argument can depend of the size of the
/// preceding arguments.
pub trait SyscallArgs<'a> {
    /// Return the next `usize` argument.
    fn next_usize(&mut self) -> Result<usize>;

    /// Return the next `u64` argument.
    fn next_u64(&mut self) -> Result<u64>;

    /// Return the next `u32` argument.
    #[inline(always)]
    fn next_u32(&mut self) -> Result<u32> {
        u32::try_from(self.next_usize()?).map_err(|_| Error::InvalidArgument)
    }

    /// Return the next `Instant` argument.
    #[inline(always)]
    fn next_instant<C: Clock>(&mut self) -> Result<Instant<C>> {
        Ok(Instant::from_ticks(self.next_u64()?))
    }
}

pub fn lookup_handle<K: Kernel>(
    kernel: K,
    handle: u32,
) -> Result<ForeignRc<K::AtomicUsize, dyn KernelObject<K>>> {
    let Some(object) = kernel
        .get_scheduler()
        .lock(kernel)
        .current_thread()
        .get_object(kernel, handle)
    else {
        log_if::debug_if!(
            SYSCALL_DEBUG,
            "sycall: ObjectWait can't access handle {:#010x}",
            handle as u32
        );
        return Err(Error::OutOfRange);
    };

    Ok(object)
}

fn handle_object_wait<'a, K: Kernel>(
    kernel: K,
    mut args: K::SyscallArgs<'a>,
) -> Result<WaitReturn> {
    let handle = args.next_u32()?;
    let signals = args.next_u32()?;
    let deadline = args.next_instant()?;
    log_if::debug_if!(
        SYSCALL_DEBUG,
        "syscall: handling object_wait {:#010x} {:#010x} {:#018x}",
        handle as u32,
        signals as u32,
        deadline.ticks() as u64
    );

    let object = lookup_handle(kernel, handle)?;
    let Some(signal_mask) = Signals::from_bits(signals) else {
        log_if::debug_if!(
            SYSCALL_DEBUG,
            "syscall: ObjectWait invalid signal mask: {:#010x}",
            signals as u32
        );

        return Err(Error::InvalidArgument);
    };

    let ret = object.object_wait(kernel, signal_mask, deadline);
    log_if::debug_if!(SYSCALL_DEBUG, "syscall: object_wait complete");
    ret
}

fn handle_wait_group_add<'a, K: Kernel>(kernel: K, mut args: K::SyscallArgs<'a>) -> Result<()> {
    let wait_group_handle = args.next_u32()?;
    let object_handle = args.next_u32()?;
    let signals = args.next_u32()?;
    let user_data = args.next_usize()?;

    log_if::debug_if!(
        SYSCALL_DEBUG,
        "syscall: handling wait_group_add {:#010x} {:#010x} {:#010x} {:#x}",
        wait_group_handle as u32,
        object_handle as u32,
        signals as u32,
        user_data as usize
    );

    let wait_group = lookup_handle(kernel, wait_group_handle)?;
    let object = lookup_handle(kernel, object_handle)?;
    let Some(signal_mask) = Signals::from_bits(signals) else {
        log_if::debug_if!(
            SYSCALL_DEBUG,
            "syscall: WaitGroupAdd invalid signal mask: {:#010x}",
            signals as u32
        );

        return Err(Error::InvalidArgument);
    };

    // Safety: `wait_group` is returned from lookup_handle() as a ForeignRc.
    let ret = unsafe { wait_group.wait_group_add(kernel, &*object, signal_mask, user_data) };
    log_if::debug_if!(SYSCALL_DEBUG, "syscall: wait_group_add complete");
    ret
}

fn handle_wait_group_remove<'a, K: Kernel>(kernel: K, mut args: K::SyscallArgs<'a>) -> Result<()> {
    let wait_group_handle = args.next_u32()?;
    let object_handle = args.next_u32()?;

    log_if::debug_if!(
        SYSCALL_DEBUG,
        "syscall: handling wait_group_remove {:#010x} {:#010x}",
        wait_group_handle as u32,
        object_handle as u32
    );

    let wait_group = lookup_handle(kernel, wait_group_handle)?;
    let object = lookup_handle(kernel, object_handle)?;
    let ret = wait_group.wait_group_remove(kernel, &*object);
    log_if::debug_if!(SYSCALL_DEBUG, "syscall: wait_group_remove complete");
    ret
}

fn handle_channel_transact<'a, K: Kernel>(kernel: K, mut args: K::SyscallArgs<'a>) -> Result<u64> {
    log_if::debug_if!(SYSCALL_DEBUG, "syscall: handling channel_transact");
    let handle = args.next_u32()?;
    let send_data_addr = args.next_usize()?;
    let send_data_len = args.next_usize()?;
    let recv_data_addr = args.next_usize()?;
    let recv_data_len = args.next_usize()?;
    let deadline: Instant<K::Clock> = args.next_instant()?;

    log_if::debug_if!(
        SYSCALL_DEBUG,
        "syscall: handling channel_transact({:#x}, {:#x}, {:#x}, {:#x}, {:#x}, {:#x})",
        handle as u32,
        send_data_addr as usize,
        send_data_len as usize,
        recv_data_addr as usize,
        recv_data_len as usize,
        deadline.ticks() as u64,
    );

    let object = lookup_handle(kernel, handle)?;
    let send_buffer = SyscallBuffer::new_in_current_process(
        kernel,
        MemoryRegionType::ReadOnlyData,
        send_data_addr,
        send_data_len.cast_signed(),
    )?;
    let recv_buffer = SyscallBuffer::new_in_current_process(
        kernel,
        MemoryRegionType::ReadWriteData,
        recv_data_addr,
        recv_data_len.cast_signed(),
    )?;

    let ret = object.channel_transact(kernel, send_buffer, recv_buffer, deadline);
    log_if::debug_if!(SYSCALL_DEBUG, "syscall: channel_transact complete");
    ret.map(|v| v.cast_into())
}

fn handle_channel_async_transact<'a, K: Kernel>(
    kernel: K,
    mut args: K::SyscallArgs<'a>,
) -> Result<()> {
    log_if::debug_if!(SYSCALL_DEBUG, "syscall: handling channel_async_transact");
    let handle = args.next_u32()?;
    let send_data_addr = args.next_usize()?;
    let send_data_len = args.next_usize()?;
    let recv_data_addr = args.next_usize()?;
    let recv_data_len = args.next_usize()?;

    let object = lookup_handle(kernel, handle)?;
    let send_buffer = SyscallBuffer::new_in_current_process(
        kernel,
        MemoryRegionType::ReadOnlyData,
        send_data_addr,
        send_data_len.cast_signed(),
    )?;
    let recv_buffer = SyscallBuffer::new_in_current_process(
        kernel,
        MemoryRegionType::ReadWriteData,
        recv_data_addr,
        recv_data_len.cast_signed(),
    )?;

    let ret = object.channel_async_transact(kernel, send_buffer, recv_buffer);
    log_if::debug_if!(SYSCALL_DEBUG, "syscall: channel_async_transact complete");
    ret
}

fn handle_channel_async_transact_complete<'a, K: Kernel>(
    kernel: K,
    mut args: K::SyscallArgs<'a>,
) -> Result<u64> {
    log_if::debug_if!(
        SYSCALL_DEBUG,
        "syscall: handling channel_async_transact_complete"
    );
    let handle = args.next_u32()?;
    let object = lookup_handle(kernel, handle)?;

    let ret = object.channel_async_transact_complete(kernel);
    log_if::debug_if!(
        SYSCALL_DEBUG,
        "syscall: channel_async_transact_complete complete"
    );
    ret.map(|v| v.cast_into())
}

fn handle_channel_async_cancel<'a, K: Kernel>(
    kernel: K,
    mut args: K::SyscallArgs<'a>,
) -> Result<()> {
    log_if::debug_if!(SYSCALL_DEBUG, "syscall: handling channel_async_cancel");
    let handle = args.next_u32()?;
    let object = lookup_handle(kernel, handle)?;

    let ret = object.channel_async_cancel(kernel);
    log_if::debug_if!(SYSCALL_DEBUG, "syscall: channel_async_cancel complete");
    ret
}

fn handle_channel_read<'a, K: Kernel>(kernel: K, mut args: K::SyscallArgs<'a>) -> Result<u64> {
    log_if::debug_if!(SYSCALL_DEBUG, "syscall: handling channel_read");
    let handle = args.next_u32()?;
    let offset = args.next_usize()?;
    let buffer_addr = args.next_usize()?;
    let buffer_len = args.next_usize()?;

    let object = lookup_handle(kernel, handle)?;
    let buffer = SyscallBuffer::new_in_current_process(
        kernel,
        MemoryRegionType::ReadWriteData,
        buffer_addr,
        buffer_len.cast_signed(),
    )?;

    let ret = object.channel_read(kernel, offset, buffer);
    log_if::debug_if!(SYSCALL_DEBUG, "syscall: channel_read complete");
    ret.map(|v| v.cast_into())
}

fn handle_channel_respond<'a, K: Kernel>(kernel: K, mut args: K::SyscallArgs<'a>) -> Result<()> {
    log_if::debug_if!(SYSCALL_DEBUG, "syscall: handling channel_respond");
    let handle = args.next_u32()?;
    let buffer_addr = args.next_usize()?;
    let buffer_len = args.next_usize()?;

    let object = lookup_handle(kernel, handle)?;
    let buffer = SyscallBuffer::new_in_current_process(
        kernel,
        MemoryRegionType::ReadOnlyData,
        buffer_addr,
        buffer_len.cast_signed(),
    )?;

    let ret = object.channel_respond(kernel, buffer);
    log_if::debug_if!(SYSCALL_DEBUG, "syscall: channel_respond complete");
    ret
}

fn handle_interrupt_ack<'a, K: Kernel>(kernel: K, mut args: K::SyscallArgs<'a>) -> Result<()> {
    log_if::debug_if!(SYSCALL_DEBUG, "syscall: handling interrupt_ack");
    let handle = args.next_u32()?;
    let signals = args.next_u32()?;

    let Some(signal_mask) = Signals::from_bits(signals) else {
        log_if::debug_if!(
            SYSCALL_DEBUG,
            "syscall: InterruptAck invalid signal mask: {:#010x}",
            signals as u32
        );

        return Err(Error::InvalidArgument);
    };

    let object = lookup_handle(kernel, handle)?;
    let ret = object.interrupt_ack(kernel, signal_mask);
    log_if::debug_if!(SYSCALL_DEBUG, "syscall: interrupt_ack complete");
    ret
}

fn handle_thread_start<'a, K: Kernel>(kernel: K, mut args: K::SyscallArgs<'a>) -> Result<()> {
    log_if::debug_if!(SYSCALL_DEBUG, "syscall: handling thread_start");
    let handle = args.next_u32()?;
    let initial_pc = args.next_usize()?;
    let initial_sp = args.next_usize()?;
    let object = lookup_handle(kernel, handle)?;
    object.thread_start(kernel, initial_pc, initial_sp)
}

fn handle_task_terminate<'a, K: Kernel>(kernel: K, mut args: K::SyscallArgs<'a>) -> Result<()> {
    log_if::debug_if!(SYSCALL_DEBUG, "syscall: handling task_terminate");
    let handle = args.next_u32()?;
    let object = lookup_handle(kernel, handle)?;
    object.task_terminate(kernel)
}

fn handle_task_join<'a, K: Kernel>(kernel: K, mut args: K::SyscallArgs<'a>) -> Result<ExitStatus> {
    log_if::debug_if!(SYSCALL_DEBUG, "syscall: handling task_join");
    let handle = args.next_u32()?;
    let object = lookup_handle(kernel, handle)?;
    object.task_join(kernel)
}

fn handle_thread_exit<'a, K: Kernel>(kernel: K, mut args: K::SyscallArgs<'a>) -> ! {
    log_if::debug_if!(SYSCALL_DEBUG, "syscall: handling thread_exit");
    // TODO: b/510812835 - infallible syscalls.
    let exit_code = args.next_u32().unwrap_or(0);
    crate::scheduler::exit_thread(kernel, syscall_defs::ExitStatus::Success(exit_code));
}

fn handle_process_exit<'a, K: Kernel>(kernel: K, mut args: K::SyscallArgs<'a>) -> ! {
    log_if::debug_if!(SYSCALL_DEBUG, "syscall: handling process_exit");
    // TODO: b/510812835 - infallible syscalls.
    let exit_code = args.next_u32().unwrap_or(0);
    let mut sched = kernel.get_scheduler().lock(kernel);
    let current_process = sched.current_process_handle().clone();
    sched.process_terminate(
        kernel,
        &current_process,
        syscall_defs::ExitStatus::Success(exit_code),
    );

    // Drop the reference to the current process before calling exit_thread.
    // Since exit_thread does not return, the reference would otherwise be leaked,
    // preventing the process from being successfully joined (which requires ref_count == 1).
    drop(current_process);

    // Release the scheduler lock to allow preemption.
    drop(sched);

    // Now exit the current thread as well.
    crate::scheduler::exit_thread(kernel, syscall_defs::ExitStatus::ProcessTerminated);
}

fn handle_process_start<'a, K: Kernel>(kernel: K, mut args: K::SyscallArgs<'a>) -> Result<()> {
    log_if::debug_if!(SYSCALL_DEBUG, "syscall: handling process_start");
    let handle = args.next_u32()?;
    let object = lookup_handle(kernel, handle)?;
    object.process_start(kernel)
}

fn handle_set_peer_user_signal<'a, K: Kernel>(
    kernel: K,
    mut args: K::SyscallArgs<'a>,
) -> Result<()> {
    log_if::debug_if!(
        SYSCALL_DEBUG,
        "syscall: handling object_set_peer_user_signal"
    );
    let handle = args.next_u32()?;
    let set = args.next_u32()? != 0;

    let object = lookup_handle(kernel, handle)?;
    let ret = object.object_set_peer_user_signal(kernel, set);
    log_if::debug_if!(
        SYSCALL_DEBUG,
        "syscall: object_set_peer_user_signal complete"
    );
    ret
}

// TODO: Remove this syscall when logging is added.
fn handle_debug_putc<'a, K: Kernel>(kernel: K, mut args: K::SyscallArgs<'a>) -> Result<u64> {
    log_if::debug_if!(SYSCALL_DEBUG, "syscall: handling debug_putc");
    let arg = args.next_u32()?;
    let c = char::from_u32(arg).ok_or(Error::InvalidArgument)?;
    let sched = kernel.get_scheduler().lock(kernel);
    info!("{}: {}", sched.current_thread().name as &str, c as char);
    log_if::debug_if!(SYSCALL_DEBUG, "syscall: debug_putc complete");
    Ok(u64::from(arg))
}

// TODO: Consider adding an feature flagged PowerManager object and move this shutdown call to it.
fn handle_debug_shutdown<'a, K: Kernel>(kernel: K, mut args: K::SyscallArgs<'a>) -> Result<u64> {
    log_if::debug_if!(SYSCALL_DEBUG, "syscall: handling debug_shutdown");
    let exit_code = args.next_u32()?;
    if exit_code != 0 {
        // Allow `kernel` to be unused if `system_dump_on_failure` is not enabled.
        let _ = kernel;
        #[cfg(feature = "system_dump_on_failure")]
        {
            pw_log::info!(
                "Kernel exiting with failure code {}. Dumping system state:",
                exit_code as i32
            );

            kernel.get_scheduler().lock(kernel).dump(kernel);
        }
    }
    crate::target::shutdown(exit_code);
}

fn handle_debug_log<'a, K: Kernel>(kernel: K, mut args: K::SyscallArgs<'a>) -> Result<()> {
    let buffer_addr = args.next_usize()?;
    let buffer_len = args.next_usize()?;
    let buffer = SyscallBuffer::new_in_current_process(
        kernel,
        MemoryRegionType::ReadOnlyData,
        buffer_addr,
        buffer_len.cast_signed(),
    )?;
    let mut console = console::Console::new();
    for byteslice in buffer.as_slices() {
        console.write_all(byteslice)?;
    }
    Ok(())
}

fn handle_debug_trigger_interrupt<'a, K: Kernel>(
    _kernel: K,
    mut args: K::SyscallArgs<'a>,
) -> Result<()> {
    log_if::debug_if!(SYSCALL_DEBUG, "syscall: handling debug_trigger_interrupt");

    let irq = args.next_u32()?;
    K::InterruptController::trigger_interrupt(irq);

    log_if::debug_if!(SYSCALL_DEBUG, "syscall: debug_trigger_interrupt complete");
    Ok(())
}

fn handle_debug_clock_now<'a, K: Kernel>(kernel: K, mut _args: K::SyscallArgs<'a>) -> u64 {
    kernel.now().ticks()
}

/// Handle a system call.
///
/// This is the architecture-independent entry point for system calls.
///
/// # Requirements
/// The architecture-specific code calling this function MUST check if the
/// current thread is terminating before returning to userspace (e.g., by
/// calling `kernel::interrupt_controller::handle_thread_termination`).
/// Since in-kernel thread termination is advisory, this ensures that
/// terminating threads actually exit instead of returning to userspace.
pub fn handle_syscall<'a, K: Kernel>(
    kernel: K,
    id: u16,
    args: K::SyscallArgs<'a>,
) -> SysCallReturnValue {
    log_if::debug_if!(SYSCALL_DEBUG, "syscall: {:#06x}", id as usize);

    // Instead of having a architecture independent match here, an array of
    // extern "C" function pointers could be kept and use the architecture's
    // calling convention to directly call them.
    //
    // This allows [`arch::arm_cortex_m::in_interrupt_handler()`] to treat
    // active SVCalls as not in interrupt context.
    let id = match SysCallId::try_from(id) {
        Ok(id) => id,
        Err(_) => {
            log_if::debug_if!(
                SYSCALL_DEBUG,
                "syscall: unknown syscall {}",
                u32::from(id) as u32
            );
            return SysCallReturnValue::from(-(Error::InvalidArgument as isize) as i64);
        }
    };
    let res = {
        match id {
            SysCallId::ObjectWait => handle_object_wait(kernel, args).into(),
            SysCallId::WaitGroupAdd => handle_wait_group_add(kernel, args).into(),
            SysCallId::WaitGroupRemove => handle_wait_group_remove(kernel, args).into(),
            SysCallId::ChannelTransact => handle_channel_transact(kernel, args).into(),
            SysCallId::ChannelAsyncTransact => handle_channel_async_transact(kernel, args).into(),
            SysCallId::ChannelAsyncTransactComplete => {
                handle_channel_async_transact_complete(kernel, args).into()
            }
            SysCallId::ChannelAsyncCancel => handle_channel_async_cancel(kernel, args).into(),
            SysCallId::ChannelRead => handle_channel_read(kernel, args).into(),
            SysCallId::ChannelRespond => handle_channel_respond(kernel, args).into(),
            SysCallId::InterruptAck => handle_interrupt_ack(kernel, args).into(),
            SysCallId::ThreadStart => handle_thread_start(kernel, args).into(),
            SysCallId::TaskTerminate => handle_task_terminate(kernel, args).into(),
            SysCallId::TaskJoin => handle_task_join(kernel, args).into(),
            SysCallId::ThreadExit => handle_thread_exit(kernel, args),
            SysCallId::ProcessStart => handle_process_start(kernel, args).into(),
            SysCallId::ProcessExit => handle_process_exit(kernel, args),
            SysCallId::RaisePeerUserSignal => handle_set_peer_user_signal(kernel, args).into(),
            SysCallId::DebugPutc => handle_debug_putc(kernel, args).into(),
            SysCallId::DebugShutdown => handle_debug_shutdown(kernel, args).into(),
            SysCallId::DebugLog => handle_debug_log(kernel, args).into(),
            SysCallId::DebugNop => 0u64.into(),
            SysCallId::DebugTriggerInterrupt => handle_debug_trigger_interrupt(kernel, args).into(),
            SysCallId::DebugClockNow => handle_debug_clock_now(kernel, args).into(),
            SysCallId::SysCallLowerBound
            | SysCallId::SysCallUpperBound
            | SysCallId::DebugSysCallLowerBound
            | SysCallId::DebugSysCallUpperBound => {
                pw_assert::panic!("Marker variants are unreachable");
            }
        }
    };
    log_if::debug_if!(SYSCALL_DEBUG, "syscall: {:#06x} returning", id as usize);

    res
}
