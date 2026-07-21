// Copyright 2023 The Pigweed Authors
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

#include "pw_allocator/test_harness.h"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "lib/stdcompat/bit.h"
#include "pw_assert/assert.h"
#include "pw_assert/check.h"

namespace pw::allocator::test {
namespace {

// Helper to allow static_assert'ing in constexpr-if branches, e.g. that a
// visitor for a std::variant are exhaustive.
template <typename T>
[[maybe_unused]] constexpr bool not_reached(T&) {
  return false;
}

}  // namespace

size_t AlignmentFromLShift(size_t lshift, size_t size) {
  constexpr size_t max_bits = (sizeof(size) * CHAR_BIT) - 1;
  size_t num_bits =
      size == 0 ? 1 : std::min(size_t(cpp20::bit_width(size)), max_bits);
  size_t alignment = 1U << (lshift % num_bits);
  return std::max(alignment, alignof(TestHarness::Allocation));
}

void TestHarness::GenerateRequests(size_t max_size, size_t num_requests) {
  for (size_t i = 0; i < num_requests; ++i) {
    GenerateRequest(max_size);
  }
  Reset();
}

void TestHarness::GenerateRequest(size_t max_size) {
  Request request;
  size_t dealloc_threshold = 40;
  if (available_.has_value()) {
    // This corresponds to a fixed 10% chance to reallocate and a sliding
    // scale between:
    // * when empty, an 80% chance to allocate and a 10% chance to deallocate
    // * when full, a 30% chance to allocate and a 60% chance to deallocate
    dealloc_threshold = 80 - (allocated_ * 50) / available_.value();
  }
  do {
    size_t request_type;
    prng_->GetInt(request_type);
    request_type %= 100;
    if (request_type < dealloc_threshold) {
      request = GenerateAllocationRequest(max_size);
    } else if (request_type < 90) {
      request = GenerateDeallocationRequest();
    } else {
      request = GenerateReallocationRequest(max_size);
    }
  } while (!HandleRequest(request));
}

AllocationRequest TestHarness::GenerateAllocationRequest(size_t max_size) {
  AllocationRequest request;
  request.size = GenerateSize(max_size);
  uint8_t lshift;
  prng_->GetInt(lshift);
  request.alignment = AlignmentFromLShift(lshift, request.size);
  return request;
}

DeallocationRequest TestHarness::GenerateDeallocationRequest() {
  DeallocationRequest request;
  prng_->GetInt(request.index);
  return request;
}

ReallocationRequest TestHarness::GenerateReallocationRequest(size_t max_size) {
  ReallocationRequest request;
  prng_->GetInt(request.index);
  request.new_size = GenerateSize(max_size);
  return request;
}

size_t TestHarness::GenerateSize(size_t max_size) {
  static constexpr size_t kMinSize = sizeof(TestHarness::Allocation);
  size_t size = 0;
  if (max_size_.has_value()) {
    max_size = std::min(max_size, max_size_.value());
  }
  if (max_size <= kMinSize) {
    return kMinSize;
  }
  prng_->GetInt(size);
  size %= max_size - kMinSize;
  return kMinSize + size;
}

void TestHarness::HandleRequests(const Vector<Request>& requests) {
  for (const auto& request : requests) {
    HandleRequest(request);
  }
  Reset();
}

bool TestHarness::HandleRequest(const Request& request) {
  PW_DCHECK_NOTNULL(allocator_);
  return std::visit(
      [this](auto&& r) {
        using T = std::decay_t<decltype(r)>;

        if constexpr (std::is_same_v<T, AllocationRequest>) {
          Layout layout(r.size, r.alignment);
          BeforeAllocate(layout);
          void* ptr = allocator_->Allocate(layout);
          AfterAllocate(ptr);
          if (ptr != nullptr) {
            AddAllocation(ptr, layout);
            max_size_.reset();
          } else {
            max_size_ = std::max(layout.size() / 2, size_t(1));
          }
        } else if constexpr (std::is_same_v<T, DeallocationRequest>) {
          Allocation* old = RemoveAllocation(r.index);
          if (old == nullptr) {
            return false;
          }
          BeforeDeallocate(old);
          allocator_->Deallocate(old);
          AfterDeallocate();

        } else if constexpr (std::is_same_v<T, ReallocationRequest>) {
          Allocation* old = RemoveAllocation(r.index);
          if (old == nullptr) {
            return false;
          }
          Layout new_layout(r.new_size, old->layout.alignment());
          BeforeReallocate(new_layout);
          void* new_ptr = allocator_->Reallocate(old, new_layout);
          AfterReallocate(new_ptr);
          if (new_ptr == nullptr) {
            AddAllocation(old, old->layout);
          } else {
            AddAllocation(new_ptr, new_layout);
          }
        } else {
          static_assert(not_reached(r), "unsupported request type!");
        }
        return true;
      },
      request);
}

void TestHarness::Reset() {
  if (allocator_ == nullptr) {
    return;
  }
  while (!allocations_.empty()) {
    allocator_->Deallocate(RemoveAllocation(0));
  }
}

void TestHarness::AddAllocation(void* ptr, Layout layout) {
  constexpr Layout min_layout = Layout::Of<Allocation>();
  if (layout.size() < min_layout.size() ||
      layout.alignment() < min_layout.alignment()) {
    // The harness should want to test small sizes and alignments, but it
    // needs the layout to be at least `Layout::Of<Allocation>` in order
    // to persist details about it. If either the size or alignment is too
    // small, deallocate immediately.
    BeforeDeallocate(ptr);
    allocator_->Deallocate(ptr);
    AfterDeallocate();
    return;
  }
  auto* bytes = static_cast<std::byte*>(ptr);
  auto* allocation = ::new (bytes) Allocation(layout);
  allocations_.push_back(*allocation);
  allocated_ += layout.size();
  ++num_allocations_;
}

TestHarness::Allocation* TestHarness::RemoveAllocation(size_t index) {
  // Move the target allocation to the back of the list.
  if (num_allocations_ == 0) {
    return nullptr;
  }
  index %= num_allocations_;
  auto prev = allocations_.before_begin();
  for (auto& allocation : allocations_) {
    if (index == 0) {
      PW_ASSERT(allocated_ >= allocation.layout.size());
      allocated_ -= allocation.layout.size();
      allocations_.erase_after(prev);
      --num_allocations_;
      return &allocation;
    }
    --index;
    ++prev;
  }
  PW_CRASH("unreachable");
}

}  // namespace pw::allocator::test
