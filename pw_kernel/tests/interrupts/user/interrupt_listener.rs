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
#![no_main]
#![no_std]

use pw_status::{Error, Result};
use test_interrupt_listener_codegen::{handle, signals};
use test_messages::TestScenario;
use userspace::syscall::Signals;
use userspace::time::{Clock, Duration, Instant, SystemClock, sleep_until};
use userspace::{entry, syscall};

fn notify_main(scenario: TestScenario, signals: Signals) -> Result<()> {
    const RECV_BUF_LEN: usize = 0;
    let send_buf = [scenario as u8];
    let mut recv_buf = [0u8; RECV_BUF_LEN];

    let bits = signals.bits();
    let len = match syscall::channel_transact(
        handle::IPC,
        &[send_buf.as_slice(), bits.to_le_bytes().as_slice()],
        &mut recv_buf,
        Instant::MAX,
    ) {
        Ok(val) => val,
        Err(err) => {
            return Err(err);
        }
    };

    if len != RECV_BUF_LEN {
        test_logger::step_failed!(
            "Received {} bytes, {} expected",
            len as usize,
            RECV_BUF_LEN as usize
        );
        return Err(Error::OutOfRange);
    }

    Ok(())
}

fn test_wait_interrupt() -> Result<()> {
    test_logger::step_start!("Testing wait on interrupt");

    // This test is designed to test multiple interrupts firing/pending. In
    // order to ensure that there are no errors in the logic surrounding
    // multiple pending interrupts, we wait Nms to guarantee they have
    // all fired.
    sleep_until(SystemClock::now() + Duration::from_millis(50))?;
    let wait_return = syscall::object_wait(
        handle::TEST_INTERRUPTS,
        signals::TEST_IRQ | signals::TEST_IRQ2,
        Instant::MAX,
    )
    .inspect_err(|_| {
        test_logger::step_failed!("Failed to wait on interrupt");
    })?;

    if !wait_return.pending_signals.contains(signals::TEST_IRQ) {
        test_logger::step_failed!(
            "Incorrect WaitReturn signals: {:#010x}",
            wait_return.pending_signals.bits() as u32
        );
        return Err(Error::Internal);
    }

    syscall::interrupt_ack(handle::TEST_INTERRUPTS, wait_return.pending_signals)?;

    test_logger::step_passed!("Wait on interrupt succeeded");

    notify_main(TestScenario::WaitInterrupt, wait_return.pending_signals)?;

    Ok(())
}

fn test_wait_group_interrupt() -> Result<()> {
    test_logger::step_start!("Testing wait on wait group containing interrupt");

    syscall::wait_group_add(
        handle::INTERRUPT_WAIT_GROUP,
        handle::TEST_INTERRUPTS,
        signals::TEST_IRQ,
        handle::TEST_INTERRUPTS as usize,
    )?;

    let wait_return = syscall::object_wait(
        handle::INTERRUPT_WAIT_GROUP,
        Signals::READABLE,
        Instant::MAX,
    )
    .inspect_err(|_| {
        test_logger::step_failed!("Failed to wait on wait group for interrupt");
    })?;

    if !wait_return.pending_signals.contains(signals::TEST_IRQ)
        || wait_return.user_data != handle::TEST_INTERRUPTS as usize
    {
        test_logger::step_failed!(
            "Incorrect WaitReturn values: pending_signals = {}, user_data = {}",
            wait_return.pending_signals.bits() as u32,
            wait_return.user_data as u32
        );
        return Err(Error::Internal);
    }

    syscall::interrupt_ack(handle::TEST_INTERRUPTS, wait_return.pending_signals)?;

    // Verify that the wait group is no longer signaled (it should time out waiting).
    let Err(err) = syscall::object_wait(
        handle::INTERRUPT_WAIT_GROUP,
        Signals::READABLE,
        SystemClock::now(),
    ) else {
        test_logger::step_failed!("WaitGroup remained signaled after interrupt ack");
        return Err(Error::Internal);
    };
    if err != Error::DeadlineExceeded {
        test_logger::step_failed!(
            "WaitGroup wait failed with unexpected error: {}",
            err as u32
        );
        return Err(err);
    }

    test_logger::step_passed!("Wait on wait group containing interrupt succeeded");

    notify_main(
        TestScenario::WaitGroupInterrupt,
        wait_return.pending_signals,
    )?;

    Ok(())
}

fn run_test() -> Result<()> {
    test_wait_interrupt()?;
    test_wait_group_interrupt()?;
    Ok(())
}

#[entry]
fn entry() -> Result<()> {
    run_test().inspect_err(|e| {
        let _ = syscall::debug_shutdown(Err(*e));
    })
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}
