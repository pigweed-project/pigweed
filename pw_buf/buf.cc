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

#include "pw_allocator/allocator.h"
#include "pw_assert/check.h"

namespace pw {

ConstBuf& ConstBuf::operator=(ConstBuf&& other) noexcept {
  if (this != &other) {
    reset();
    MoveFrom(std::move(other));
  }
  return *this;
}

void ConstBuf::reset() {
  if (deallocator_ != nullptr && data_ != nullptr) {
    deallocator_->Deallocate(base());
  }
  data_ = nullptr;
  size_ = 0;
  deallocator_ = nullptr;
  prefix_ = 0;
  suffix_ = 0;
}

ConstBuf ConstBuf::Slice(size_t offset, size_t length) && {
  const size_t new_prefix = prefix_ + offset;
  const size_t new_suffix = suffix_ + (size_ - offset - length);
  PW_CHECK(offset <= size_ && length <= size_ - offset &&
           (new_prefix | new_suffix) <= std::numeric_limits<OffsetType>::max());
  data_ += offset;
  size_ = length;
  prefix_ = static_cast<OffsetType>(new_prefix);
  suffix_ = static_cast<OffsetType>(new_suffix);
  return std::move(*this);
}

ConstBuf ConstBuf::Reclaim(size_t prefix_count, size_t suffix_count) && {
  PW_CHECK(prefix_count <= prefix_ && suffix_count <= suffix_);
  data_ -= prefix_count;
  size_ += prefix_count + suffix_count;
  prefix_ -= static_cast<OffsetType>(prefix_count);
  suffix_ -= static_cast<OffsetType>(suffix_count);
  return std::move(*this);
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
  if (buf != nullptr && size_bytes > 0) {
    std::memset(buf.data(), static_cast<int>(value), size_bytes);
  }
  return buf;
}

Buf Buf::TryAllocateCopy(Allocator& allocator,
                         const std::byte* data,
                         size_t size_bytes) {
  Buf buf = TryAllocate(allocator, size_bytes);
  if (buf != nullptr && size_bytes > 0) {
    std::memcpy(buf.data(), data, size_bytes);
  }
  return buf;
}

}  // namespace pw
