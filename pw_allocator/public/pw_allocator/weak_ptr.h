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

#include "pw_allocator/config.h"

// TODO(b/402489948): Remove when portable atomics are provided by `pw_atomic`.
#if PW_ALLOCATOR_HAS_ATOMICS

#include <cstddef>
#include <utility>

#include "pw_allocator/internal/control_block.h"
#include "pw_allocator/internal/managed_ptr.h"
#include "pw_allocator/shared_ptr.h"

namespace pw {

/// @submodule{pw_allocator,core}

// Forward declarations.
template <typename T>
class WeakPtr;

/// A `std::weak_ptr<T>`-like type that integrates with `pw::SharedPtr`.
///
/// @tparam   T   The type being pointed to. This may be an array type, e.g.
///           `pw::WeakPtr<T[]>`.
template <typename T>
class WeakPtr final : public ::pw::allocator::internal::WeakManagedPtr<T> {
 private:
  using Base = ::pw::allocator::internal::WeakManagedPtr<T>;
  using ControlBlock = ::pw::allocator::internal::ControlBlock;

 public:
  using element_type = typename Base::element_type;

  /// Creates an empty (`nullptr`) instance.
  constexpr WeakPtr() noexcept = default;

  /// Creates an empty (`nullptr`) instance.
  constexpr WeakPtr(std::nullptr_t) noexcept : WeakPtr() {}

  /// Copy-constructs a `WeakPtr<T>` from a `WeakPtr<T>`.
  WeakPtr(const WeakPtr& other) noexcept : WeakPtr() { *this = other; }

  /// Copy-constructs a `WeakPtr<T>` from a `WeakPtr<U>`.
  ///
  /// This allows not only pure copy construction where `T == U`, but also
  /// converting construction where `T` is a base class of `U`.
  template <typename U,
            typename = std::enable_if_t<std::is_assignable_v<T*&, U*>>>
  WeakPtr(const WeakPtr<U>& other) noexcept : WeakPtr() {
    *this = other;
  }

  /// Copy-constructs a `WeakPtr<T>` from a `SharedPtr<U>`.
  ///
  /// This allows converting copy construction where `T` is a base class of `U`.
  template <typename U,
            typename = std::enable_if_t<std::is_assignable_v<T*&, U*>>>
  WeakPtr(const SharedPtr<U>& other) noexcept : WeakPtr() {
    *this = other;
  }

  /// Move-constructs a `WeakPtr<T>` from a `WeakPtr<T>`.
  WeakPtr(WeakPtr&& other) noexcept : WeakPtr() { *this = std::move(other); }

  /// Move-constructs a `WeakPtr<T>` from a `WeakPtr<U>`.
  ///
  /// This allows not only pure move construction where `T == U`, but also
  /// converting construction where `T` is a base class of `U`.
  template <typename U,
            typename = std::enable_if_t<std::is_assignable_v<T*&, U*>>>
  WeakPtr(WeakPtr<U>&& other) noexcept : WeakPtr() {
    *this = std::move(other);
  }

  ~WeakPtr() { reset(); }

  /// Copy-assigns a `WeakPtr<T>` from a `WeakPtr<T>`.
  WeakPtr& operator=(const WeakPtr& other) noexcept {
    operator= <T>(other);
    return *this;
  }

  /// Copy-assigns a `WeakPtr<T>` from a `WeakPtr<U>`.
  ///
  /// This allows not only pure copy assignment where `T == U`, but also
  /// converting assignment where `T` is a base class of `U`.
  template <typename U,
            typename = std::enable_if_t<std::is_assignable_v<T*&, U*>>>
  WeakPtr& operator=(const WeakPtr<U>& other) noexcept;

  /// Copy-assigns a `WeakPtr<T>` from a `SharedPtr<U>`.
  ///
  /// This allows converting copy assignment where `T` is a base class of `U`.
  template <typename U,
            typename = std::enable_if_t<std::is_assignable_v<T*&, U*>>>
  WeakPtr& operator=(const SharedPtr<U>& other) noexcept;

  /// Move-assigns a `WeakPtr<T>` from a `WeakPtr<T>`.
  WeakPtr& operator=(WeakPtr&& other) noexcept {
    operator= <T>(std::move(other));
    return *this;
  }

  /// Move-assigns a `WeakPtr<T>` from a `WeakPtr<U>`.
  ///
  /// This allows not only pure move assignment where `T == U`, but also
  /// converting assignment where `T` is a base class of `U`.
  template <typename U,
            typename = std::enable_if_t<std::is_assignable_v<T*&, U*>>>
  WeakPtr& operator=(WeakPtr<U>&& other) noexcept;

  /// Explicit conversion operator for downcasting.
  ///
  /// If an arbitrary type `A` derives from another type `B`, a shared or weak
  /// pointer to `A` can be automatically upcast to one of type `B`. This
  /// operator performs the reverse operation with an explicit cast.
  ///
  /// @code{.cpp}
  /// pw::SharedPtr<A> a1 = allocator.MakeShared<A>();
  /// pw::WeakPtr<B> b = a1;
  /// pw::WeakPtr<A> a2 = static_cast<pw::WeakPtr<A>>(b);
  /// @endcode
  template <typename U,
            typename = std::enable_if_t<std::is_assignable_v<T*&, U*>>>
  explicit operator WeakPtr<U>() const {
    if (control_block_ != nullptr) {
      control_block_->IncrementWeak();
    }
    return WeakPtr<U>(static_cast<U*>(Base::ptr_), control_block_);
  }

  /// Resets this object to an empty state.
  ///
  /// If this is the last shared or weak pointer associated with the control
  /// block, it is deallocated.
  void reset() noexcept;

  /// Swaps the managed pointer and control block of this and another object.
  void swap(WeakPtr& other) noexcept {
    Base::Swap(other);
    std::swap(control_block_, other.control_block_);
  }

  /// Returns the number of shared pointers to the associated object, or 0 if
  /// this object is empty.
  int32_t use_count() const noexcept {
    return control_block_ == nullptr ? 0 : control_block_->num_shared();
  }

  /// Returns true if the associated object has been destroyed; otherwise
  /// returns false.
  bool expired() const noexcept { return use_count() == 0; }

  /// Creates a new `SharedPtr` to the associated object, or an empty
  /// `SharedPtr` if this object is empty.
  SharedPtr<T> Lock() const noexcept;

  /// Checks whether `this` precedes `other` based on an ordering of their
  /// control blocks.
  template <typename PtrType>
  bool owner_before(const PtrType& other) const noexcept {
    return control_block_ < other.control_block();
  }

 private:
  // Allow WeakPtr<T> to access WeakPtr<U> and vice versa.
  template <typename>
  friend class WeakPtr;

  ControlBlock* control_block() const { return control_block_; }

  explicit WeakPtr(element_type* ptr, ControlBlock* control_block) noexcept
      : Base(ptr), control_block_(control_block) {}

  void Release() noexcept {
    Base::Release();
    control_block_ = nullptr;
  }

  ControlBlock* control_block_ = nullptr;
};

// Template implementations.

template <typename T>
template <typename U, typename>
WeakPtr<T>& WeakPtr<T>::operator=(const WeakPtr<U>& other) noexcept {
  Base::template CheckAssignable<U>();
  if (static_cast<const void*>(this) == static_cast<const void*>(&other)) {
    return *this;
  }
  if (other.control_block_ != nullptr) {
    other.control_block_->IncrementWeak();
  }
  reset();
  Base::CopyFrom(other);
  control_block_ = other.control_block_;
  return *this;
}

template <typename T>
template <typename U, typename>
WeakPtr<T>& WeakPtr<T>::operator=(const SharedPtr<U>& other) noexcept {
  Base::template CheckAssignable<U>();
  if (other.control_block_ != nullptr) {
    other.control_block_->IncrementWeak();
  }
  reset();
  Base::CopyFrom(other);
  control_block_ = other.control_block_;
  return *this;
}

template <typename T>
template <typename U, typename>
WeakPtr<T>& WeakPtr<T>::operator=(WeakPtr<U>&& other) noexcept {
  Base::template CheckAssignable<U>();
  if (static_cast<const void*>(this) == static_cast<const void*>(&other)) {
    return *this;
  }
  reset();
  Base::CopyFrom(other);
  control_block_ = other.control_block_;
  other.Release();
  return *this;
}

template <typename T>
void WeakPtr<T>::reset() noexcept {
  if (control_block_ == nullptr) {
    Release();
    return;
  }
  if (control_block_->DecrementWeak() == ControlBlock::Action::kFree) {
    Allocator* allocator = control_block_->allocator();
    std::destroy_at(control_block_);
    Base::Deallocate(allocator, control_block_);
  }
  Release();
}

template <typename T>
SharedPtr<T> WeakPtr<T>::Lock() const noexcept {
  if (control_block_ == nullptr || !control_block_->IncrementShared()) {
    return SharedPtr<T>();
  }
  return SharedPtr<T>(Base::ptr_, control_block_);
}

/// @}

}  // namespace pw

// TODO(b/402489948): Remove when portable atomics are provided by `pw_atomic`.
#endif  // PW_ALLOCATOR_HAS_ATOMICS
