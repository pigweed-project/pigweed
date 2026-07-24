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

//! System clock configuration for the Nucleo-F103RB (STM32F103RBT6).
//!
//! Configures 72 MHz SYSCLK via HSE Bypass (ST-Link MCO 8 MHz on OSC_IN
//! via SB54) → PLL ×9.

#![no_std]

use stm32f1_rcc_regs::{FlashAcr, FlashAcrVal, RccCfgr, RccCfgrVal, RccCr};

/// Configure system clock to 72 MHz via HSE Bypass + PLL ×9.
///
/// Uses HSE Bypass mode (ST-Link MCO provides 8 MHz on OSC_IN via SB54),
/// then PLL ×9 = 72 MHz.
///
/// # Safety
/// Must be called once at boot before any peripheral initialization.
/// Writes to RCC and FLASH registers.
pub unsafe fn clock_init() {
    let mut rcc_cr = RccCr;
    let mut rcc_cfgr = RccCfgr;
    let mut flash_acr = FlashAcr;

    // Enable HSE in bypass mode (external clock, not crystal).
    // HSEBYP must be set before HSEON per RM0008.
    let cr = rcc_cr.read();
    rcc_cr.write(cr.with_hsebyp(true));
    let cr = rcc_cr.read();
    rcc_cr.write(cr.with_hseon(true));

    // Wait for HSE ready.
    while !rcc_cr.read().hserdy() {}

    // Set flash latency to 2 wait states (required for 48-72 MHz).
    flash_acr.write(FlashAcrVal(0).with_latency(2));

    // Configure PLL: HSE (8 MHz) as source, multiply by 9 → 72 MHz
    // APB1 prescaler = /2 → PCLK1 = 36 MHz (max for APB1)
    // APB2 prescaler = /1 → PCLK2 = 72 MHz
    // AHB prescaler = /1 → HCLK = 72 MHz
    let cfgr = RccCfgrVal(0)
        .with_pllsrc(true)
        .with_pllmul(0b0111) // ×9
        .with_hpre(0b0000) // AHB /1
        .with_ppre1(0b100) // APB1 /2
        .with_ppre2(0b000); // APB2 /1
    rcc_cfgr.write(cfgr);

    // Enable PLL.
    let cr = rcc_cr.read();
    rcc_cr.write(cr.with_pllon(true));

    // Wait for PLL ready.
    while !rcc_cr.read().pllrdy() {}

    // Switch system clock to PLL.
    let cfgr = rcc_cfgr.read();
    rcc_cfgr.write(cfgr.with_sw(0b10)); // SW = PLL

    // Wait until PLL is the system clock source.
    while rcc_cfgr.read().sws() != 0b10 {}
}
