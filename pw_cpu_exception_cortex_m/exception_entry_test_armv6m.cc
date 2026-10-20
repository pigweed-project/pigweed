// Copyright 2024 The Pigweed Authors
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

#include "pw_cpu_exception/entry.h"
#include "pw_cpu_exception/handler.h"
#include "pw_cpu_exception/support.h"
#include "pw_cpu_exception_cortex_m/exception_entry_test_util.h"
#include "pw_cpu_exception_cortex_m_private/cortex_m_constants.h"
#include "pw_unit_test/framework.h"

namespace pw::cpu_exception::cortex_m {
namespace {

using pw::cpu_exception::RawFaultingCpuState;

// CMSIS/Cortex-M/ARMv6 related constants.
// These values are from the ARMv6-M Architecture Reference Manual DDI 0419
// https://developer.arm.com/documentation/ddi0419/latest

// Memory mapped registers. (ARMv6-M Section B3.2.2, Table B3-4)
volatile uint32_t& cortex_m_ccr =
    *reinterpret_cast<volatile uint32_t*>(0xE000ED14u);

// Counter that is incremented if the test's exception handler correctly handles
// a triggered exception.
size_t exceptions_handled = 0;

// Allow up to kMaxFaultDepth faults before determining the device is
// unrecoverable.
constexpr size_t kMaxFaultDepth = 1;

// The instruction size in bytes on the ARMv6-M architecture.
constexpr size_t kMaxInstructionSize = 4;

// The manually captured PC won't be the exact same as the faulting PC. This is
// the maximum tolerated distance between the two to allow the test to pass.
constexpr uint32_t kMaxPcDistance = 6;

// Variable to prevent more than kMaxFaultDepth nested crashes.
size_t current_fault_depth = 0;

// Faulting pw_cpu_exception_State is copied here so values can be validated
// after exiting exception handler.
pw_cpu_exception_State captured_states[kMaxFaultDepth] = {};
pw_cpu_exception_State& captured_state = captured_states[0];

// Flag used to check if the contents of span matches the captured state.
bool span_matches = false;
// Forward declaration of the exception handler.
void TestingExceptionHandler(pw_cpu_exception_State*);

// Populate the device's registers with testable values, then trigger exception.
void BeginBaseFaultTest() {
  // Make sure divide by zero causes a fault.
  uint32_t magic = kMagicPattern;
  asm volatile(
      " mov r0, %[magic]                                      \n"
      " movs r1, #0                                           \n"
      " mov r2, pc                                            \n"
      " mov r3, lr                                            \n"
      // This instruction reads a bad address.
      " ldr r1, [r0]                                          \n"
      " nop                                                   \n"
      " nop                                                   \n"
      // clang-format off
      : /*output=*/
      : /*input=*/[magic]"r"(magic)
      : /*clobbers=*/"r0", "r1", "r2", "r3"
      // clang-format on
  );

  // Check that the stack align bit was not set.
  EXPECT_EQ(captured_state.base.psr & kPsrExtraStackAlignBit, 0u);
}

// Populate the device's registers with testable values, then trigger exception.
// This version causes stack to not be 4-byte aligned initially, testing
// the fault handlers correction for psp.
void BeginBaseFaultUnalignedStackTest() {
  uint32_t magic = kMagicPattern;
  asm volatile(
      // Push one register to cause $sp to be no longer 8-byte aligned,
      // assuming it started 8-byte aligned as expected.
      " push {r0}                                             \n"
      " mov r0, %[magic]                                      \n"
      " movs r1, #0                                           \n"
      " mov r2, pc                                            \n"
      " mov r3, lr                                            \n"
      // This instruction reads a bad address. Our fault handler should
      // ultimately advance the pc to the pop instruction.
      " ldr r1, [r0]                                          \n"
      " nop                                                   \n"
      " nop                                                   \n"
      " pop {r0}                                              \n"
      // clang-format off
      : /*output=*/
      : /*input=*/[magic]"r"(magic)
      : /*clobbers=*/"r0", "r1", "r2", "r3"
      // clang-format on
  );

  // Check that the stack align bit was set.
  EXPECT_EQ(captured_state.base.psr & kPsrExtraStackAlignBit,
            kPsrExtraStackAlignBit);
}

// Populate some of the extended set of captured registers, then trigger
// exception.
void BeginExtendedFaultTest() {
  uint32_t magic = kMagicPattern;
  volatile uint32_t local_msp = 0;
  volatile uint32_t local_psp = 0;
  asm volatile(
      " mov r4, %[magic]                                      \n"
      " movs r5, #0                                           \n"
      " mov r11, %[magic]                                     \n"
      " mrs %[local_msp], msp                                 \n"
      " mrs %[local_psp], psp                                 \n"
      // This instruction reads a bad address.
      " ldr r1, [r4]                                          \n"
      " nop                                                   \n"
      " nop                                                   \n"
      // clang-format off
      : /*output=*/[local_msp]"=r"(local_msp), [local_psp]"=r"(local_psp)
      : /*input=*/[magic]"r"(magic)
      : /*clobbers=*/"r4", "r5", "r11", "memory"
      // clang-format on
  );

  // Check that the stack align bit was not set.
  EXPECT_EQ(captured_state.base.psr & kPsrExtraStackAlignBit, 0u);

  // Check that the captured stack pointers matched the ones in the context of
  // the fault.
  EXPECT_EQ(static_cast<uint32_t>(captured_state.extended.msp), local_msp);
  EXPECT_EQ(static_cast<uint32_t>(captured_state.extended.psp), local_psp);
}

// Populate some of the extended set of captured registers, then trigger
// exception.
// This version causes stack to not be 4-byte aligned initially, testing
// the fault handlers correction for psp.
void BeginExtendedFaultUnalignedStackTest() {
  uint32_t magic = kMagicPattern;
  volatile uint32_t local_msp = 0;
  volatile uint32_t local_psp = 0;
  asm volatile(
      // Push one register to cause $sp to be no longer 8-byte aligned,
      // assuming it started 8-byte aligned as expected.
      " push {r0}                                             \n"
      " mov r4, %[magic]                                      \n"
      " movs r5, #0                                           \n"
      " mov r11, %[magic]                                     \n"
      " mrs %[local_msp], msp                                 \n"
      " mrs %[local_psp], psp                                 \n"
      // This instruction reads a bad address. Our fault handler should
      // ultimately advance the pc to the pop instruction.
      " ldr r1, [r4]                                          \n"
      " nop                                                   \n"
      " nop                                                   \n"
      " pop {r0}                                              \n"
      // clang-format off
      : /*output=*/[local_msp]"=r"(local_msp), [local_psp]"=r"(local_psp)
      : /*input=*/[magic]"r"(magic)
      : /*clobbers=*/"r0", "r4", "r5", "r11", "memory"
      // clang-format on
  );

  // Check that the stack align bit was set.
  EXPECT_EQ(captured_state.base.psr & kPsrExtraStackAlignBit,
            kPsrExtraStackAlignBit);

  // Check that the captured stack pointers matched the ones in the context of
  // the fault.
  EXPECT_EQ(static_cast<uint32_t>(captured_state.extended.msp), local_msp);
  EXPECT_EQ(static_cast<uint32_t>(captured_state.extended.psp), local_psp);
}

void Setup() {
  pw_cpu_exception_SetHandler(TestingExceptionHandler);
  InstallVectorTableEntries(
      reinterpret_cast<uint32_t*>(pw_cpu_exception_Entry));
  exceptions_handled = 0;
  current_fault_depth = 0;
  captured_state = {};
}

TEST(FaultEntry, BasicFault) {
  Setup();
  BeginBaseFaultTest();
  ASSERT_EQ(exceptions_handled, 1u);
  // captured_state values must be cast since they're in a packed struct.
  EXPECT_EQ(static_cast<uint32_t>(captured_state.base.r0), kMagicPattern);
  EXPECT_EQ(static_cast<uint32_t>(captured_state.base.r1), 0u);
  // PC is manually saved in r2 before the exception occurs (where PC is also
  // stored). Ensure these numbers are within a reasonable distance.
  uint32_t captured_pc_distance =
      captured_state.base.pc - captured_state.base.r2;
  EXPECT_LT(captured_pc_distance, kMaxPcDistance);
  EXPECT_EQ(static_cast<uint32_t>(captured_state.base.r3),
            static_cast<uint32_t>(captured_state.base.lr));
}

TEST(FaultEntry, BasicUnalignedStackFault) {
  Setup();
  BeginBaseFaultUnalignedStackTest();
  ASSERT_EQ(exceptions_handled, 1u);
  // captured_state values must be cast since they're in a packed struct.
  EXPECT_EQ(static_cast<uint32_t>(captured_state.base.r0), kMagicPattern);
  EXPECT_EQ(static_cast<uint32_t>(captured_state.base.r1), 0u);
  // PC is manually saved in r2 before the exception occurs (where PC is also
  // stored). Ensure these numbers are within a reasonable distance.
  uint32_t captured_pc_distance =
      captured_state.base.pc - captured_state.base.r2;
  EXPECT_LT(captured_pc_distance, kMaxPcDistance);
  EXPECT_EQ(static_cast<uint32_t>(captured_state.base.r3),
            static_cast<uint32_t>(captured_state.base.lr));
}

TEST(FaultEntry, ExtendedFault) {
  Setup();
  BeginExtendedFaultTest();
  ASSERT_EQ(exceptions_handled, 1u);
  ASSERT_TRUE(span_matches);
  const ExtraRegisters& extended_registers = captured_state.extended;
  // captured_state values must be cast since they're in a packed struct.
  EXPECT_EQ(static_cast<uint32_t>(extended_registers.r4), kMagicPattern);
  EXPECT_EQ(static_cast<uint32_t>(extended_registers.r5), 0u);
  EXPECT_EQ(static_cast<uint32_t>(extended_registers.r11), kMagicPattern);

  // Check expected values for this crash.
  EXPECT_EQ((extended_registers.icsr & 0x1FFu), kHardFaultIsrNum);
}

TEST(FaultEntry, ExtendedUnalignedStackFault) {
  Setup();
  BeginExtendedFaultUnalignedStackTest();
  ASSERT_EQ(exceptions_handled, 1u);
  ASSERT_TRUE(span_matches);
  const ExtraRegisters& extended_registers = captured_state.extended;
  // captured_state values must be cast since they're in a packed struct.
  EXPECT_EQ(static_cast<uint32_t>(extended_registers.r4), kMagicPattern);
  EXPECT_EQ(static_cast<uint32_t>(extended_registers.r5), 0u);
  EXPECT_EQ(static_cast<uint32_t>(extended_registers.r11), kMagicPattern);

  // Check expected values for this crash.
  EXPECT_EQ((extended_registers.icsr & 0x1FFu), kHardFaultIsrNum);
}

void TestingExceptionHandler(pw_cpu_exception_State* state) {
  if (++current_fault_depth > kMaxFaultDepth) {
    volatile bool loop = true;
    while (loop) {
      // Hit unexpected nested crash, prevent further nesting.
    }
  }

  // After a fault has been handled, the exception handler will
  // return to the faulting instruction.  For testing, move the
  // PC to the next instruction to allow the tests to continue
  // without entering an exception loop.
  // v6m supports variable instruction size, so always skip the
  // max instruction size, knowing there is enough nops after the
  // faulting test instruction to ensure it's safe to skip kMaxInstructionSize
  // even if it's a 2 byte instruction.
  state->base.pc += kMaxInstructionSize;

  // Disable traps. Must be disabled before EXPECT, as memcpy() can do unaligned
  // operations.
  cortex_m_ccr &= ~kUnalignedTrapEnableMask;

  // Copy captured state to check later.
  std::memcpy(&captured_states[exceptions_handled],
              state,
              sizeof(pw_cpu_exception_State));

  // Ensure span compares to be the same.
  span<const uint8_t> state_span = RawFaultingCpuState(*state);
  EXPECT_EQ(state_span.size(), sizeof(pw_cpu_exception_State));
  if (std::memcmp(state, state_span.data(), state_span.size()) == 0) {
    span_matches = true;
  } else {
    span_matches = false;
  }

  exceptions_handled++;

  EXPECT_EQ(state->extended.shcsr, cortex_m_shcsr());
}

}  // namespace
}  // namespace pw::cpu_exception::cortex_m
