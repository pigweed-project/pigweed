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

#include "pw_buf/buf.h"

#include <cstring>
#include <limits>
#include <utility>

#include "pw_allocator/allocator.h"
#include "pw_assert/check.h"

namespace pw {

Buf& Buf::operator=(Buf&& other) noexcept {
  if (this != &other) {
    reset();
    MoveFrom(std::move(other));
  }
  return *this;
}

void Buf::reset() {
  if (owned()) {
    deallocator_->Deallocate(mutable_base());
  }
  Clear();
}

Buf Slice(Buf&& buf, size_t offset, size_t length) {
  const size_t new_prefix = buf.prefix_ + offset;
  const size_t new_suffix = buf.suffix_ + (buf.size_ - offset - length);
  PW_CHECK(offset <= buf.size_ && length <= buf.size_ - offset &&
           (new_prefix | new_suffix) <=
               std::numeric_limits<Buf::OffsetType>::max());
  buf.data_ += offset;
  buf.size_ = length;
  buf.prefix_ = static_cast<Buf::OffsetType>(new_prefix);
  buf.suffix_ = static_cast<Buf::OffsetType>(new_suffix);
  return std::move(buf);
}

Buf Reclaim(Buf&& buf, size_t prefix_count, size_t suffix_count) {
  PW_CHECK(prefix_count <= buf.prefix_ && suffix_count <= buf.suffix_);
  buf.data_ -= prefix_count;
  buf.size_ += prefix_count + suffix_count;
  buf.prefix_ -= static_cast<Buf::OffsetType>(prefix_count);
  buf.suffix_ -= static_cast<Buf::OffsetType>(suffix_count);
  return std::move(buf);
}

UniquePtr<std::byte[]> TakeAllocation(Buf&& buf) {
  if (!buf.owned()) {
    // Only a null Buf may be unowned here. A null Buf has no state to clear.
    PW_CHECK(buf.data_ == nullptr);
    return UniquePtr<std::byte[]>();
  }
  UniquePtr<std::byte[]> allocation(
      buf.mutable_base(), buf.base_size(), *buf.deallocator_);
  buf.Clear();
  return allocation;
}

Buf Buf::Allocate(Allocator& allocator, size_t size_bytes) {
  Buf buf = TryAllocate(allocator, size_bytes);
  PW_CHECK(buf != nullptr);
  return buf;
}

Buf Buf::AllocateFill(Allocator& allocator,
                      size_t size_bytes,
                      std::byte value) {
  Buf buf = Allocate(allocator, size_bytes);
  if (size_bytes > 0) {
    std::memset(buf.data(), static_cast<int>(value), size_bytes);
  }
  return buf;
}

Buf Buf::AllocateCopy(Allocator& allocator,
                      const std::byte* data,
                      size_t size_bytes) {
  Buf buf = Allocate(allocator, size_bytes);
  if (size_bytes > 0) {
    std::memcpy(buf.data(), data, size_bytes);
  }
  return buf;
}

Buf Buf::TryAllocate(Allocator& allocator, size_t size_bytes) {
  if (size_bytes == 0) {
    return ZeroLength();
  }
  void* ptr =
      allocator.Allocate(allocator::Layout::Of<std::byte[]>(size_bytes));
  if (ptr == nullptr) {
    return Buf();
  }
  return Buf(static_cast<std::byte*>(ptr), size_bytes, allocator);
}

Buf Buf::TryAllocateFill(Allocator& allocator,
                         size_t size_bytes,
                         std::byte value) {
  Buf buf = TryAllocate(allocator, size_bytes);
  if (!buf.empty()) {
    std::memset(buf.data(), static_cast<int>(value), size_bytes);
  }
  return buf;
}

Buf Buf::TryAllocateCopy(Allocator& allocator,
                         const std::byte* data,
                         size_t size_bytes) {
  Buf buf = TryAllocate(allocator, size_bytes);
  if (!buf.empty()) {
    std::memcpy(buf.data(), data, size_bytes);
  }
  return buf;
}

}  // namespace pw
