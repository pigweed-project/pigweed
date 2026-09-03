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
#pragma once

#include <cstddef>
#include <optional>

#include "pw_allocator/allocator.h"
#include "pw_allocator/layout.h"
#include "pw_result/result.h"

namespace pw::allocator {

/// An allocator that is implemented by forwarding calls to another allocator.
///
/// This type can be used as a generic base type for forwarding allocators that
/// provide additional behavior to another allocator. It provides default
/// implementations of the virtual methods from `Deallocator` and `Allocator`,
/// allowing authors of derived types to avoid boilerplate.
///
/// @note It is an error for a derived type to override any of the methods below
/// with an implementation that calls any of the corresponding public methods.
/// If it does, the public, NVI-style method will dispatch to the derived type's
/// override, leading to an infinite recursion. Instead, use the `allocator`
/// accessor or call this type's implementation methods directly:
///
/// @code{.cpp}
/// class MyAllocator : public pw::allocator::ForwardingAllocator {
///  private:
///   void* Allocate(pw::allocator::Layout layout) override {
///     // INCORRECT! This will infinitely recurse.
///     // return pw::allocator::ForwardingAllocator::Allocate(layout);
///
///     // Correct: This dispatches to the wrapped allocator.
///     return allocator().Allocate(layout);
///   }
///
///   void Deallocate(void* ptr) override {
///     // Also correct: This dispatches to the wrapped allocator.
///     pw::allocator::ForwardingAllocator::DoDeallocate(ptr);
///   }
/// }
/// @endcode
class ForwardingAllocator : public pw::Allocator {
 protected:
  /// Creates a forwarding allocator without setting its wrapped allocator.
  ///
  /// This constructor should be used when the forwarding allocator needs to be
  /// created before the allocator being wrapped. The allocator must be set
  /// using `Init` before any other method is called. The given `capabilities`
  /// must match those of the allocator subsequently provided to `Init`.
  constexpr explicit ForwardingAllocator(
      const Capabilities& capabilities) noexcept
      : pw::Allocator(capabilities) {}

  /// Creates a forwarding allocator that wraps a given allocator.
  ///
  /// This constructor should be used when the forwarding allocator is created
  /// after the allocator being wrapped.
  ///
  /// The allocator must remain valid for the lifetime of this object.
  constexpr explicit ForwardingAllocator(pw::Allocator& allocator) noexcept
      : pw::Allocator(allocator.capabilities()), allocator_(&allocator) {}

  /// Sets the allocator being wrapped.
  ///
  /// It is an error to call this method if an allocator was provided to the
  /// constructor. The `capabilities` of the given allocator must match those of
  /// previously provided to the constructor.
  ///
  /// The allocator must remain valid for the lifetime of this object.
  constexpr void Init(pw::Allocator& allocator) {
    PW_ASSERT(allocator_ == nullptr);
    PW_ASSERT(capabilities() == allocator.capabilities());
    allocator_ = &allocator;
  }

  // Accessors

  constexpr pw::Allocator& allocator() {
    PW_ASSERT(allocator_ != nullptr);
    return *allocator_;
  }

  constexpr const pw::Allocator& allocator() const {
    PW_ASSERT(allocator_ != nullptr);
    return *allocator_;
  }

  // API methods

  /// @copydoc Allocator::Allocate
  void* DoAllocate(Layout layout) override {
    return allocator().Allocate(layout);
  }

  /// @copydoc Deallocator::Deallocate
  void DoDeallocate(void* ptr) override { return allocator().Deallocate(ptr); }

  /// @copydoc Allocator::Resize
  bool DoResize(void* ptr, size_t new_size) override {
    return allocator().Resize(ptr, new_size);
  }

  /// @copydoc Allocator::DoBeforeReallocate
  void DoBeforeReallocate(void* ptr, Layout new_layout) override {
    BeforeReallocate(allocator(), ptr, new_layout);
  }

  /// @copydoc Allocator::DoAfterReallocateCopy
  void DoAfterReallocateCopy(void* ptr,
                             Layout new_layout,
                             void* new_ptr) override {
    AfterReallocateCopy(allocator(), ptr, new_layout, new_ptr);
  }

  /// @copydoc Allocator::DoAfterReallocateDone
  void DoAfterReallocateDone(Layout new_layout, void* new_ptr) override {
    AfterReallocateDone(allocator(), new_layout, new_ptr);
  }

  /// @copydoc Allocator::GetAllocated
  size_t DoGetAllocated() const override { return allocator().GetAllocated(); }

  /// @copydoc Allocator::MeasureFragmentation
  std::optional<Fragmentation> DoMeasureFragmentation() const override {
    return allocator().MeasureFragmentation();
  }

  /// @copydoc Deallocator::GetInfo
  Result<Layout> DoGetInfo(InfoType info_type, const void* ptr) const override {
    return Deallocator::GetInfo(allocator(), info_type, ptr);
  }

 private:
  pw::Allocator* allocator_ = nullptr;
};

}  // namespace pw::allocator
