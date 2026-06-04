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
use test_interrupts_codegen::{constants, handle};
use test_messages::TestScenario;
use userspace::syscall::Signals;
use userspace::time::Instant;
use userspace::{entry, syscall};

fn read_expected_scenario(expected_scenario: TestScenario) -> Result<()> {
    // the interrupt listener responds on IPC with the interrupt scenario.
    let wait_return = syscall::object_wait(handle::IPC, Signals::READABLE, Instant::MAX)?;

    if !wait_return.pending_signals.contains(Signals::READABLE) || wait_return.user_data != 0 {
        return Err(Error::Internal);
    }

    let mut buffer = [0u8; size_of::<TestScenario>()];
    let len = syscall::channel_read(handle::IPC, 0, &mut buffer)?;
    if len != buffer.len() {
        return Err(Error::OutOfRange);
    };

    let received_scenario = TestScenario::try_from(buffer[0])?;
    if received_scenario != expected_scenario {
        test_logger::step_failed!(
            "Unexpected test scenario received: {} (expected {})",
            received_scenario as u8,
            expected_scenario as u8
        );
        return Err(Error::Internal);
    }

    let response_buffer = [0u8; 0];
    syscall::channel_respond(handle::IPC, &response_buffer)?;

    Ok(())
}

fn test_wait_interrupt() -> Result<()> {
    syscall::debug_trigger_interrupt(constants::TEST_IRQ)?;
    read_expected_scenario(TestScenario::WaitInterrupt)
}

fn test_wait_group_interrupt() -> Result<()> {
    syscall::debug_trigger_interrupt(constants::TEST_IRQ)?;
    read_expected_scenario(TestScenario::WaitGroupInterrupt)
}

fn test_interrupts() -> Result<()> {
    test_wait_interrupt()?;
    test_wait_group_interrupt()
}

#[entry]
fn entry() -> Result<()> {
    test_logger::start("User Interrupts Test");
    let ret = test_interrupts()
        .inspect(|_| test_logger::passed("User Interrupts Test"))
        .inspect_err(|e| {
            test_logger::failed("User Interrupts Test");
            test_logger::step_failed!("status code: {}", *e as u32);
        });

    // Since this is written as a test, shut down with the return status from `main()`.
    let _ = syscall::debug_shutdown(ret);
    ret
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}
