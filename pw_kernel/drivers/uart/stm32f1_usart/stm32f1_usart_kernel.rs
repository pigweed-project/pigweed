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

//! Kernel-mode STM32F1 USART driver.
//!
//! Provides polled TX and interrupt-driven RX via a ring buffer.
//! Handles full peripheral initialization including RCC clocks, GPIO pin
//! configuration, and baud rate setup.

#![no_std]

use circular_buffer::CircularBuffer;
use kernel::Kernel;
use kernel::interrupt_controller::InterruptController;
use kernel::sync::spinlock::SpinLock;
use pw_status::Result;
pub use stm32f1_usart_regs::Stm32f1UsartConfig;

const LOG_USART: bool = false;

const BUFFER_SIZE: usize = 16;

/// RCC base address (common to all STM32F1 devices).
const RCC_BASE: usize = 0x4002_1000;
const RCC_APB2ENR_OFFSET: usize = 0x18;

pub struct Usart<K: Kernel> {
    base_address: usize,
    irq: u32,
    read_buffer: SpinLock<K, CircularBuffer<u8, BUFFER_SIZE>>,
}

impl<K: Kernel> regs::BaseAddress for Usart<K> {
    fn base_address(&self) -> usize {
        self.base_address
    }
}

impl<K: Kernel> stm32f1_usart_regs::Stm32f1UsartBaseAddress for Usart<K> {}

impl<K: Kernel> Usart<K> {
    #[must_use]
    pub const fn new(base_address: usize, irq: u32) -> Usart<K> {
        Self {
            base_address,
            irq,
            read_buffer: SpinLock::new(CircularBuffer::new()),
        }
    }

    pub fn read(&self, kernel: K) -> Result<Option<u8>> {
        log_if::debug_if!(LOG_USART, "usart read");
        Ok(self.read_buffer.lock(kernel).pop_front())
    }

    pub fn write(&self, value: u8) -> Result<()> {
        log_if::debug_if!(LOG_USART, "usart write: {}", value as u8);

        let sr = stm32f1_usart_regs::Sr;
        while !sr.read(self).txe() {}

        let mut dr = stm32f1_usart_regs::Dr;
        dr.write(self, stm32f1_usart_regs::DrValue(u32::from(value)));

        log_if::debug_if!(LOG_USART, "done");
        Ok(())
    }

    pub fn interrupt_handler(&self, kernel: K) {
        log_if::debug_if!(
            LOG_USART,
            "usart {:#010x}: interrupt_handler",
            self.base_address as usize
        );

        let sr = stm32f1_usart_regs::Sr;
        while sr.read(self).rxne() {
            let dr = stm32f1_usart_regs::Dr;
            #[allow(clippy::cast_possible_truncation)]
            let value = dr.read(self).data() as u8;
            log_if::debug_if!(LOG_USART, "data ready: {}", value as u8);
            let _ = self.read_buffer.lock(kernel).push_back(value);
        }
    }
}

/// Initialize a USART peripheral with full hardware setup.
///
/// Enables RCC clocks for the GPIO port and USART, configures the TX pin
/// as alternate-function push-pull, sets the baud rate from the APB clock
/// frequency, and enables the USART transmitter and receiver.
///
/// # Safety
/// Writes to RCC and GPIO registers. Must be called after system clock
/// initialization.
#[allow(clippy::cast_possible_truncation)]
pub unsafe fn init_usart<K: Kernel, C: Stm32f1UsartConfig>(usart: &Usart<K>, baud_rate: u32) {
    // 1. Enable GPIO port clock (always in APB2ENR).
    let apb2enr_addr = RCC_BASE + RCC_APB2ENR_OFFSET;
    // SAFETY: Writing to a valid RCC APB2ENR register to enable the GPIO
    // port clock. Called once at boot after clock initialization.
    unsafe {
        let apb2enr =
            core::ptr::read_volatile(core::ptr::with_exposed_provenance::<u32>(apb2enr_addr));
        core::ptr::write_volatile(
            core::ptr::with_exposed_provenance_mut::<u32>(apb2enr_addr),
            apb2enr | C::RCC_GPIO_BIT,
        );
    }

    // 2. Enable USART clock (APB1ENR or APB2ENR depending on USART).
    let usart_enr_addr = RCC_BASE + C::RCC_USART_ENR_OFFSET;
    // SAFETY: Writing to a valid RCC APBxENR register to enable the USART
    // peripheral clock. Called once at boot after clock initialization.
    unsafe {
        let usart_enr =
            core::ptr::read_volatile(core::ptr::with_exposed_provenance::<u32>(usart_enr_addr));
        core::ptr::write_volatile(
            core::ptr::with_exposed_provenance_mut::<u32>(usart_enr_addr),
            usart_enr | C::RCC_USART_BIT,
        );
    }

    // 3. Configure TX pin as alternate function push-pull, 50 MHz.
    // CRL covers pins 0-7, CRH covers pins 8-15.
    let pin = C::TX_PIN;
    let (crl_offset, bit_offset) = if pin < 8 {
        (0x00_usize, u32::from(pin) * 4)
    } else {
        (0x04_usize, u32::from(pin - 8) * 4)
    };
    let cr_addr = C::TX_GPIO_PORT_BASE + crl_offset;
    // SAFETY: Writing to a valid GPIO CRL/CRH register to configure the TX
    // pin as alternate-function push-pull at 50 MHz.
    unsafe {
        let cr = core::ptr::read_volatile(core::ptr::with_exposed_provenance::<u32>(cr_addr));
        // Clear the 4 config bits for this pin, then set MODE=11 (50 MHz), CNF=10 (AF push-pull).
        let cr = cr & !(0xf << bit_offset);
        let cr = cr | (0b1011 << bit_offset);
        core::ptr::write_volatile(core::ptr::with_exposed_provenance_mut::<u32>(cr_addr), cr);
    }

    // 4. Set baud rate: BRR = round(pclk_hz / baud_rate).
    // The BRR register stores USARTDIV × 16 directly as a 16-bit value
    // (mantissa in [15:4], fraction in [3:0]).
    let brr = (C::PCLK_HZ + baud_rate / 2) / baud_rate;
    let mut brr_reg = stm32f1_usart_regs::Brr;
    brr_reg.write(usart, stm32f1_usart_regs::BrrValue(brr));

    // 5. Enable USART: UE + TE (+ RE for receive capability).
    let mut cr1_reg = stm32f1_usart_regs::Cr1;
    let cr1 = stm32f1_usart_regs::Cr1Value(0)
        .with_ue(true)
        .with_te(true)
        .with_re(true);
    cr1_reg.write(usart, cr1);

    // 6. Enable RX interrupt in interrupt controller.
    K::InterruptController::enable_interrupt(usart.irq);
}
