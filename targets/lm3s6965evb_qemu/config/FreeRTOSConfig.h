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
#pragma once

#include <stdint.h>

#define configSUPPORT_DYNAMIC_ALLOCATION 1
#define configSUPPORT_STATIC_ALLOCATION 1
#define configTOTAL_HEAP_SIZE ((size_t)(24 * 1024))

#define configUSE_16_BIT_TICKS 0
#define configUSE_CO_ROUTINES 0
#define configUSE_IDLE_HOOK 0
#define configUSE_MUTEXES 1
#define configUSE_RECURSIVE_MUTEXES 1
#define configUSE_COUNTING_SEMAPHORES 1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1
#define configUSE_PREEMPTION 1
#define configUSE_TICK_HOOK 0
#define configUSE_TIMERS 1
#define configUSE_TRACE_FACILITY 1
#define configUSE_MALLOC_FAILED_HOOK 1

#define configCHECK_FOR_STACK_OVERFLOW 2
#define configCPU_CLOCK_HZ (12000000UL)
#define configENABLE_BACKWARD_COMPATIBILITY 0
#define configMAX_PRIORITIES (7)
#define configMAX_TASK_NAME_LEN (16)
#define configMESSAGE_BUFFER_LENGTH_TYPE size_t
#define configMINIMAL_STACK_SIZE ((uint16_t)128)
#define configQUEUE_REGISTRY_SIZE 8
#define configRECORD_STACK_HIGH_ADDRESS 1
#define configTICK_RATE_HZ ((TickType_t)1000)
#define configTIMER_QUEUE_LENGTH 10
#define configTIMER_TASK_PRIORITY (configMAX_PRIORITIES - 1)
#define configTIMER_TASK_STACK_DEPTH 256

/* Stellaris LM3S implements 3 bits of interrupt priority (bits 7:5) */
#define configPRIO_BITS 3

#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY 7
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5
#define configKERNEL_INTERRUPT_PRIORITY \
  (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
  (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

#define INCLUDE_uxTaskPriorityGet 1
#define INCLUDE_vTaskCleanUpResources 0
#define INCLUDE_vTaskDelay 1
#define INCLUDE_vTaskDelayUntil 1
#define INCLUDE_vTaskDelete 1
#define INCLUDE_vTaskPrioritySet 1
#define INCLUDE_vTaskSuspend 1
#define INCLUDE_xTaskGetSchedulerState 1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#define INCLUDE_xTimerPendFunctionCall 1

#include "pw_third_party/freertos/config_assert.h"

#define vPortSVCHandler SVC_Handler
#define xPortPendSVHandler PendSV_Handler
#define xPortSysTickHandler SysTick_Handler
