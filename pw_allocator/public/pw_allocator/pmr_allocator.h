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
#pragma once

#include <cstddef>

#include "pw_allocator/allocator.h"
#include "pw_allocator/config.h"
#include "pw_status/status_with_size.h"

#if __has_include(<memory_resource>)
#include <memory_resource>
namespace pw {
namespace pmr = ::std::pmr;
}  // namespace pw

#elif __has_include(<experimental/memory_resource>)
#include <experimental/memory_resource>
namespace pw {
namespace pmr = ::std::experimental::pmr;
}  // namespace pw

#else
#error "<memory_resource> is required to use this header!"
#endif  // __has_include(<memory_resource>)

namespace pw::allocator {

// Forward declaration
class PmrAllocator;

namespace internal {

/// Implementation of C++'s abstract memory resource interface that uses an
/// Allocator.
///
/// NOTE! This class aborts if allocation fails.
///
/// See also https://en.cppreference.com/w/cpp/memory/memory_resource.
class MemoryResource final : public pw::pmr::memory_resource {
 public:
  constexpr MemoryResource() = default;

  pw::Allocator& allocator() { return *allocator_; }

 private:
  friend class ::pw::allocator::PmrAllocator;
  void set_allocator(pw::Allocator& allocator) { allocator_ = &allocator; }

  void* do_allocate(size_t bytes, size_t alignment) override;
  void do_deallocate(void* p, size_t bytes, size_t alignment) override;
  bool do_is_equal(
      const pw::pmr::memory_resource& other) const noexcept override;

  pw::Allocator* allocator_ = nullptr;
};

}  // namespace internal

/// @submodule{pw_allocator,forwarding}

/// Implementation of C++'s abstract polymorphic allocator interface that uses
/// a pw::Allocator.
///
/// Note that despite is name, this is NOT a pw::Allocator itself. Instead, it
/// can be used in `pw::pmr` containers, such as `pw::pmr::vector`.
///
/// See also https://en.cppreference.com/w/cpp/memory/polymorphic_allocator.
class PmrAllocator final : public pw::pmr::polymorphic_allocator<std::byte> {
 public:
  using Base = pw::pmr::polymorphic_allocator<std::byte>;

  explicit PmrAllocator(pw::Allocator& allocator) : Base(&memory_resource_) {
    memory_resource_.set_allocator(allocator);
  }

  pw::Allocator& allocator() { return memory_resource_.allocator(); }

 private:
  internal::MemoryResource memory_resource_;
};

/// @}

}  // namespace pw::allocator
