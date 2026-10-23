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

#include "pw_allocator/config.h"

// TODO(b/402489948): Remove when portable atomics are provided by `pw_atomic`.
#if PW_ALLOCATOR_HAS_ATOMICS

#include "pw_allocator/internal/control_block.h"
#include "pw_allocator/shared_ptr.h"
#include "pw_allocator/weak_ptr.h"
#include "pw_assert/assert.h"

namespace pw {

/// @submodule{pw_allocator,core}

/// Allows an object managed by a `pw::SharedPtr` to safely generate additional
/// `pw::SharedPtr` or `pw::WeakPtr` instances to itself.
///
/// Inheriting from `EnableSharedFromThis<T>` provides `shared_from_this()` and
/// `weak_from_this()`, mirroring `std::enable_shared_from_this`.
///
/// ### Preconditions and Limitations
///
/// - The `EnableSharedFromThis<T>` base must be public and unambiguous. With a
///   private, protected, or duplicate base, `weak_from_this()` remains empty
///   and `shared_from_this()` asserts.
/// - `weak_this_` is only initialized by `Allocator::MakeShared` /
///   `SharedPtr::Create` and by `SharedPtr(UniquePtr&)`. It is not initialized
///   for `SharedPtr<T[]>` or internally constructed `SharedPtr`s (e.g.
///   `async2::Task`, `multibuf`).
/// - `shared_from_this()` asserts if called from the object's constructor or
///   destructor.
/// - With an allocator that has the `kSkipsDestroy` capability (such as
///   `BumpAllocator`), the object's destructor never executes, meaning
///   `weak_this_` never releases its weak reference and the control block is
///   never deallocated.
///
/// @tparam   T   The derived class type (CRTP pattern).
template <typename T>
class EnableSharedFromThis {
 public:
  /// Returns a `SharedPtr<T>` that shares ownership of `this`.
  ///
  /// Asserts that `this` is managed by an existing `SharedPtr`.
  SharedPtr<T> shared_from_this() {
    SharedPtr<T> ptr = weak_this_.Lock();
    PW_ASSERT(ptr != nullptr);
    return ptr;
  }

  /// Returns a `SharedPtr<const T>` that shares ownership of `this`.
  ///
  /// Asserts that `this` is managed by an existing `SharedPtr`.
  SharedPtr<const T> shared_from_this() const {
    SharedPtr<const T> ptr = weak_this_.Lock();
    PW_ASSERT(ptr != nullptr);
    return ptr;
  }

  /// Returns a `WeakPtr<T>` tracking `this`.
  ///
  /// Returns an empty `WeakPtr<T>` if `this` is not managed by a `SharedPtr`.
  WeakPtr<T> weak_from_this() noexcept { return weak_this_; }

  /// Returns a `WeakPtr<const T>` tracking `this`.
  ///
  /// Returns an empty `WeakPtr<const T>` if `this` is not managed by a
  /// `SharedPtr`.
  WeakPtr<const T> weak_from_this() const noexcept { return weak_this_; }

 protected:
  constexpr EnableSharedFromThis() noexcept = default;
  constexpr EnableSharedFromThis(const EnableSharedFromThis&) noexcept {}
  constexpr EnableSharedFromThis(EnableSharedFromThis&&) noexcept {}
  constexpr EnableSharedFromThis& operator=(
      const EnableSharedFromThis&) noexcept {
    return *this;
  }
  constexpr EnableSharedFromThis& operator=(EnableSharedFromThis&&) noexcept {
    return *this;
  }
  ~EnableSharedFromThis() = default;

 private:
  template <typename, typename>
  friend struct allocator::internal::HasInitializeWeakThis;

  template <typename U>
  friend void allocator::internal::EnableWeakThis(
      allocator::internal::ControlBlock*, U*);

  /// Used internally by `pw::SharedPtr` to initialize `weak_this_`.
  template <typename U>
  void InitializeWeakThis(
      ::pw::allocator::internal::ControlBlock* control_block, U* ptr) {
    if (weak_this_.expired() && control_block != nullptr) {
      // The control block has an active weak count held by the SharedPtr.
      // Incrementing weak count cannot fail unless the counter overflows.
      if (control_block->IncrementWeak()) {
        weak_this_ = WeakPtr<T>(static_cast<T*>(ptr), control_block);
      }
    }
  }

  WeakPtr<T> weak_this_;
};

/// @endsubmodule

}  // namespace pw

// TODO(b/402489948): Remove when portable atomics are provided by `pw_atomic`.
#endif  // PW_ALLOCATOR_HAS_ATOMICS
