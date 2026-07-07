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
#pragma once

#include <cstddef>

#include "pw_allocator/abstract_allocator.h"
#include "pw_allocator/capability.h"
#include "pw_bytes/span.h"

namespace pw::allocator {
namespace internal {

/// Type-erased base class to allow destroying a generic instance of ``Owned``.
class GenericOwned {
 public:
  virtual ~GenericOwned() = default;
  void set_next(GenericOwned* next) { next_ = next; }
  void Destroy() { DoDestroy(); }

 private:
  virtual void DoDestroy() = 0;

  GenericOwned* next_ = nullptr;
};

/// Wrapper class that invokes an object's destructor when a BumpAllocator is
/// destroyed.
template <typename T>
class Owned : public GenericOwned {
 public:
  void set_object(T* object) { object_ = object; }

 private:
  void DoDestroy() override;

  T* object_ = nullptr;
};

}  // namespace internal

/// @submodule{pw_allocator,concrete}

/// Allocator that does not automatically delete.
///
/// A "bump" or "arena" allocator provides memory by simply incrementing a
/// pointer to a memory region in order to "allocate" memory for requests, and
/// doing nothing on deallocation. All memory is freed only when the allocator
/// itself is destroyed. As a result, this allocator is extremely fast.
///
/// Bump allocators are useful when short-lived to allocate objects from small
/// buffers with almost zero overhead. Since memory is not deallocated, a bump
/// allocator with a longer lifespan than any of its allocations will end up
/// holding unusable memory. An example of a good use case might be decoding
/// RPC messages that may require a variable amount of space only for as long as
/// it takes to decode a single message.
///
/// On destruction, the destructors for any objects allocated using `New` or
/// `MakeUnique` are NOT called. To have these destructors invoked, you can
/// allocate "owned" objects using `NewOwned` and `MakeUniqueOwned`. This adds a
/// small amount of overhead to the allocation.
class BumpAllocator : public AbstractAllocator {
 public:
  static constexpr Capabilities kCapabilities = kSkipsDestroy;

  /// Constructs a BumpAllocator without initializing it.
  constexpr BumpAllocator() : AbstractAllocator(kCapabilities) {}

  /// Constructs a BumpAllocator and initializes it.
  explicit BumpAllocator(ByteSpan region) : BumpAllocator() { Init(region); }

  ~BumpAllocator() override { Reset(); }

  /// Sets the memory region to be used by the allocator.
  void Init(ByteSpan region);

  constexpr size_t remaining() const { return remaining_.size(); }

  /// Constructs an "owned" object of type `T` from the given `args`
  ///
  /// Owned objects will have their destructors invoked when the allocator goes
  /// out of scope.
  ///
  /// The return value is nullable, as allocating memory for the object may
  /// fail. Callers must check for this error before using the resulting
  /// pointer.
  ///
  /// @param[in]  args        Arguments passed to the object constructor.
  template <typename T, int&... kExplicitGuard, typename... Args>
  T* NewOwned(Args&&... args);

  /// Constructs and object of type `T` from the given `args`, and wraps it in a
  /// `UniquePtr`
  ///
  /// Owned objects will have their destructors invoked when the allocator goes
  /// out of scope.
  ///
  /// The returned value may contain null if allocating memory for the object
  /// fails. Callers must check for null before using the `UniquePtr`.
  ///
  /// @param[in]  args        Arguments passed to the object constructor.
  template <typename T, int&... kExplicitGuard, typename... Args>
  [[nodiscard]] UniquePtr<T> MakeUniqueOwned(Args&&... args);

 protected:
  /// @copydoc Allocator::Allocate
  void* DoAllocate(Layout layout) override;

  /// @copydoc Deallocator::Deallocate
  void DoDeallocate(void* ptr) override;

  /// @copydoc Allocator::GetAllocated
  size_t DoGetAllocated() const override { return allocated_; }

 private:
  /// Frees any owned objects and discards remaining memory.
  void Reset();

  size_t allocated_ = 0;
  ByteSpan remaining_;
  internal::GenericOwned* owned_ = nullptr;
};

/// @}

// Template method implementations.

namespace internal {

template <typename T>
void Owned<T>::DoDestroy() {
  if (object_ != nullptr) {
    std::destroy_at(object_);
    object_ = nullptr;
  }
}

}  // namespace internal

template <typename T, int&... kExplicitGuard, typename... Args>
T* BumpAllocator::NewOwned(Args&&... args) {
  internal::Owned<T>* owned = New<internal::Owned<T>>();
  T* ptr = owned != nullptr ? New<T>(std::forward<Args>(args)...) : nullptr;
  if (ptr != nullptr) {
    owned->set_object(ptr);
    owned->set_next(owned_);
    owned_ = owned;
  }
  return ptr;
}

template <typename T, int&... kExplicitGuard, typename... Args>
UniquePtr<T> BumpAllocator::MakeUniqueOwned(Args&&... args) {
  return UniquePtr<T>(NewOwned<T>(std::forward<Args>(args)...), *this);
}

}  // namespace pw::allocator
