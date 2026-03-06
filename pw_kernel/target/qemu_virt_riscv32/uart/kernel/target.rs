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
#![no_main]

use arch_riscv::Arch;
use codegen as _;
use console_backend as _;
use entry as _;
use kernel_config::Uart0Config;
use kernel_uart::UartConfigInterface;
use pw_status::Result;
use riscv_semihosting::debug::{EXIT_FAILURE, EXIT_SUCCESS, exit};
use target_common::{TargetInterface, declare_target};

pub struct Target {}
struct TargetUart {}

kernel_uart::declare_uarts!(Arch, UARTS, uart_16550_kernel::Uart, [
    UART0: Uart0Config,
]);

impl test_uart::TestUart for TargetUart {
    fn enable_loopback() {
        UART0.enable_loopback()
    }

    fn read() -> Result<Option<u8>> {
        UART0.read(Arch)
    }

    fn write(byte: u8) -> Result<()> {
        UART0.write(byte)
    }
}

impl TargetInterface for Target {
    const NAME: &'static str = "QEMU-VIRT-RISCV Kernel UART";

    fn main() -> ! {
        uart_16550_kernel::init(&UARTS);

        let exit_status = match test_uart::main::<Arch, TargetUart>(Arch) {
            Ok(()) => EXIT_SUCCESS,
            Err(_e) => EXIT_FAILURE,
        };
        exit(exit_status);
        #[expect(clippy::empty_loop)]
        loop {}
    }
}

codegen::declare_kernel_interrupt_handlers!();
declare_target!(Target);
