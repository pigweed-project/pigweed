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

pub use kernel_config::{CortexMKernelConfigInterface, KernelConfigInterface, NvicConfigInterface};
use stm32f1_usart_regs::Stm32f1UsartConfig;

pub struct KernelConfig;

impl CortexMKernelConfigInterface for KernelConfig {
    // Board: 72 MHz via HSE Bypass (ST-Link MCO, SB54 closed) + PLL ×9.
    // QEMU: 25 MHz default system clock on mps2-an385.
    #[cfg(feature = "board")]
    const SYS_TICK_HZ: u32 = 72_000_000;
    #[cfg(feature = "qemu")]
    const SYS_TICK_HZ: u32 = 25_000_000;

    // STM32F103 has no physical MPU, but the kernel requires at least one
    // region for KERNEL_THREAD_MEMORY_CONFIG. MPU register writes are
    // harmlessly ignored (RAZ/WI) on hardware without an MPU.
    const NUM_MPU_REGIONS: usize = 8;
}

impl KernelConfigInterface for KernelConfig {
    const SYSTEM_CLOCK_HZ: u64 = KernelConfig::SYS_TICK_HZ as u64;

    // Reduced from default 2048 to fit within 20 KB RAM.
    const KERNEL_STACK_SIZE_BYTES: usize = 1024;
}

pub struct NvicConfig;

impl NvicConfigInterface for NvicConfig {
    // STM32F103xB medium-density: 43 maskable interrupt channels.
    #[cfg(feature = "board")]
    const MAX_IRQS: u32 = 43;
    // mps2-an385 only implements 32 external NVIC lines (0-31). See
    // interrupts/kernel/system_qemu.json5 for details.
    #[cfg(feature = "qemu")]
    const MAX_IRQS: u32 = 32;
}

/// USART2 configuration for the Nucleo-F103RB.
/// TX on PA2 (connected to ST-Link virtual COM port).
pub struct Usart2Config;

impl kernel_uart::UartConfigInterface for Usart2Config {
    const BASE_ADDRESS: usize = 0x4000_4400;
    // USART2 global interrupt (position 38 in STM32F103 vector table).
    const IRQ: u32 = 38;
}

impl Stm32f1UsartConfig for Usart2Config {
    const TX_GPIO_PORT_BASE: usize = 0x4001_0800; // GPIOA
    const TX_PIN: u8 = 2; // PA2
    const RCC_USART_ENR_OFFSET: usize = 0x1c; // APB1ENR
    const RCC_USART_BIT: u32 = 1 << 17; // USART2EN
    const RCC_GPIO_BIT: u32 = 1 << 2; // IOPAEN (in APB2ENR)
    const PCLK_HZ: u32 = 36_000_000; // APB1 = SYSCLK / 2 = 36 MHz
}
