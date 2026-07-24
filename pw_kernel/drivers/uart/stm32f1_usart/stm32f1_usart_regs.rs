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

//! Register definitions for STM32F1 USART peripheral (RM0008).

#![no_std]

use regs::{ro_block_reg, rw_block_reg, rw_bool_field, rw_int_field};

/// Marker trait to ensure only STM32F1 USART drivers use these registers.
pub trait Stm32f1UsartBaseAddress: regs::BaseAddress {}

/// STM32F1-specific USART configuration.
///
/// Provides the GPIO and RCC constants needed for full peripheral
/// initialization. Each USART instance on a board implements this trait.
///
/// Lives in the regs crate (not the kernel driver) to avoid dependency
/// cycles when implemented by kernel_config.
pub trait Stm32f1UsartConfig {
    /// Base address of the TX GPIO port (e.g., GPIOA = 0x4001_0800).
    const TX_GPIO_PORT_BASE: usize;
    /// TX pin number within the port (e.g., 2 for PA2).
    const TX_PIN: u8;
    /// Offset of the RCC APBxENR register for this USART (APB1ENR=0x1c, APB2ENR=0x18).
    const RCC_USART_ENR_OFFSET: usize;
    /// Bit mask to enable this USART's clock in the APBxENR register.
    const RCC_USART_BIT: u32;
    /// Bit mask to enable the TX GPIO port's clock in APB2ENR.
    const RCC_GPIO_BIT: u32;
    /// APB peripheral clock frequency in Hz (used for baud rate calculation).
    const PCLK_HZ: u32;
}

// --- SR: Status Register (offset 0x00) ---
ro_block_reg!(
    Sr,
    SrValue,
    u32,
    Stm32f1UsartBaseAddress,
    0x00,
    "Status Register"
);
#[derive(Copy, Clone)]
#[repr(transparent)]
pub struct SrValue(pub u32);
impl SrValue {
    rw_bool_field!(u32, pe, 0, "Parity Error");
    rw_bool_field!(u32, fe, 1, "Framing Error");
    rw_bool_field!(u32, nf, 2, "Noise Detected Flag");
    rw_bool_field!(u32, ore, 3, "Overrun Error");
    rw_bool_field!(u32, idle, 4, "IDLE Line Detected");
    rw_bool_field!(u32, rxne, 5, "Read Data Register Not Empty");
    rw_bool_field!(u32, tc, 6, "Transmission Complete");
    rw_bool_field!(u32, txe, 7, "Transmit Data Register Empty");
}

// --- DR: Data Register (offset 0x04) ---
rw_block_reg!(
    Dr,
    DrValue,
    u32,
    Stm32f1UsartBaseAddress,
    0x04,
    "Data Register"
);
#[derive(Copy, Clone)]
#[repr(transparent)]
pub struct DrValue(pub u32);
impl DrValue {
    rw_int_field!(u32, data, 0, 8, u16, "Data Value");
}

// --- BRR: Baud Rate Register (offset 0x08) ---
rw_block_reg!(
    Brr,
    BrrValue,
    u32,
    Stm32f1UsartBaseAddress,
    0x08,
    "Baud Rate Register"
);
#[derive(Copy, Clone)]
#[repr(transparent)]
pub struct BrrValue(pub u32);
impl BrrValue {
    rw_int_field!(u32, div_fraction, 0, 3, u8, "Fraction of USARTDIV");
    rw_int_field!(u32, div_mantissa, 4, 15, u16, "Mantissa of USARTDIV");
}

// --- CR1: Control Register 1 (offset 0x0C) ---
rw_block_reg!(
    Cr1,
    Cr1Value,
    u32,
    Stm32f1UsartBaseAddress,
    0x0c,
    "Control Register 1"
);
#[derive(Copy, Clone)]
#[repr(transparent)]
pub struct Cr1Value(pub u32);
impl Cr1Value {
    rw_bool_field!(u32, re, 2, "Receiver Enable");
    rw_bool_field!(u32, te, 3, "Transmitter Enable");
    rw_bool_field!(u32, rxneie, 5, "RXNE Interrupt Enable");
    rw_bool_field!(u32, tcie, 6, "Transmission Complete Interrupt Enable");
    rw_bool_field!(u32, ue, 13, "USART Enable");
}
