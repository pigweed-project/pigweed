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

#include "pw_async2/internal/logging.h"
// logging.h must be included first

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <tuple>
#include <utility>

#include "pw_allocator/deallocator.h"
#include "pw_allocator/layout.h"
#include "pw_async2/coro.h"
#include "pw_bytes/alignment.h"
#include "pw_log/log.h"

namespace pw::async2::internal {
namespace {

// TODO: https://pwbug.dev/529845745 - Extract utility functions to use here
// and with FramingAllocator to reduce similar/duplicate code. Just take care
// that the functions do not increase the code size in any meaningful way.

struct CoroAllocationHeader {
  Allocator* allocator;
  void* outer_alloc;

  CoroAllocationHeader(Allocator* alloc, void* outer)
      : allocator(alloc), outer_alloc(outer) {}
};

}  // namespace

void* CoroPromiseBase::SharedNew(CoroContext coro_cx,
                                 std::size_t size,
                                 std::align_val_t align) noexcept {
  const auto coro_layout =
      allocator::Layout(size, static_cast<std::size_t>(align));

  const size_t padding_size = coro_layout.alignment();
  const auto outer_layout = allocator::Layout(
      coro_layout.size() + sizeof(CoroAllocationHeader) + padding_size,
      alignof(CoroAllocationHeader));

  PW_LOG_DEBUG("Allocating %zu B for %zu B coroutine with %zu B alignment",
               outer_layout.size(),
               coro_layout.size(),
               static_cast<std::size_t>(align));

  Allocator* const allocator = &coro_cx.allocator();
  auto* outer_alloc = allocator->Allocate(outer_layout);
  if (outer_alloc == nullptr) {
    return nullptr;
  }

  const uintptr_t outer_addr = reinterpret_cast<uintptr_t>(outer_alloc);

  // We need coro_alloc to be aligned to coro_layout.alignment(), and
  // coro_alloc >= outer_addr + sizeof(CoroAllocationHeader).
  const uintptr_t min_coro_addr = outer_addr + sizeof(CoroAllocationHeader);
  const uintptr_t coro_addr =
      pw::AlignUp(min_coro_addr, coro_layout.alignment());

  void* const coro_alloc = reinterpret_cast<void*>(coro_addr);

  // Construct the header in the space between the outer allocation and the
  // inner coroutine allocation, and store the allocator and pointer to the
  // outer allocation there.
  std::construct_at<CoroAllocationHeader>(
      reinterpret_cast<CoroAllocationHeader*>(coro_addr -
                                              sizeof(CoroAllocationHeader)),
      allocator,
      outer_alloc);

  // Return the pointer to the memory to use for the coroutine state.
  return coro_alloc;
}

void CoroPromiseBase::SharedDelete(
    void* ptr,
    [[maybe_unused]] std::size_t size,
    [[maybe_unused]] std::align_val_t align) noexcept {
  if (ptr == nullptr) {
    return;
  }

  PW_LOG_DEBUG("Deallocating %zu B coroutine with %zu B alignment",
               size,
               static_cast<std::size_t>(align));

  auto* header = reinterpret_cast<CoroAllocationHeader*>(
      reinterpret_cast<uintptr_t>(ptr) - sizeof(CoroAllocationHeader));

  // Copy the values in the header so we can use them after destroying it.
  Allocator* allocator = header->allocator;
  void* outer_alloc = header->outer_alloc;

  std::destroy_at<CoroAllocationHeader>(header);
  allocator->Deallocate(outer_alloc);
}

}  // namespace pw::async2::internal
