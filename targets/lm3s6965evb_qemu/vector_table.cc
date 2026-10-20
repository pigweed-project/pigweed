// Copyright 2020 The Pigweed Authors
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

#include <cstdint>

#include "pw_boot/boot.h"
#include "pw_boot_cortex_m/boot.h"
#include "pw_memory/busy_wait_forever.h"
#include "pw_preprocessor/compiler.h"

namespace {

// Default handler to insert into the ARMv7-M vector table (below).
// This function exists for convenience. If a device isn't doing what you
// expect, it might have hit a fault and ended up here.
[[noreturn]] void DefaultFaultHandler() {
  // Tell QEMU to shut down immediately on fault instead of hanging.
  volatile uint32_t* aircr = reinterpret_cast<uint32_t*>(0xE000ED0CU);
  *aircr = 0x5fa0004;
  // Wait for debugger to attach if not running in QEMU.
  pw::BusyWaitForever();
}

// This typedef is for convenience when building the vector table. With the
// exception of SP_main (0th entry in the vector table), all the entries of the
// vector table are function pointers.
using InterruptHandler = void (*)();

}  // namespace

extern "C" {

// Interrupt handlers critical for OS operation.
// Default weak implementations fall back to DefaultFaultHandler if no OS is
// linked. When FreeRTOS is linked, its port handlers override these symbols.
PW_WEAK void SVC_Handler() { DefaultFaultHandler(); }
PW_WEAK void DebugMon_Handler() { DefaultFaultHandler(); }
PW_WEAK void PendSV_Handler() { DefaultFaultHandler(); }
PW_WEAK void SysTick_Handler() { DefaultFaultHandler(); }

// This is the device's interrupt vector table. It's not referenced in any
// code because the platform (STM32F4xx) expects this table to be present at the
// beginning of flash. The exact address is specified in the pw_boot_cortex_m
// configuration as part of the target config.
//
// For more information, see ARMv7-M Architecture Reference Manual DDI 0403E.b
// section B1.5.3.
PW_KEEP_IN_SECTION(".vector_table")
const InterruptHandler vector_table[] = {
    // [0] The starting location of the stack pointer.
    // This address is NOT an interrupt handler/function pointer, it is simply
    // the address that the main stack pointer should be initialized to. The
    // value is reinterpret casted because it needs to be in the vector table.
    reinterpret_cast<InterruptHandler>(&pw_boot_stack_high_addr),

    // [1] Reset handler, dictates how to handle reset interrupt. This is the
    // address that the Program Counter (PC) is initialized to at boot.
    pw_boot_Entry,

    // [2] NMI handler.
    DefaultFaultHandler,
    // [3] HardFault handler.
    DefaultFaultHandler,
    // [4] MemManage fault handler.
    DefaultFaultHandler,
    // [5] BusFault handler.
    DefaultFaultHandler,
    // [6] UsageFault handler.
    DefaultFaultHandler,
    // [7-10] Reserved.
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    // [11] SVCall handler.
    SVC_Handler,
    // [12] DebugMon handler.
    DebugMon_Handler,
    // [13] Reserved.
    nullptr,
    // [14] PendSV handler.
    PendSV_Handler,
    // [15] SysTick handler.
    SysTick_Handler,
};

}  // extern "C"
