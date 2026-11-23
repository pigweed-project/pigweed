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

#include "pw_boot/boot.h"

#include <cstdint>

#include "pw_boot_cortex_m/boot.h"
#include "pw_malloc/malloc.h"
#include "pw_memory/busy_wait_forever.h"
#include "pw_preprocessor/compiler.h"
#include "stm32_nucleo_f103/init.h"

extern "C" {

extern uint32_t SystemCoreClock;
uint32_t SystemCoreClock = 8000000u;

}  // extern "C"

namespace {

// Flash Access Control Register (RM0008 Section 3.3.3).
constexpr uintptr_t kFlashAcrAddr = 0x40022000u;
constexpr uint32_t kFlashAcrPrftbe = 1u << 4;
constexpr uint32_t kFlashAcrLatencyMask = 0x7u;
constexpr uint32_t kFlashAcrLatency2Ws = 0x2u;  // 2 wait states for 48..72 MHz.

struct RccRegs {
  volatile uint32_t cr;
  volatile uint32_t cfgr;
  volatile uint32_t cir;
  volatile uint32_t apb2rstr;
  volatile uint32_t apb1rstr;
  volatile uint32_t ahbenr;
  volatile uint32_t apb2enr;
  volatile uint32_t apb1enr;
  volatile uint32_t bdcr;
  volatile uint32_t csr;
};

constexpr uintptr_t kRccBase = 0x40021000u;

// RCC_CR bit definitions (RM0008 Section 7.3.1).
constexpr uint32_t kRccCrHsion = 1u << 0;
constexpr uint32_t kRccCrHsirdy = 1u << 1;
constexpr uint32_t kRccCrHseon = 1u << 16;
constexpr uint32_t kRccCrHserdy = 1u << 17;
constexpr uint32_t kRccCrHsebyp = 1u << 18;
constexpr uint32_t kRccCrPllon = 1u << 24;
constexpr uint32_t kRccCrPllrdy = 1u << 25;

// RCC_CFGR bit definitions (RM0008 Section 7.3.2).
constexpr uint32_t kRccCfgrSwMask = 0x3u;
constexpr uint32_t kRccCfgrSwHsi = 0x0u;
constexpr uint32_t kRccCfgrSwPll = 0x2u;
constexpr uint32_t kRccCfgrSwsMask = 0x3u << 2;
constexpr uint32_t kRccCfgrSwsHsi = kRccCfgrSwHsi << 2;
constexpr uint32_t kRccCfgrSwsPll = 0x2u << 2;
constexpr uint32_t kRccCfgrHpreDiv1 = 0x0u << 4;
constexpr uint32_t kRccCfgrPpre1Div2 = 0x4u << 8;
constexpr uint32_t kRccCfgrPpre2Div1 = 0x0u << 11;
constexpr uint32_t kRccCfgrPllsrcHse = 1u << 16;
constexpr uint32_t kRccCfgrPllmul9 = 0x7u << 18;
constexpr uint32_t kRccCfgrPllmul16 = 0xEu << 18;

// RCC_APB2ENR & AFIO_MAPR definitions (RM0008 Sections 7.3.7 & 9.4.2).
constexpr uint32_t kRccApb2EnrAfioen = 1u << 0;
constexpr uintptr_t kAfioMaprAddr = 0x40010004u;
constexpr uint32_t kAfioMaprSwjCfgMask = 0x7u << 24;
// SWJ_CFG = 0b010 disables 5-wire JTAG-DP while keeping 2-wire SW-DP active.
constexpr uint32_t kAfioMaprSwjCfgJtagDisable = 0x2u << 24;

// ARMv7-M System Control Block registers (ARM DDI 0403E Section B3.2).
constexpr uintptr_t kScbVtorAddr = 0xE000ED08u;
constexpr uintptr_t kScbAircrAddr = 0xE000ED0Cu;
constexpr uint32_t kScbAircrVectkey = 0x5FAu << 16;
// PRIGROUP = 3 assigns all 4 STM32 priority bits to preemption priority (0
// subpriority bits), as required by the FreeRTOS Cortex-M3 port.
constexpr uint32_t kScbAircrPrigroup4 = 0x3u << 8;

// Bounded poll iteration budget (~tens of milliseconds at 8 MHz HSI) so missing
// external clocks fall back cleanly rather than wedging boot.
constexpr uint32_t kClockLockTimeout = 100000u;
constexpr uint32_t kHsiClockHz = 8000000u;
constexpr uint32_t kHseSysclkHz = 72000000u;
constexpr uint32_t kHsiPllSysclkHz = 64000000u;

void ConfigureSystemClock() {
  auto& rcc = *reinterpret_cast<RccRegs*>(kRccBase);
  auto& flash_acr = *reinterpret_cast<volatile uint32_t*>(kFlashAcrAddr);

  // Ensure HSI is running and switch to HSI before reconfiguring the PLL.
  rcc.cr |= kRccCrHsion;
  for (uint32_t i = 0; i < kClockLockTimeout && !(rcc.cr & kRccCrHsirdy); ++i) {
  }
  if ((rcc.cr & kRccCrHsirdy) == 0u) {
    return;
  }
  rcc.cfgr &= ~kRccCfgrSwMask;
  for (uint32_t i = 0;
       i < kClockLockTimeout && (rcc.cfgr & kRccCfgrSwsMask) != kRccCfgrSwsHsi;
       ++i) {
  }
  if ((rcc.cfgr & kRccCfgrSwsMask) != kRccCfgrSwsHsi) {
    return;
  }

  // Disable PLL and HSE first and wait for PLLRDY and HSERDY to clear before
  // modifying HSEBYP or PLL multipliers (RM0008 Sections 7.2.2 & 7.3.1).
  rcc.cr &= ~(kRccCrPllon | kRccCrHseon);
  for (uint32_t i = 0;
       i < kClockLockTimeout && (rcc.cr & (kRccCrPllrdy | kRccCrHserdy)) != 0u;
       ++i) {
  }
  rcc.cr &= ~kRccCrHsebyp;
  rcc.cfgr = 0u;

  // Enable Flash prefetch and 2 wait states for 64/72 MHz operation.
  flash_acr = (flash_acr & ~kFlashAcrLatencyMask) | kFlashAcrPrftbe |
              kFlashAcrLatency2Ws;

  // NUCLEO-F103RB feeds an 8 MHz clock from ST-LINK MCO into OSC_IN (HSE
  // bypass mode). Fall back to HSI/2 * 16 = 64 MHz if HSE is unavailable.
  rcc.cr |= kRccCrHsebyp;
  rcc.cr |= kRccCrHseon;
  bool hse_ready = false;
  for (uint32_t i = 0; i < kClockLockTimeout; ++i) {
    if ((rcc.cr & kRccCrHserdy) != 0u) {
      hse_ready = true;
      break;
    }
  }

  if (hse_ready) {
    // 8 MHz HSE * 9 = 72 MHz SYSCLK, HCLK = 72 MHz, PCLK2 = 72 MHz,
    // PCLK1 = 36 MHz.
    rcc.cfgr = kRccCfgrHpreDiv1 | kRccCfgrPpre2Div1 | kRccCfgrPpre1Div2 |
               kRccCfgrPllsrcHse | kRccCfgrPllmul9;
    SystemCoreClock = kHseSysclkHz;
  } else {
    rcc.cr &= ~kRccCrHseon;
    for (uint32_t i = 0; i < kClockLockTimeout && (rcc.cr & kRccCrHserdy) != 0u;
         ++i) {
    }
    rcc.cr &= ~kRccCrHsebyp;
    // (8 MHz HSI / 2) * 16 = 64 MHz SYSCLK, PCLK2 = 64 MHz, PCLK1 = 32 MHz.
    rcc.cfgr = kRccCfgrHpreDiv1 | kRccCfgrPpre2Div1 | kRccCfgrPpre1Div2 |
               kRccCfgrPllmul16;
    SystemCoreClock = kHsiPllSysclkHz;
  }

  rcc.cr |= kRccCrPllon;
  for (uint32_t i = 0; i < kClockLockTimeout && !(rcc.cr & kRccCrPllrdy); ++i) {
  }

  if ((rcc.cr & kRccCrPllrdy) != 0u) {
    rcc.cfgr = (rcc.cfgr & ~kRccCfgrSwMask) | kRccCfgrSwPll;
    for (uint32_t i = 0; i < kClockLockTimeout &&
                         (rcc.cfgr & kRccCfgrSwsMask) != kRccCfgrSwsPll;
         ++i) {
    }
  }
  if ((rcc.cfgr & kRccCfgrSwsMask) != kRccCfgrSwsPll) {
    rcc.cfgr = 0u;
    rcc.cr &= ~kRccCrPllon;
    SystemCoreClock = kHsiClockHz;
  }
}

uint32_t ComputeAhbClockHz() {
  const auto& rcc = *reinterpret_cast<const RccRegs*>(kRccBase);
  const uint32_t hpre = (rcc.cfgr >> 4) & 0xFu;
  if ((hpre & 0x8u) == 0u) {
    return SystemCoreClock;
  }
  static constexpr uint8_t kAhbShifts[8] = {1, 2, 3, 4, 6, 7, 8, 9};
  return SystemCoreClock >> kAhbShifts[hpre & 0x7u];
}

}  // namespace

namespace pw::stm32_nucleo_f103 {

uint32_t GetApb1ClockHz() {
  const auto& rcc = *reinterpret_cast<const RccRegs*>(kRccBase);
  const uint32_t hclk = ComputeAhbClockHz();
  const uint32_t ppre1 = (rcc.cfgr >> 8) & 0x7u;
  if ((ppre1 & 0x4u) == 0u) {
    return hclk;
  }
  return hclk >> ((ppre1 & 0x3u) + 1u);
}

}  // namespace pw::stm32_nucleo_f103

extern "C" void pw_boot_PreStaticMemoryInit() {
  // Explicitly point SCB->VTOR at the linked vector table.
  volatile uint32_t& scb_vtor =
      *reinterpret_cast<volatile uint32_t*>(kScbVtorAddr);
  scb_vtor = reinterpret_cast<uint32_t>(&pw_boot_vector_table_addr);
}

extern "C" void pw_boot_PreStaticConstructorInit() {
  volatile uint32_t& scb_aircr =
      *reinterpret_cast<volatile uint32_t*>(kScbAircrAddr);
  scb_aircr = kScbAircrVectkey | kScbAircrPrigroup4;

  ConfigureSystemClock();

  // Enable AFIO clock and set SWJ_CFG = 0b010 (JTAG-DP disabled, SW-DP enabled)
  // so JTAG pins PA15, PB3, and PB4 are available as standard GPIOs while
  // keeping 2-wire SWD debugging active on PA13/PA14.
  auto& rcc = *reinterpret_cast<RccRegs*>(kRccBase);
  volatile uint32_t& afio_mapr =
      *reinterpret_cast<volatile uint32_t*>(kAfioMaprAddr);
  rcc.apb2enr |= kRccApb2EnrAfioen;
  // Errata ES096: read back RCC register to ensure clock enable propagates
  // across the AHB/APB bridge before accessing AFIO.
  [[maybe_unused]] const uint32_t apb2enr_sync = rcc.apb2enr;
  afio_mapr = (afio_mapr & ~kAfioMaprSwjCfgMask) | kAfioMaprSwjCfgJtagDisable;

#if PW_MALLOC_ACTIVE
  pw_MallocInit(&pw_boot_heap_low_addr, &pw_boot_heap_high_addr);
#endif  // PW_MALLOC_ACTIVE

  pw::stm32_nucleo_f103::SysIoInit();
}

extern "C" void pw_boot_PreMainInit() {}

extern "C" PW_NO_RETURN void pw_boot_PostMain() { pw::BusyWaitForever(); }
