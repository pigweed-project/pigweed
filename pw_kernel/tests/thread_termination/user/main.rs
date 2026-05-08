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
#![no_std]
#![no_main]

use core::sync::atomic::{AtomicU32, Ordering};

use main_codegen::handle;
use pw_log::info;
use pw_status::Result;
use userspace::{entry, syscall};

// NOTE: Atomic operations will not work on platforms without atomic support.
static THREAD_DONE: AtomicU32 = AtomicU32::new(0);

#[unsafe(no_mangle)]
pub extern "C" fn test_thread_entry(_arg: usize) -> ! {
    info!("Test thread started");
    THREAD_DONE.store(1, Ordering::SeqCst);
    info!("Test thread exiting");

    syscall::thread_exit(42);
}

#[unsafe(no_mangle)]
pub extern "C" fn spin_thread_entry(_arg: usize) -> ! {
    info!("Spin thread started");
    loop {}
}

fn do_test() -> Result<()> {
    info!("🔄 [User Thread Termination] RUNNING");

    let thread_handle = handle::TEST_THREAD;

    static mut THREAD_STACK: [u8; 1024] = [0; 1024];

    let initial_pc = test_thread_entry as *const () as usize;
    let initial_sp =
        unsafe { core::ptr::addr_of_mut!(THREAD_STACK).cast::<u8>().add(1024) as usize };

    info!("🔄 ├─ Starting test thread");
    syscall::thread_start(thread_handle, initial_pc, initial_sp)?;

    info!("🔄 ├─ Waiting for test thread to terminate");
    syscall::object_wait(
        thread_handle,
        syscall::Signals::JOINABLE,
        userspace::time::Instant::MAX,
    )?;
    let status = syscall::thread_join(thread_handle)?;
    if status != syscall::ExitStatus::Success(42) {
        pw_log::error!("❌ ├─ Thread joined with unexpected status");
        return Err(pw_status::Error::Internal);
    }

    info!("🔄 ├─ Thread joined");
    let done = THREAD_DONE.load(Ordering::SeqCst);
    if done != 1 {
        return Err(pw_status::Error::Internal);
    }

    info!("🔄 ├─ Starting test thread again for external termination");
    let initial_pc = spin_thread_entry as *const () as usize;
    syscall::thread_start(thread_handle, initial_pc, initial_sp)?;

    info!("🔄 ├─ Terminating test thread from main thread");
    syscall::thread_terminate(thread_handle)?;

    info!("🔄 ├─ Waiting for test thread to terminate");
    syscall::object_wait(
        thread_handle,
        syscall::Signals::JOINABLE,
        userspace::time::Instant::from_ticks(u64::MAX),
    )?;
    let status = syscall::thread_join(thread_handle)?;
    if status != syscall::ExitStatus::TerminatedBySyscall {
        pw_log::error!("❌ ├─ Thread joined with unexpected status (expected TerminatedBySyscall)");
        return Err(pw_status::Error::Internal);
    }
    info!("🔄 ├─ Thread joined (terminated from outside)");

    Ok(())
}

#[entry]
fn main_entry() -> Result<()> {
    let ret = do_test()
        .inspect(|_| pw_log::info!("✅ └─ PASSED"))
        .inspect_err(|e| {
            pw_log::error!("❌ ├─ FAILED");
            pw_log::error!("❌ └─ status code: {}", *e as u32);
        });

    let _ = syscall::debug_shutdown(ret);
    ret
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    pw_log::error!("❌ PANIC");
    let _ = syscall::debug_shutdown(Err(pw_status::Error::Internal));
    loop {}
}
