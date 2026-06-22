// Copyright 2022 The Pigweed Authors
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
#include "pw_system/device_handler.h"

#include "hardware/watchdog.h"
#include "pw_cpu_exception/state.h"
#include "pw_cpu_exception_cortex_m/snapshot.h"
#include "pw_thread_freertos/snapshot.h"

// These symbols are added to the default pico_sdk linker script as part
// of the build process. If the build fails due to missing these symbols,
// it may be because a different linker script is configured and these
// symbols need added.
extern "C" uint32_t __pw_code_begin;
extern "C" uint32_t __pw_code_end;

extern "C" uint32_t __StackBottom;
extern "C" uint32_t __StackTop;

namespace pw::system::device_handler {

namespace {

uintptr_t GetLinkerSymbolValue(const uint32_t& symbol) {
  return reinterpret_cast<uintptr_t>(&symbol);
}

bool IsAddressExecutable(uintptr_t address) {
  const uintptr_t code_begin = GetLinkerSymbolValue(__pw_code_begin);
  const uintptr_t code_end = GetLinkerSymbolValue(__pw_code_end);

  if ((address >= code_begin) && (address <= code_end)) {
    return true;
  }

  return false;
}

Status AddressFilteredDumper(
    thread::proto::pwpb::Thread::StreamEncoder& encoder, ConstByteSpan stack) {
  span<const uint32_t> addresses =
      span<const uint32_t>(reinterpret_cast<const uint32_t*>(stack.data()),
                           stack.size_bytes() / sizeof(uint32_t));
  for (const uint32_t address : addresses) {
    if (IsAddressExecutable(address)) {
      auto status = encoder.WriteRawBacktrace(address);
      if (status != OkStatus())
        return status;
    }
  }
  return OkStatus();
}

}  // namespace

void RebootSystem() { watchdog_reboot(0, 0, 0); }

void CapturePlatformMetadata(
    snapshot::pwpb::Metadata::StreamEncoder& metadata_encoder) {
// The device_handler is shared between rp2040 and 2350, so handle differences
// with the preprocessor.
#if _PW_ARCH_ARM_V6M
  // TODO: https://pwbug.dev/357132837 - Review if IgnoreError is correct here.
  metadata_encoder.WriteCpuArch(snapshot::pwpb::CpuArchitecture::Enum::ARMV6M)
      .IgnoreError();
#elif _PW_ARCH_ARM_V8M_MAINLINE || _PW_ARCH_ARM_V8_1M_MAINLINE
  // TODO: https://pwbug.dev/357132837 - Review if IgnoreError is correct here.
  metadata_encoder.WriteCpuArch(snapshot::pwpb::CpuArchitecture::Enum::ARMV8M)
      .IgnoreError();
#else
#error "Unknown CPU architecture."
#endif  // _PW_ARCH_ARM_V6M
}

Status CaptureCpuState(
    const pw_cpu_exception_State& cpu_state,
    snapshot::pwpb::Snapshot::StreamEncoder& snapshot_encoder) {
  return cpu_exception::cortex_m::SnapshotCpuState(
      cpu_state,
      *static_cast<cpu_exception::cortex_m::pwpb::SnapshotCpuStateOverlay::
                       StreamEncoder*>(
          static_cast<protobuf::StreamEncoder*>(&snapshot_encoder)));
}

Status CaptureMainStackThread(
    const pw_cpu_exception_State& cpu_state,
    thread::proto::pwpb::SnapshotThreadInfo::StreamEncoder& encoder) {
  uintptr_t stack_low_addr = GetLinkerSymbolValue(__StackBottom);
  uintptr_t stack_high_addr = GetLinkerSymbolValue(__StackTop);
  thread::ProcessThreadStackCallback stack_dumper =
      device_handler::AddressFilteredDumper;

  return cpu_exception::cortex_m::SnapshotMainStackThread(
      cpu_state, stack_low_addr, stack_high_addr, encoder, stack_dumper);
}

Status CaptureThreads(
    uint32_t running_thread_stack_pointer,
    thread::proto::pwpb::SnapshotThreadInfo::StreamEncoder& encoder) {
  thread::ProcessThreadStackCallback stack_dumper =
      device_handler::AddressFilteredDumper;
  return thread::freertos::SnapshotThreads(
      reinterpret_cast<void*>(running_thread_stack_pointer),
      encoder,
      stack_dumper);
}

}  // namespace pw::system::device_handler
