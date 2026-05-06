// Copyright 2023 The Pigweed Authors
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

#include <stdbool.h>

#include "pw_boot/boot.h"
#include "pw_boot_cortex_m/boot.h"
#include "pw_memory/busy_wait_forever.h"
#include "pw_preprocessor/compiler.h"

// Default handler to insert into the ARMv7-M vector table (below).
// This function exists for convenience. If a device isn't doing what you
// expect, it might have hit a fault and ended up here.
static void DefaultFaultHandler(void) {
  // Wait for debugger to attach.
  pw_BusyWaitForever();
}

// This typedef is for convenience when building the vector table. With the
// exception of SP_main (0th entry in the vector table), all the entries of the
// vector table are function pointers.
typedef void (*InterruptHandler)(void);

void SVC_Handler(void) PW_ALIAS(DefaultFaultHandler);
void PendSV_Handler(void) PW_ALIAS(DefaultFaultHandler);
void SysTick_Handler(void) PW_ALIAS(DefaultFaultHandler);

void MemManage_Handler(void) PW_ALIAS(DefaultFaultHandler);
void BusFault_Handler(void) PW_ALIAS(DefaultFaultHandler);
void UsageFault_Handler(void) PW_ALIAS(DefaultFaultHandler);
void DebugMon_Handler(void) PW_ALIAS(DefaultFaultHandler);

void am_brownout_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_watchdog_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_rtc_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_vcomp_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_ioslave_ios_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_ioslave_acc_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_iomaster0_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_iomaster1_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_iomaster2_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_iomaster3_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_iomaster4_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_iomaster5_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_iomaster6_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_iomaster7_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_ctimer_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_uart_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_uart1_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_uart2_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_uart3_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_adc_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_mspi0_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_mspi1_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_mspi2_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_clkgen_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_cryptosec_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_sdio_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_usb_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_gpu_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_disp_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_dsi_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_stimer_cmpr0_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_stimer_cmpr1_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_stimer_cmpr2_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_stimer_cmpr3_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_stimer_cmpr4_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_stimer_cmpr5_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_stimer_cmpr6_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_stimer_cmpr7_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_stimerof_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_audadc0_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_dspi2s0_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_dspi2s1_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_dspi2s2_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_dspi2s3_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_pdm0_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_pdm1_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_pdm2_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_pdm3_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_gpio0_001f_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_gpio0_203f_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_gpio0_405f_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_gpio0_607f_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_gpio1_001f_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_gpio1_203f_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_gpio1_405f_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_gpio1_607f_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_timer00_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_timer01_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_timer02_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_timer03_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_timer04_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_timer05_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_timer06_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_timer07_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_timer08_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_timer09_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_timer10_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_timer11_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_timer12_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_timer13_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_timer14_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_timer15_isr(void) PW_ALIAS(DefaultFaultHandler);
void am_cachecpu_isr(void) PW_ALIAS(DefaultFaultHandler);

PW_KEEP_IN_SECTION(".vector_table")
const InterruptHandler vector_table[] = {
    // Cortex-M CPU specific interrupt handlers.
    (InterruptHandler)(&pw_boot_stack_high_addr),
    pw_boot_Entry,        // The reset handler
    DefaultFaultHandler,  // The NMI handler
    DefaultFaultHandler,  // The hard fault handler
    DefaultFaultHandler,  // The MemManage_Handler
    DefaultFaultHandler,  // The BusFault_Handler
    DefaultFaultHandler,  // The UsageFault_Handler
    0,                    // Reserved
    0,                    // Reserved
    0,                    // Reserved
    0,                    // Reserved
    SVC_Handler,          // SVCall handler
    DefaultFaultHandler,  // Debug monitor handler
    0,                    // Reserved
    PendSV_Handler,       // The PendSV handler
    SysTick_Handler,      // The SysTick handler
    // Vendor specific peripheral interrupt handlers.
    am_brownout_isr,      //  0: Brownout (rstgen)
    am_watchdog_isr,      //  1: Watchdog (WDT)
    am_rtc_isr,           //  2: RTC
    am_vcomp_isr,         //  3: Voltage Comparator
    am_ioslave_ios_isr,   //  4: I/O Responder general
    am_ioslave_acc_isr,   //  5: I/O Responder access
    am_iomaster0_isr,     //  6: I/O Controller 0
    am_iomaster1_isr,     //  7: I/O Controller 1
    am_iomaster2_isr,     //  8: I/O Controller 2
    am_iomaster3_isr,     //  9: I/O Controller 3
    am_iomaster4_isr,     // 10: I/O Controller 4
    am_iomaster5_isr,     // 11: I/O Controller 5
    am_iomaster6_isr,     // 12: I/O Controller 6 (I3C/I2C/SPI)
    am_iomaster7_isr,     // 13: I/O Controller 7 (I3C/I2C/SPI)
    am_ctimer_isr,        // 14: OR of all timerX interrupts
    am_uart_isr,          // 15: UART0
    am_uart1_isr,         // 16: UART1
    am_uart2_isr,         // 17: UART2
    am_uart3_isr,         // 18: UART3
    am_adc_isr,           // 19: ADC
    am_mspi0_isr,         // 20: MSPI0
    am_mspi1_isr,         // 21: MSPI1
    am_mspi2_isr,         // 22: MSPI2
    am_clkgen_isr,        // 23: ClkGen
    am_cryptosec_isr,     // 24: Crypto Secure
    DefaultFaultHandler,  // 25: Reserved
    am_sdio_isr,          // 26: SDIO
    am_usb_isr,           // 27: USB
    am_gpu_isr,           // 28: GPU
    am_disp_isr,          // 29: DISP
    am_dsi_isr,           // 30: DSI
    DefaultFaultHandler,  // 31: Reserved
    am_stimer_cmpr0_isr,  // 32: System Timer Compare0
    am_stimer_cmpr1_isr,  // 33: System Timer Compare1
    am_stimer_cmpr2_isr,  // 34: System Timer Compare2
    am_stimer_cmpr3_isr,  // 35: System Timer Compare3
    am_stimer_cmpr4_isr,  // 36: System Timer Compare4
    am_stimer_cmpr5_isr,  // 37: System Timer Compare5
    am_stimer_cmpr6_isr,  // 38: System Timer Compare6
    am_stimer_cmpr7_isr,  // 39: System Timer Compare7
    am_stimerof_isr,      // 40: System Timer Cap Overflow
    DefaultFaultHandler,  // 41: Reserved
    am_audadc0_isr,       // 42: Audio ADC
    DefaultFaultHandler,  // 43: Reserved
    am_dspi2s0_isr,       // 44: I2S0
    am_dspi2s1_isr,       // 45: I2S1
    am_dspi2s2_isr,       // 46: I2S2
    am_dspi2s3_isr,       // 47: I2S3
    am_pdm0_isr,          // 48: PDM0
    am_pdm1_isr,          // 49: PDM1
    am_pdm2_isr,          // 50: PDM2
    am_pdm3_isr,          // 51: PDM3
    DefaultFaultHandler,  // 52: Reserved
    DefaultFaultHandler,  // 53: Reserved
    DefaultFaultHandler,  // 54: Reserved
    DefaultFaultHandler,  // 55: Reserved
    am_gpio0_001f_isr,    // 56: GPIO N0 pins  0-31
    am_gpio0_203f_isr,    // 57: GPIO N0 pins 32-63
    am_gpio0_405f_isr,    // 58: GPIO N0 pins 64-95
    am_gpio0_607f_isr,    // 59: GPIO N0 pins 96-104, virtual 105-127
    am_gpio1_001f_isr,    // 60: GPIO N1 pins  0-31
    am_gpio1_203f_isr,    // 61: GPIO N1 pins 32-63
    am_gpio1_405f_isr,    // 62: GPIO N1 pins 64-95
    am_gpio1_607f_isr,    // 63: GPIO N1 pins 96-104, virtual 105-127
    DefaultFaultHandler,  // 64: Reserved
    DefaultFaultHandler,  // 65: Reserved
    DefaultFaultHandler,  // 66: Reserved
    am_timer00_isr,       // 67: timer0
    am_timer01_isr,       // 68: timer1
    am_timer02_isr,       // 69: timer2
    am_timer03_isr,       // 70: timer3
    am_timer04_isr,       // 71: timer4
    am_timer05_isr,       // 72: timer5
    am_timer06_isr,       // 73: timer6
    am_timer07_isr,       // 74: timer7
    am_timer08_isr,       // 75: timer8
    am_timer09_isr,       // 76: timer9
    am_timer10_isr,       // 77: timer10
    am_timer11_isr,       // 78: timer11
    am_timer12_isr,       // 79: timer12
    am_timer13_isr,       // 80: timer13
    am_timer14_isr,       // 81: timer14
    am_timer15_isr,       // 82: timer15
    am_cachecpu_isr       // 83: CPU cache
};
