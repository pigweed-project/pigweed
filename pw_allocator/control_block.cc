// Copyright 2025 The Pigweed Authors
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

#include "pw_allocator/internal/control_block.h"

#include "lib/stdcompat/bit.h"
#include "pw_allocator/allocator.h"
#include "pw_allocator/capability.h"
#include "pw_allocator/deallocator.h"
#include "pw_allocator/hardening.h"
#include "pw_assert/check.h"
#include "pw_bytes/alignment.h"

namespace pw::allocator::internal {

ControlBlock* ControlBlock::Create(Allocator* allocator, Layout layout) {
  size_t size = layout.size();
  layout = layout.Extend(AlignUp(sizeof(ControlBlock), layout.alignment()));
  void* ptr = allocator->Allocate(layout);
  if (ptr == nullptr) {
    return nullptr;
  }
  auto addr = cpp20::bit_cast<uintptr_t>(ptr);
  addr = AlignUp(addr + sizeof(ControlBlock), layout.alignment());
  auto* data = cpp20::bit_cast<std::byte*>(addr);
  return new (ptr) ControlBlock(allocator, data, size, true);
}

ControlBlock* ControlBlock::Create(Deallocator* deallocator,
                                   void* data,
                                   size_t size) {
  if (!deallocator->HasCapability(
          allocator::Capability::kCanAllocateArbitraryLayout)) {
    return nullptr;
  }
  auto* allocator = static_cast<Allocator*>(deallocator);
  void* ptr = allocator->Allocate(Layout::Of<ControlBlock>());
  if (ptr == nullptr) {
    return nullptr;
  }
  return new (ptr) ControlBlock(allocator, data, size, false);
}

ControlBlock::ControlBlock(Allocator* allocator,
                           void* data,
                           size_t size,
                           bool coallocated)
    : allocator_(allocator),
      data_(data),
      coallocated_and_size_((size << 1) | (coallocated ? 1 : 0)),
      num_weak_and_shared_(Pack(1, 1)) {
  PW_CHECK_UINT_EQ(size, GetSize());
}

ControlBlock::~ControlBlock() {
  if (!IsCoallocated()) {
    allocator_->Deallocate(data_);
  }
}

int32_t ControlBlock::num_shared() const noexcept {
  return UnpackShared(num_weak_and_shared_.load(std::memory_order_relaxed));
}

bool ControlBlock::IncrementShared() {
  uint32_t num = num_weak_and_shared_.load(std::memory_order_relaxed);
  while (true) {
    uint16_t num_weak = UnpackWeak(num);
    uint16_t num_shared = UnpackShared(num);
    if constexpr (Hardening::kIncludesDebugChecks) {
      PW_CHECK_UINT_GE(num_weak, num_shared);
    }
    if (num_shared == 0 || num_shared == 0xFFFF) {
      return false;
    }
    uint32_t packed = Pack(num_weak + 1, num_shared + 1);
    if (num_weak_and_shared_.compare_exchange_weak(
            num, packed, std::memory_order_relaxed)) {
      return true;
    }
  }
}

bool ControlBlock::IncrementWeak() {
  uint32_t num = num_weak_and_shared_.load(std::memory_order_relaxed);
  while (true) {
    uint16_t num_weak = UnpackWeak(num);
    uint16_t num_shared = UnpackShared(num);
    if constexpr (Hardening::kIncludesDebugChecks) {
      PW_CHECK_UINT_GE(num_weak, num_shared);
    }
    if (num_weak == 0 || num_weak == 0xFFFF) {
      return false;
    }
    uint32_t packed = Pack(num_weak + 1, num_shared);
    if (num_weak_and_shared_.compare_exchange_weak(
            num, packed, std::memory_order_relaxed)) {
      return true;
    }
  }
}

ControlBlock::Action ControlBlock::DecrementShared() {
  uint32_t prev =
      num_weak_and_shared_.fetch_sub(Pack(1, 1), std::memory_order_acq_rel);
  uint16_t prev_weak = UnpackWeak(prev);
  uint16_t prev_shared = UnpackShared(prev);
  if constexpr (Hardening::kIncludesDebugChecks) {
    PW_CHECK_UINT_NE(prev_weak, 0);
    PW_CHECK_UINT_NE(prev_shared, 0);
    PW_CHECK_UINT_GE(prev_weak, prev_shared);
  }
  if (prev_weak == 1 && prev_shared == 1) {
    return ControlBlock::Action::kFree;
  }
  if (prev_shared == 1) {
    return ControlBlock::Action::kExpire;
  }
  return ControlBlock::Action::kNone;
}

ControlBlock::Action ControlBlock::DecrementWeak() {
  uint32_t prev =
      num_weak_and_shared_.fetch_sub(Pack(1, 0), std::memory_order_acq_rel);
  uint16_t prev_weak = UnpackWeak(prev);
  if constexpr (Hardening::kIncludesDebugChecks) {
    uint16_t prev_shared = UnpackShared(prev);
    PW_CHECK_UINT_NE(prev_weak, 0);
    PW_CHECK_UINT_GE(prev_weak, prev_shared);
  }
  if (prev_weak == 1) {
    return ControlBlock::Action::kFree;
  }
  return ControlBlock::Action::kNone;
}

}  // namespace pw::allocator::internal
