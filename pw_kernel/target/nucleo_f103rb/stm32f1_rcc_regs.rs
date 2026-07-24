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

//! Register definitions for STM32F1 RCC and FLASH peripherals (RM0008).

#![no_std]

use regs::{ro_bool_field, ro_int_field, rw_bool_field, rw_int_field, rw_reg};

// --- RCC_CR: Clock Control Register (0x4002_1000) ---
rw_reg!(
    RccCr,
    RccCrVal,
    u32,
    0x4002_1000,
    "RCC Clock Control Register"
);
#[derive(Copy, Clone)]
#[repr(transparent)]
pub struct RccCrVal(pub u32);
impl RccCrVal {
    rw_bool_field!(u32, hseon, 16, "HSE Enable");
    ro_bool_field!(u32, hserdy, 17, "HSE Ready");
    rw_bool_field!(u32, hsebyp, 18, "HSE Bypass");
    rw_bool_field!(u32, pllon, 24, "PLL Enable");
    ro_bool_field!(u32, pllrdy, 25, "PLL Ready");
}

// --- RCC_CFGR: Clock Configuration Register (0x4002_1004) ---
rw_reg!(
    RccCfgr,
    RccCfgrVal,
    u32,
    0x4002_1004,
    "RCC Clock Configuration Register"
);
#[derive(Copy, Clone)]
#[repr(transparent)]
pub struct RccCfgrVal(pub u32);
impl RccCfgrVal {
    rw_int_field!(u32, sw, 0, 1, u8, "System Clock Switch");
    ro_int_field!(u32, sws, 2, 3, u8, "System Clock Switch Status");
    rw_int_field!(u32, hpre, 4, 7, u8, "AHB Prescaler");
    rw_int_field!(u32, ppre1, 8, 10, u8, "APB1 Prescaler");
    rw_int_field!(u32, ppre2, 11, 13, u8, "APB2 Prescaler");
    rw_bool_field!(u32, pllsrc, 16, "PLL Source (HSE)");
    rw_int_field!(u32, pllmul, 18, 21, u8, "PLL Multiplication Factor");
}

// --- FLASH_ACR: Flash Access Control Register (0x4002_2000) ---
rw_reg!(
    FlashAcr,
    FlashAcrVal,
    u32,
    0x4002_2000,
    "Flash Access Control Register"
);
#[derive(Copy, Clone)]
#[repr(transparent)]
pub struct FlashAcrVal(pub u32);
impl FlashAcrVal {
    rw_int_field!(u32, latency, 0, 2, u8, "Flash Latency (wait states)");
}
