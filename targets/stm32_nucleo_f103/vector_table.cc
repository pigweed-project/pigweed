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
#include "pw_boot_cortex_m/boot.h"
#include "pw_memory/busy_wait_forever.h"
#include "pw_preprocessor/compiler.h"

namespace {

[[noreturn]] void DefaultFaultHandler() { pw::BusyWaitForever(); }

using InterruptHandler = void (*)();

}  // namespace

extern "C" {

// Cortex-M3 system exception handlers. FreeRTOS overrides SVC, PendSV, and
// SysTick when linked.
PW_WEAK void NMI_Handler() { DefaultFaultHandler(); }
PW_WEAK void HardFault_Handler() { DefaultFaultHandler(); }
PW_WEAK void MemManage_Handler() { DefaultFaultHandler(); }
PW_WEAK void BusFault_Handler() { DefaultFaultHandler(); }
PW_WEAK void UsageFault_Handler() { DefaultFaultHandler(); }
PW_WEAK void SVC_Handler() { DefaultFaultHandler(); }
PW_WEAK void DebugMon_Handler() { DefaultFaultHandler(); }
PW_WEAK void PendSV_Handler() { DefaultFaultHandler(); }
PW_WEAK void SysTick_Handler() { DefaultFaultHandler(); }

// STM32F103xB peripheral interrupt handlers (IRQs 0..42).
PW_WEAK void WWDG_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void PVD_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void TAMPER_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void RTC_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void FLASH_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void RCC_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void EXTI0_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void EXTI1_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void EXTI2_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void EXTI3_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void EXTI4_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void DMA1_Channel1_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void DMA1_Channel2_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void DMA1_Channel3_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void DMA1_Channel4_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void DMA1_Channel5_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void DMA1_Channel6_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void DMA1_Channel7_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void ADC1_2_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void USB_HP_CAN1_TX_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void USB_LP_CAN1_RX0_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void CAN1_RX1_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void CAN1_SCE_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void EXTI9_5_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void TIM1_BRK_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void TIM1_UP_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void TIM1_TRG_COM_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void TIM1_CC_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void TIM2_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void TIM3_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void TIM4_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void I2C1_EV_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void I2C1_ER_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void I2C2_EV_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void I2C2_ER_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void SPI1_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void SPI2_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void USART1_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void USART2_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void USART3_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void EXTI15_10_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void RTC_Alarm_IRQHandler() { DefaultFaultHandler(); }
PW_WEAK void USBWakeUp_IRQHandler() { DefaultFaultHandler(); }

// STM32F103xB interrupt vector table (RM0008 Table 63).
PW_KEEP_IN_SECTION(".vector_table")
const InterruptHandler vector_table[] = {
    // [0] Initial main stack pointer.
    reinterpret_cast<InterruptHandler>(&pw_boot_stack_high_addr),
    // [1] Reset handler.
    pw_boot_Entry,
    // [2] NMI.
    NMI_Handler,
    // [3] HardFault.
    HardFault_Handler,
    // [4] MemManage.
    MemManage_Handler,
    // [5] BusFault.
    BusFault_Handler,
    // [6] UsageFault.
    UsageFault_Handler,
    // [7..10] Reserved.
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    // [11] SVCall.
    SVC_Handler,
    // [12] DebugMonitor.
    DebugMon_Handler,
    // [13] Reserved.
    nullptr,
    // [14] PendSV.
    PendSV_Handler,
    // [15] SysTick.
    SysTick_Handler,

    // External Interrupts (IRQ 0..42):
    WWDG_IRQHandler,             // [16] IRQ 0
    PVD_IRQHandler,              // [17] IRQ 1
    TAMPER_IRQHandler,           // [18] IRQ 2
    RTC_IRQHandler,              // [19] IRQ 3
    FLASH_IRQHandler,            // [20] IRQ 4
    RCC_IRQHandler,              // [21] IRQ 5
    EXTI0_IRQHandler,            // [22] IRQ 6
    EXTI1_IRQHandler,            // [23] IRQ 7
    EXTI2_IRQHandler,            // [24] IRQ 8
    EXTI3_IRQHandler,            // [25] IRQ 9
    EXTI4_IRQHandler,            // [26] IRQ 10
    DMA1_Channel1_IRQHandler,    // [27] IRQ 11
    DMA1_Channel2_IRQHandler,    // [28] IRQ 12
    DMA1_Channel3_IRQHandler,    // [29] IRQ 13
    DMA1_Channel4_IRQHandler,    // [30] IRQ 14
    DMA1_Channel5_IRQHandler,    // [31] IRQ 15
    DMA1_Channel6_IRQHandler,    // [32] IRQ 16
    DMA1_Channel7_IRQHandler,    // [33] IRQ 17
    ADC1_2_IRQHandler,           // [34] IRQ 18
    USB_HP_CAN1_TX_IRQHandler,   // [35] IRQ 19
    USB_LP_CAN1_RX0_IRQHandler,  // [36] IRQ 20
    CAN1_RX1_IRQHandler,         // [37] IRQ 21
    CAN1_SCE_IRQHandler,         // [38] IRQ 22
    EXTI9_5_IRQHandler,          // [39] IRQ 23
    TIM1_BRK_IRQHandler,         // [40] IRQ 24
    TIM1_UP_IRQHandler,          // [41] IRQ 25
    TIM1_TRG_COM_IRQHandler,     // [42] IRQ 26
    TIM1_CC_IRQHandler,          // [43] IRQ 27
    TIM2_IRQHandler,             // [44] IRQ 28
    TIM3_IRQHandler,             // [45] IRQ 29
    TIM4_IRQHandler,             // [46] IRQ 30
    I2C1_EV_IRQHandler,          // [47] IRQ 31
    I2C1_ER_IRQHandler,          // [48] IRQ 32
    I2C2_EV_IRQHandler,          // [49] IRQ 33
    I2C2_ER_IRQHandler,          // [50] IRQ 34
    SPI1_IRQHandler,             // [51] IRQ 35
    SPI2_IRQHandler,             // [52] IRQ 36
    USART1_IRQHandler,           // [53] IRQ 37
    USART2_IRQHandler,           // [54] IRQ 38
    USART3_IRQHandler,           // [55] IRQ 39
    EXTI15_10_IRQHandler,        // [56] IRQ 40
    RTC_Alarm_IRQHandler,        // [57] IRQ 41
    USBWakeUp_IRQHandler,        // [58] IRQ 42
};

}  // extern "C"
