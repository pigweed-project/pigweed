// Copyright 2021 The Pigweed Authors
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

#pragma once

#include <cstdint>

#include "pw_preprocessor/arch.h"

namespace pw::cpu_exception::cortex_m {

// CMSIS/Cortex-M/ARMv7 related constants.
// These values are from the ARMv7-M Architecture Reference Manual DDI 0403E.b.
// https://static.docs.arm.com/ddi0403/e/DDI0403E_B_armv7m_arm.pdf

inline constexpr uint32_t kThreadModeIsrNum = 0x0;
inline constexpr uint32_t kNmiIsrNum = 0x2;
inline constexpr uint32_t kHardFaultIsrNum = 0x3;
inline constexpr uint32_t kMemFaultIsrNum = 0x4;
inline constexpr uint32_t kBusFaultIsrNum = 0x5;
inline constexpr uint32_t kUsageFaultIsrNum = 0x6;
inline constexpr uint32_t kDebugMonIsrNum = 0xC;

// Masks for Interrupt Control and State Register ICSR (ARMv7-M Section B3.2.4)
inline constexpr uint32_t kIcsrVectactiveMask = (1 << 9) - 1;

// Masks for individual bits of HFSR. (ARMv7-M Section B3.2.16)
inline constexpr uint32_t kHfsrForcedMask = (0x1u << 30);
inline constexpr uint32_t kHfsrDebugEvtMask = (0x1u << 31);

// Masks for different sections of CFSR. (ARMv7-M Section B3.2.15)
inline constexpr uint32_t kCfsrMemFaultMask = 0x000000ff;
inline constexpr uint32_t kCfsrBusFaultMask = 0x0000ff00;
inline constexpr uint32_t kCfsrUsageFaultMask = 0xffff0000;

// Masks for individual bits of CFSR. (ARMv7-M Section B3.2.15)
// Memory faults (MemManage Status Register)
inline constexpr uint32_t kCfsrMemFaultStart = (0x1);
inline constexpr uint32_t kCfsrIaccviolMask = (kCfsrMemFaultStart << 0);
inline constexpr uint32_t kCfsrDaccviolMask = (kCfsrMemFaultStart << 1);
inline constexpr uint32_t kCfsrMunstkerrMask = (kCfsrMemFaultStart << 3);
inline constexpr uint32_t kCfsrMstkerrMask = (kCfsrMemFaultStart << 4);
inline constexpr uint32_t kCfsrMlsperrMask = (kCfsrMemFaultStart << 5);
inline constexpr uint32_t kCfsrMmarvalidMask = (kCfsrMemFaultStart << 7);
// Mask for all supported memory error flags i.e. bits that indicate a specific
// error.
inline constexpr uint32_t kCfsrMemAllErrorsMask =
    kCfsrIaccviolMask | kCfsrDaccviolMask | kCfsrMunstkerrMask |
    kCfsrMstkerrMask | kCfsrMlsperrMask;
// Bus faults (BusFault Status Register)
inline constexpr uint32_t kCfsrBusFaultStart = (0x1 << 8);
inline constexpr uint32_t kCfsrIbuserrMask = (kCfsrBusFaultStart << 0);
inline constexpr uint32_t kCfsrPreciserrMask = (kCfsrBusFaultStart << 1);
inline constexpr uint32_t kCfsrImpreciserrMask = (kCfsrBusFaultStart << 2);
inline constexpr uint32_t kCfsrUnstkerrMask = (kCfsrBusFaultStart << 3);
inline constexpr uint32_t kCfsrStkerrMask = (kCfsrBusFaultStart << 4);
inline constexpr uint32_t kCfsrLsperrMask = (kCfsrBusFaultStart << 5);
inline constexpr uint32_t kCfsrBfarvalidMask = (kCfsrBusFaultStart << 7);
// Mask for all supported bus error flags i.e. bits that indicate a specific
// error.
inline constexpr uint32_t kCfsrBusAllErrorsMask =
    kCfsrIbuserrMask | kCfsrPreciserrMask | kCfsrImpreciserrMask |
    kCfsrUnstkerrMask | kCfsrStkerrMask | kCfsrLsperrMask;
// Usage faults (UsageFault Status Register)
inline constexpr uint32_t kCfsrUsageFaultStart = (0x1 << 16);
inline constexpr uint32_t kCfsrUndefinstrMask = (kCfsrUsageFaultStart << 0);
inline constexpr uint32_t kCfsrInvstateMask = (kCfsrUsageFaultStart << 1);
inline constexpr uint32_t kCfsrInvpcMask = (kCfsrUsageFaultStart << 2);
inline constexpr uint32_t kCfsrNocpMask = (kCfsrUsageFaultStart << 3);
#if _PW_ARCH_ARM_V8M_MAINLINE || _PW_ARCH_ARM_V8_1M_MAINLINE
inline constexpr uint32_t kCfsrStkofMask = (kCfsrUsageFaultStart << 4);
#endif  // _PW_ARCH_ARM_V8M_MAINLINE || _PW_ARCH_ARM_V8_1M_MAINLINE
inline constexpr uint32_t kCfsrUnalignedMask = (kCfsrUsageFaultStart << 8);
inline constexpr uint32_t kCfsrDivbyzeroMask = (kCfsrUsageFaultStart << 9);
// Mask for all supported usage error flags i.e. bits that indicate a specific
// error.
inline constexpr uint32_t kCfsrUsageAllErrorsMask =
    kCfsrUndefinstrMask | kCfsrInvstateMask | kCfsrInvpcMask | kCfsrNocpMask |
#if _PW_ARCH_ARM_V8M_MAINLINE || _PW_ARCH_ARM_V8_1M_MAINLINE
    kCfsrStkofMask |
#endif  // _PW_ARCH_ARM_V8M_MAINLINE || _PW_ARCH_ARM_V8_1M_MAINLINE
    kCfsrUnalignedMask | kCfsrDivbyzeroMask;

// Bit masks for an exception return value. (ARMv7-M Section B1.5.8)
inline constexpr uint32_t kExcReturnStackMask = 0x1u << 2;  // 0=MSP, 1=PSP
// 0=Handler, 1=Thread
inline constexpr uint32_t kExcReturnModeMask = 0x1u << 3;
inline constexpr uint32_t kExcReturnBasicFrameMask = 0x1u << 4;

// Mask for the IPSR, bits 8:0, of the xPSR register.
inline constexpr uint32_t kXpsrIpsrMask = 0b1'1111'1111;

// Bit masks for the control register. (ARMv7-M Section B1.4.4)
// The SPSEL bit is only valid while in Thread Mode:
// 0=MSP, 1=PSP
inline constexpr uint32_t kControlThreadModeStackMask = 0x1u << 1;

// Memory mapped registers. (ARMv7-M Section B3.2.2, Table B3-4)
// Note: Only some of these are supported on ARMv6-M.
inline volatile uint32_t& cortex_m_cfsr() {
  return *reinterpret_cast<volatile uint32_t*>(0xE000ED28u);
}
inline volatile uint32_t& cortex_m_mmfar() {
  return *reinterpret_cast<volatile uint32_t*>(0xE000ED34u);
}
inline volatile uint32_t& cortex_m_bfar() {
  return *reinterpret_cast<volatile uint32_t*>(0xE000ED38u);
}
inline volatile uint32_t& cortex_m_icsr() {
  return *reinterpret_cast<volatile uint32_t*>(0xE000ED04u);
}
inline volatile uint32_t& cortex_m_hfsr() {
  return *reinterpret_cast<volatile uint32_t*>(0xE000ED2Cu);
}
inline volatile uint32_t& cortex_m_shcsr() {
  return *reinterpret_cast<volatile uint32_t*>(0xE000ED24u);
}

}  // namespace pw::cpu_exception::cortex_m
