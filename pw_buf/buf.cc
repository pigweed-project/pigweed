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

#include "pw_allocator/allocator.h"
#include "pw_assert/assert.h"
#include "pw_assert/check.h"

namespace pw {

ConstBuf& ConstBuf::operator=(ConstBuf&& other) noexcept {
  if (this != &other) {
    reset();
    MoveFrom(std::move(other));
  }
  return *this;
}

ConstBuf& ConstBuf::operator=(Buf&& other) noexcept {
  if (this != &other.const_buf_) {
    reset();
    MoveFrom(std::move(other.const_buf_));
  }
  return *this;
}

void ConstBuf::reset() {
  if (deallocator_ != nullptr && base_ != nullptr) {
    deallocator_->Deallocate(base_);
  }
  base_ = nullptr;
  view_ = {};
  deallocator_ = nullptr;
}

void ConstBuf::MoveFrom(ConstBuf&& other) {
  base_ = std::exchange(other.base_, nullptr);
  view_ = std::exchange(other.view_, pw::span<std::byte>{});
  deallocator_ = std::exchange(other.deallocator_, nullptr);
}

ConstBuf ConstBuf::Slice(size_t offset, size_t length) && {
  PW_CHECK(offset <= view_.size());
  PW_CHECK(length <= view_.size() - offset);
  view_ = view_.subspan(offset, length);
  return std::move(*this);
}

ConstBuf ConstBuf::Reclaim(size_t prefix_count, size_t suffix_count) && {
  PW_CHECK(prefix_count <= static_cast<size_t>(view_.data() - base_));
  view_ = pw::span<std::byte>(view_.data() - prefix_count,
                              view_.size() + prefix_count + suffix_count);
  return std::move(*this);
}

Buf Buf::Allocate(Allocator& allocator, size_t offset, size_t size) {
  size_t allocation_size = offset + size;
  void* ptr =
      allocator.Allocate(allocator::Layout::Of<std::byte[]>(allocation_size));
  PW_ASSERT(ptr != nullptr);
  std::byte* byte_ptr = static_cast<std::byte*>(ptr);
  return Buf(byte_ptr, offset, size, allocator);
}

Buf Buf::TryAllocate(Allocator& allocator, size_t offset, size_t size) {
  size_t allocation_size = offset + size;
  void* ptr =
      allocator.Allocate(allocator::Layout::Of<std::byte[]>(allocation_size));
  if (ptr == nullptr) {
    return Buf();
  }
  std::byte* byte_ptr = static_cast<std::byte*>(ptr);
  return Buf(byte_ptr, offset, size, allocator);
}

}  // namespace pw
