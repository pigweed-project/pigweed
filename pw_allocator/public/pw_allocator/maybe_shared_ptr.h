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

#include <cstddef>
#include <type_traits>
#include <utility>

#include "pw_allocator/allocator.h"
#include "pw_allocator/shared_ptr.h"

namespace pw {

/// @submodule{pw_allocator,core}

// Forward declarations.
template <typename T>
class MaybeSharedPtr;

template <typename To, typename From>
constexpr MaybeSharedPtr<To> static_pointer_cast(
    const MaybeSharedPtr<From>& p) noexcept;

template <typename To, typename From>
constexpr MaybeSharedPtr<To> const_pointer_cast(
    const MaybeSharedPtr<From>& p) noexcept;

/// A smart pointer whose sole purpose is to support sharing between dynamic
/// and statically allocated code.
///
/// `MaybeSharedPtr` can be constructed from any `pw::SharedPtr` (retaining its
/// shared ownership semantics) or via `MaybeSharedPtr::Unowned(...)` (holding a
/// non-owning borrowed reference to a statically allocated object).
///
/// Unlike `SharedPtr`, unowned `MaybeSharedPtr` instances do not participate in
/// reference counting, will not destroy the object upon leaving scope, and
/// avoid dynamic control block allocations.
///
/// `MaybeSharedPtr` explicitly does not support array types (e.g.
/// `pw::MaybeSharedPtr<T[]>`). In `pw::SharedPtr<T[]>`, the array size is
/// stored within the dynamic control block, which unowned instances lack. To
/// pass unowned contiguous buffers, prefer `pw::span`.
///
/// @tparam   T   The type being pointed to. Must not be an array type.
template <typename T>
class MaybeSharedPtr final {
  static_assert(
      !std::is_array_v<T>,
      "MaybeSharedPtr does not support array types because array "
      "sizes are stored in the control block, which unowned instances "
      "lack. Use pw::span or pw::SharedPtr<T[]> instead.");

 public:
  using element_type = typename SharedPtr<T>::element_type;

  /// Creates an empty (`nullptr`) instance.
  constexpr MaybeSharedPtr() noexcept = default;

  /// Creates an empty (`nullptr`) instance.
  constexpr MaybeSharedPtr(std::nullptr_t) noexcept : MaybeSharedPtr() {}

  /// Copy-constructs a `MaybeSharedPtr<T>` from another `MaybeSharedPtr<T>`.
  constexpr MaybeSharedPtr(const MaybeSharedPtr& other) noexcept = default;

  /// Move-constructs a `MaybeSharedPtr<T>` from another `MaybeSharedPtr<T>`.
  MaybeSharedPtr(MaybeSharedPtr&& other) noexcept = default;

  /// Copy-assigns a `MaybeSharedPtr<T>` from another `MaybeSharedPtr<T>`.
  constexpr MaybeSharedPtr& operator=(const MaybeSharedPtr& other) noexcept {
    if (this == &other) {
      return *this;
    }
    ptr_ = other.ptr_;
    return *this;
  }

  /// Move-assigns a `MaybeSharedPtr<T>` from another `MaybeSharedPtr<T>`.
  MaybeSharedPtr& operator=(MaybeSharedPtr&& other) noexcept {
    if (this == &other) {
      return *this;
    }
    ptr_ = std::move(other.ptr_);
    return *this;
  }

  /// Copy-constructs a `MaybeSharedPtr<T>` from a `MaybeSharedPtr<U>`.
  ///
  /// This allows converting construction where `T` is a base class of `U`.
  template <typename U,
            typename = std::enable_if_t<std::is_assignable_v<T*&, U*>>>
  constexpr MaybeSharedPtr(const MaybeSharedPtr<U>& other) noexcept
      : ptr_(other.ptr_) {}

  /// Move-constructs a `MaybeSharedPtr<T>` from a `MaybeSharedPtr<U>`.
  ///
  /// This allows converting construction where `T` is a base class of `U`.
  template <typename U,
            typename = std::enable_if_t<std::is_assignable_v<T*&, U*>>>
  MaybeSharedPtr(MaybeSharedPtr<U>&& other) noexcept
      : ptr_(std::move(other.ptr_)) {}

  /// Implicitly constructs a `MaybeSharedPtr<T>` from an owned `SharedPtr<U>`.
  ///
  /// This allows converting construction where `T` is a base class of `U`.
  template <typename U,
            typename = std::enable_if_t<std::is_assignable_v<T*&, U*>>>
  constexpr MaybeSharedPtr(const SharedPtr<U>& other) noexcept : ptr_(other) {}

  /// Implicitly move-constructs a `MaybeSharedPtr<T>` from an owned
  /// `SharedPtr<U>`.
  template <typename U,
            typename = std::enable_if_t<std::is_assignable_v<T*&, U*>>>
  MaybeSharedPtr(SharedPtr<U>&& other) noexcept : ptr_(std::move(other)) {}

  /// Copy-assigns a `MaybeSharedPtr<T>` from a `MaybeSharedPtr<U>`.
  template <typename U,
            typename = std::enable_if_t<std::is_assignable_v<T*&, U*>>>
  constexpr MaybeSharedPtr& operator=(const MaybeSharedPtr<U>& other) noexcept {
    if (static_cast<const void*>(this) == static_cast<const void*>(&other)) {
      return *this;
    }
    ptr_ = other.ptr_;
    return *this;
  }

  /// Move-assigns a `MaybeSharedPtr<T>` from a `MaybeSharedPtr<U>`.
  template <typename U,
            typename = std::enable_if_t<std::is_assignable_v<T*&, U*>>>
  MaybeSharedPtr& operator=(MaybeSharedPtr<U>&& other) noexcept {
    if (static_cast<const void*>(this) == static_cast<const void*>(&other)) {
      return *this;
    }
    ptr_ = std::move(other.ptr_);
    return *this;
  }

  /// Copy-assigns a `MaybeSharedPtr<T>` from a `SharedPtr<U>`.
  template <typename U,
            typename = std::enable_if_t<std::is_assignable_v<T*&, U*>>>
  constexpr MaybeSharedPtr& operator=(const SharedPtr<U>& other) noexcept {
    ptr_ = other;
    return *this;
  }

  /// Move-assigns a `MaybeSharedPtr<T>` from a `SharedPtr<U>`.
  template <typename U,
            typename = std::enable_if_t<std::is_assignable_v<T*&, U*>>>
  MaybeSharedPtr& operator=(SharedPtr<U>&& other) noexcept {
    ptr_ = std::move(other);
    return *this;
  }

  /// Sets this `MaybeSharedPtr` to null, releasing any held value.
  MaybeSharedPtr& operator=(std::nullptr_t) noexcept {
    reset();
    return *this;
  }

  // Factories

  /// Constructs an unowned `MaybeSharedPtr` referencing an lvalue object.
  static constexpr MaybeSharedPtr Unowned(element_type& value) noexcept {
    return MaybeSharedPtr(&value, nullptr);
  }

  /// Disallows constructing an unowned `MaybeSharedPtr` from a temporary.
  static constexpr MaybeSharedPtr Unowned(const element_type&&) = delete;

  // Observers

  /// Returns whether this pointer owns a dynamically allocated object backed
  /// by a control block.
  [[nodiscard]] constexpr bool is_owned() const noexcept {
    return ptr_.control_block_ != nullptr;
  }

  /// `operator bool` is deleted to match `SharedPtr` and `UniquePtr`
  /// conventions. Use `ptr == nullptr` or `ptr != nullptr`.
  explicit operator bool() const = delete;

  /// Returns the underlying (possibly null) pointer.
  constexpr element_type* get() const noexcept { return ptr_.get(); }

  /// Permits access to members of `T`.
  constexpr element_type* operator->() const noexcept {
    return ptr_.operator->();
  }

  /// Returns a reference to the underlying object.
  constexpr element_type& operator*() const noexcept { return *ptr_; }

  /// Returns the allocator that owns this object, or nullptr if empty or
  /// unowned.
  [[nodiscard]] Allocator* allocator() const noexcept {
    return ptr_.allocator();
  }

  // Conversions

  /// Explicit conversion operator for downcasting.
  template <typename U>
  constexpr explicit operator MaybeSharedPtr<U>() const noexcept {
    return static_pointer_cast<U>(*this);
  }

  /// Creates a new `MaybeSharedPtr` by static casting the given pointer.
  template <typename To, typename From>
  friend constexpr MaybeSharedPtr<To> static_pointer_cast(
      const MaybeSharedPtr<From>& p) noexcept;

  /// Creates a new `MaybeSharedPtr` by const casting the given pointer.
  template <typename To, typename From>
  friend constexpr MaybeSharedPtr<To> const_pointer_cast(
      const MaybeSharedPtr<From>& p) noexcept;

  // Mutators

  /// Resets this object to an empty state (`nullptr`).
  ///
  /// The implications depend on whether this instance is owned or unowned:
  ///
  /// - **Owned** (`SharedPtr`): Decrements the shared reference count. If this
  ///   was the last shared pointer referencing the object, the object is
  ///   destroyed. If no weak pointers remain to the control block, memory is
  ///   deallocated by its allocator.
  /// - **Unowned** (`Unowned()`): Clears the pointer to `nullptr`. The
  ///   referenced object is NOT destroyed, and NO memory is freed.
  /// - **Empty** (`nullptr`): Has no effect.
  void reset() noexcept { ptr_.reset(); }

  /// Swaps the managed pointer with another object.
  void swap(MaybeSharedPtr& other) noexcept { ptr_.swap(other.ptr_); }

  // Comparisons

  [[nodiscard]] friend constexpr bool operator==(const MaybeSharedPtr& lhs,
                                                 std::nullptr_t) noexcept {
    return lhs.get() == nullptr;
  }
  [[nodiscard]] friend constexpr bool operator==(
      std::nullptr_t, const MaybeSharedPtr& rhs) noexcept {
    return rhs.get() == nullptr;
  }
  [[nodiscard]] friend constexpr bool operator!=(const MaybeSharedPtr& lhs,
                                                 std::nullptr_t) noexcept {
    return lhs.get() != nullptr;
  }
  [[nodiscard]] friend constexpr bool operator!=(
      std::nullptr_t, const MaybeSharedPtr& rhs) noexcept {
    return rhs.get() != nullptr;
  }

  template <typename U>
  [[nodiscard]] friend constexpr bool operator==(
      const MaybeSharedPtr& lhs, const MaybeSharedPtr<U>& rhs) noexcept {
    return lhs.get() == rhs.get();
  }

  template <typename U>
  [[nodiscard]] friend constexpr bool operator!=(
      const MaybeSharedPtr& lhs, const MaybeSharedPtr<U>& rhs) noexcept {
    return lhs.get() != rhs.get();
  }

  template <typename U>
  [[nodiscard]] friend constexpr bool operator==(
      const MaybeSharedPtr& lhs, const SharedPtr<U>& rhs) noexcept {
    return lhs.get() == rhs.get();
  }

  template <typename U>
  [[nodiscard]] friend constexpr bool operator==(
      const SharedPtr<U>& lhs, const MaybeSharedPtr& rhs) noexcept {
    return lhs.get() == rhs.get();
  }

  template <typename U>
  [[nodiscard]] friend constexpr bool operator!=(
      const MaybeSharedPtr& lhs, const SharedPtr<U>& rhs) noexcept {
    return lhs.get() != rhs.get();
  }

  template <typename U>
  [[nodiscard]] friend constexpr bool operator!=(
      const SharedPtr<U>& lhs, const MaybeSharedPtr& rhs) noexcept {
    return lhs.get() != rhs.get();
  }

 private:
  template <typename>
  friend class MaybeSharedPtr;

  /// Private constructor for unowned initialization.
  constexpr MaybeSharedPtr(element_type* value,
                           allocator::internal::ControlBlock* control_block)
      : ptr_(value, control_block) {}

  SharedPtr<T> ptr_;
};

template <typename To, typename From>
constexpr MaybeSharedPtr<To> static_pointer_cast(
    const MaybeSharedPtr<From>& p) noexcept {
  return MaybeSharedPtr<To>(static_pointer_cast<To>(p.ptr_));
}

template <typename To, typename From>
constexpr MaybeSharedPtr<To> const_pointer_cast(
    const MaybeSharedPtr<From>& p) noexcept {
  return MaybeSharedPtr<To>(const_pointer_cast<To>(p.ptr_));
}

/// Constructs an unowned `MaybeSharedPtr` referencing an lvalue object.
template <typename T>
constexpr MaybeSharedPtr<T> Unowned(T& object) noexcept {
  return MaybeSharedPtr<T>::Unowned(object);
}

/// Disallows constructing an unowned `MaybeSharedPtr` from a temporary.
template <typename T>
MaybeSharedPtr<T> Unowned(const T&&) = delete;

/// @endsubmodule

}  // namespace pw

#endif  // PW_ALLOCATOR_HAS_ATOMICS
