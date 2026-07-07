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

#include "pw_allocator/bump_allocator.h"
#include "pw_allocator/deallocator.h"
#include "pw_allocator/layout.h"
#include "pw_assert/check.h"
#include "pw_async2/coro.h"
#include "pw_bytes/alignment.h"
#include "pw_bytes/span.h"
#include "pw_log/log.h"

namespace pw::async2::internal {
namespace {

// TODO: https://pwbug.dev/529845745 - Extract utility functions to use here and
// with FramingAllocator to reduce similar/duplicate code.

template <typename Prefix>
class FramedAllocation {
 public:
  static allocator::Layout GetOuterLayout(allocator::Layout inner_layout);
  template <typename... Args>
  static std::pair<Prefix*, void*> Allocate(Allocator* allocator,
                                            allocator::Layout inner_layout,
                                            Args&&... args);
  static Prefix& GetPrefixFromInner(void* inner);
  static void DeallocateFromInner(Allocator* allocator, void* inner);

 private:
  struct Header {
    Prefix prefix;
    size_t offset;

    template <typename... Args>
    explicit Header(size_t offs, Args&&... args)
        : prefix(std::forward<Args>(args)...), offset(offs) {}
  };

  static Header& GetHeaderFromInner(void* inner);

  static_assert(alignof(Header) <= alignof(size_t),
                "alignof(Header) cannot exceed alignof(size_t)");

  static constexpr allocator::Layout kHeaderLayout =
      allocator::Layout::Of<Header>();
};

template <typename Prefix>
allocator::Layout FramedAllocation<Prefix>::GetOuterLayout(
    allocator::Layout inner_layout) {
  // Note: This is intentionally an overestimate of the size needed.
  // We need enough extra room to:
  //   - A prefix (offset + pointer)
  //   - Padding to realign to the alignment needed for the coroutine state
  const size_t padding_size = inner_layout.alignment();
  return {inner_layout.size() + kHeaderLayout.size() + padding_size,
          kHeaderLayout.alignment()};
}

template <typename Prefix>
template <typename... Args>
std::pair<Prefix*, void*> FramedAllocation<Prefix>::Allocate(
    Allocator* allocator, allocator::Layout inner_layout, Args&&... args) {
  PW_DCHECK(allocator != nullptr);

  const auto outer_layout = GetOuterLayout(inner_layout);

  auto* outer_alloc = allocator->Allocate(outer_layout);
  if (outer_alloc == nullptr) {
    return {nullptr, nullptr};
  }

  const pw::ByteSpan bytes = {static_cast<std::byte*>(outer_alloc),
                              outer_layout.size()};
  allocator::BumpAllocator bump_allocator(bytes);

  // Reserve space for the prefix. Note the prefix will actually be located
  // after any padding needed to align the inner allocation, such that the
  // prefix immediately precedes the inner allocation in memory.
  std::ignore = bump_allocator.Allocate(kHeaderLayout);

  // Allocate space for the inner/wrapped allocation.
  void* const inner_alloc = bump_allocator.Allocate(inner_layout);

  // Construct the header (including prefix) in the space between the outer
  // allocation and the inner
  Header* const header = std::construct_at<Header>(
      reinterpret_cast<Header*>(reinterpret_cast<uintptr_t>(inner_alloc) -
                                sizeof(Header)),
      reinterpret_cast<uintptr_t>(inner_alloc) -
          reinterpret_cast<uintptr_t>(outer_alloc),
      std::forward<Args>(args)...);

  // Trim and return any excess memory.
  const size_t used = outer_layout.size() - bump_allocator.remaining();
  std::ignore = allocator->Resize(outer_alloc, used);

  return {&header->prefix, inner_alloc};
}

template <typename Prefix>
FramedAllocation<Prefix>::Header& FramedAllocation<Prefix>::GetHeaderFromInner(
    void* inner_alloc) {
  PW_CHECK(IsAlignedAs<size_t>(inner_alloc));
  return *reinterpret_cast<Header*>(reinterpret_cast<uintptr_t>(inner_alloc) -
                                    sizeof(Header));
}

template <typename Prefix>
Prefix& FramedAllocation<Prefix>::GetPrefixFromInner(void* inner_alloc) {
  Header& header = GetHeaderFromInner(inner_alloc);
  return header.prefix;
}

template <typename Prefix>
void FramedAllocation<Prefix>::DeallocateFromInner(Allocator* allocator,
                                                   void* inner_alloc) {
  PW_DCHECK(allocator != nullptr);
  PW_DCHECK(inner_alloc != nullptr);

  void* outer_alloc = nullptr;

  {
    Header& header = GetHeaderFromInner(inner_alloc);
    outer_alloc = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(inner_alloc) - header.offset);
    std::destroy_at<Header>(&header);
  }

  allocator->Deallocate(outer_alloc);
}

struct CoroAllocationHeader {
  Allocator* allocator;
};

using CoroFramedAllocation = FramedAllocation<CoroAllocationHeader>;

}  // namespace

void* CoroPromiseBase::SharedNew(CoroContext coro_cx,
                                 std::size_t size,
                                 std::align_val_t align) noexcept {
  const auto coro_layout =
      allocator::Layout(size, static_cast<std::size_t>(align));

  const auto framed_layout = CoroFramedAllocation::GetOuterLayout(coro_layout);
  PW_LOG_DEBUG("Allocating %zu B for %zu B coroutine with %zu B alignment",
               framed_layout.size(),
               coro_layout.size(),
               static_cast<std::size_t>(align));

  // Allocate space for the header and the opaque coroutine state.
  Allocator* const allocator = &coro_cx.allocator();
  auto [header, coro_alloc] =
      CoroFramedAllocation::Allocate(allocator, coro_layout);
  if (coro_alloc == nullptr) {
    return nullptr;
  }

  // Store the allocator in the header for use in deallocation.
  header->allocator = allocator;

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

  auto& header = CoroFramedAllocation::GetPrefixFromInner(ptr);
  PW_DCHECK(header.allocator != nullptr);
  CoroFramedAllocation::DeallocateFromInner(header.allocator, ptr);
}

}  // namespace pw::async2::internal
