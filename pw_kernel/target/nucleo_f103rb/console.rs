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

//! Console backend for the Nucleo-F103RB.
//!
//! Thin glue that initializes USART2 via the stm32f1_usart driver and
//! delegates writes to it.

#![no_std]

use arch_arm_cortex_m::Arch;
use kernel::sync::spinlock::SpinLock;
use kernel_config::Usart2Config;
use kernel_uart::UartConfigInterface;
use pw_status::Result;

static USART2: stm32f1_usart_kernel::Usart<Arch> =
    stm32f1_usart_kernel::Usart::new(Usart2Config::BASE_ADDRESS, Usart2Config::IRQ);

static INITIALIZED: SpinLock<Arch, bool> = SpinLock::new(false);

/// Initialize USART2 as the console output.
pub fn init() {
    let mut initialized = INITIALIZED.lock(Arch);
    if *initialized {
        return;
    }

    // SAFETY: Called once at boot after clock_init(). The driver handles
    // RCC peripheral clocks, GPIO pin configuration, and baud rate setup.
    unsafe {
        stm32f1_usart_kernel::init_usart::<Arch, Usart2Config>(&USART2, 115_200);
    }

    *initialized = true;
}

#[unsafe(no_mangle)]
pub fn console_backend_write_all(buf: &[u8]) -> Result<()> {
    let initialized = INITIALIZED.lock(Arch);
    if !*initialized {
        return Ok(());
    }
    // Drop the lock before the write loop to avoid holding it during I/O.
    drop(initialized);

    for &byte in buf {
        let _ = USART2.write(byte);
    }
    Ok(())
}
