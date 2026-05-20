// Copyright 2025 The Pigweed Authors
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

#include "pw_allocator/allocator.h"
#include "pw_allocator/first_fit.h"

namespace pw::allocator::test {

/// @submodule{pw_allocator,impl_test}

/// Forwarding allocator for injecting failures. Forwards calls to a real
/// allocator implementation, or artificially fails if requested.
///
/// @warning FaultInjectingAllocator is NOT thread safe, even if used with
/// `SynchronizedAllocator`.
class FaultInjectingAllocator : public pw::Allocator {
 public:
  explicit constexpr FaultInjectingAllocator(Allocator& allocator)
      : Allocator(allocator.capabilities()),
        allocator_(allocator),
        allow_allocate_(true),
        allow_resize_(true),
        allow_reallocate_(true) {}

  /// Forward `Allocate`, `Resize`, and `Reallocate` calls to the allocator.
  void EnableAll() {
    allow_allocate_ = allow_resize_ = allow_reallocate_ = true;
  }

  /// Return errors for `Allocate`, `Resize`, and `Reallocate` calls without
  /// forwarding to the allocator.
  void DisableAll() {
    allow_allocate_ = allow_resize_ = allow_reallocate_ = false;
  }

  /// Forward `Allocate` calls to the allocator.
  void EnableAllocate() { allow_allocate_ = true; }

  /// Return `nullptr` for `Allocate` calls.
  void DisableAllocate() { allow_allocate_ = false; }

  /// Forward `Resize` calls to the allocator.
  void EnableResize() { allow_resize_ = true; }

  /// Return `false` for `Resize` calls.
  void DisableResize() { allow_resize_ = false; }

  /// Forward `Reallocate` calls to the allocator.
  void EnableReallocate() { allow_reallocate_ = true; }

  /// Return `nullptr` for `Reallocate` calls.
  void DisableReallocate() { allow_reallocate_ = false; }

  /// Returns a reference to the wrapped allocator.
  Allocator& real_allocator() { return allocator_; }

 protected:
  void* DoAllocate(Layout layout) override {
    return allow_allocate_ ? allocator_.Allocate(layout) : nullptr;
  }
  void DoDeallocate(void* ptr) override { allocator_.Deallocate(ptr); }

  bool DoResize(void* ptr, size_t new_size) override {
    return allow_resize_ && allocator_.Resize(ptr, new_size);
  }

  void* DoReallocate(void* ptr, Layout new_layout) override {
    return allow_reallocate_ ? allocator_.Reallocate(ptr, new_layout) : nullptr;
  }

 private:
  Allocator& allocator_;

  // Flags for whether to allow calls to pass through.
  bool allow_allocate_;
  bool allow_resize_;
  bool allow_reallocate_;
};

/// @endsubmodule

}  // namespace pw::allocator::test
