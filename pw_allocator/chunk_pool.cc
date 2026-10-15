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

#include "pw_allocator/chunk_pool.h"

#include "lib/stdcompat/bit.h"
#include "pw_assert/check.h"
#include "pw_bytes/alignment.h"

namespace pw::allocator {

static Layout EnsurePointerLayout(const Layout& layout) {
  return Layout(std::max(layout.size(), sizeof(void*)),
                std::max(layout.alignment(), alignof(void*)));
}

ChunkPool::ChunkPool(ByteSpan region, const Layout& layout)
    : AbstractPool(kCapabilities, layout),
      allocated_layout_(EnsurePointerLayout(layout)) {
  Result<ByteSpan> result =
      GetAlignedSubspan(region, allocated_layout_.alignment());
  if constexpr (Hardening::kIncludesDebugChecks) {
    PW_CHECK_OK(result.status());
  }
  start_ = cpp20::bit_cast<uintptr_t>(region.data());
  end_ = start_ + region.size() - (region.size() % allocated_layout_.size());
  region = result.value();
  next_ = region.data();
  std::byte* current = next_;
  std::byte* end = current + region.size();
  std::byte** next = &current;
  while (current < end) {
    next = std::launder(reinterpret_cast<std::byte**>(current));
    current += allocated_layout_.size();
    *next = current;
  }
  *next = nullptr;
}

void* ChunkPool::DoAllocate() {
  if (next_ == nullptr) {
    return nullptr;
  }
  std::byte* ptr = next_;
  next_ = *(std::launder(reinterpret_cast<std::byte**>(next_)));
  return ptr;
}

void ChunkPool::DoDeallocate(void* ptr) {
  if (ptr == nullptr) {
    return;
  }
  std::byte** next = std::launder(reinterpret_cast<std::byte**>(ptr));
  *next = next_;
  next_ = cpp20::bit_cast<std::byte*>(ptr);
}

Result<Layout> ChunkPool::DoGetInfo(InfoType info_type, const void* ptr) const {
  if (info_type == InfoType::kCapacity) {
    return Layout(end_ - start_, allocated_layout_.alignment());
  }
  auto addr = cpp20::bit_cast<uintptr_t>(ptr);
  if (addr < start_ || end_ <= addr) {
    return Status::OutOfRange();
  }
  if ((addr - start_) % allocated_layout_.size() != 0) {
    return Status::OutOfRange();
  }
  switch (info_type) {
    case InfoType::kRequestedLayoutOf:
    case InfoType::kUsableLayoutOf:
    case InfoType::kAllocatedLayoutOf:
      return allocated_layout_;
    case InfoType::kRecognizes:
      return Layout();
    case InfoType::kCapacity:
    default:
      return Status::Unimplemented();
  }
}

}  // namespace pw::allocator
