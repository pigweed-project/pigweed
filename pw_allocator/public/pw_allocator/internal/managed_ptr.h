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

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

#include "pw_allocator/capability.h"
#include "pw_allocator/hardening.h"

namespace pw {

// Forward declarations.
class Allocator;
class Deallocator;

namespace allocator::internal {

// Empty struct used in place of the `size_` field when the pointer type is not
// an array type.
struct Empty {};

/// This class simply provides type-erased static methods to check capabilities
/// and manage memory in a managed pointer. This allows `ManagedPtr<T>` to
/// be declared without a complete declaration of `Allocator` or
/// `Deallocator`, breaking the dependency cycle between `ManagedPtr<T>` and
/// `Allocator`methods including `MakeUnique<T>()` and `MakeShared<T>()`.
class BaseManagedPtr {
 protected:
  static bool HasCapability(Deallocator* deallocator, Capability capability);
  static void Deallocate(Deallocator* deallocator, void* ptr);
  static bool Resize(Allocator* deallocator, void* ptr, size_t new_size);
};

/// This class extends `BaseManagerPtr` to provide type checking for methods
/// including the assignment operators. It has no concept of ownership of the
/// object or its memory and is thus "weak".
template <typename T>
class WeakManagedPtr : public BaseManagedPtr {
 protected:
  using element_type = std::conditional_t<std::is_array_v<T>,
                                          typename std::remove_extent<T>::type,
                                          T>;

  template <typename U>
  constexpr void CheckAssignable() {
    static_assert(
        std::is_assignable_v<element_type*&,
                             typename WeakManagedPtr<U>::element_type*>,
        "Attempted to construct a WeakManagedPtr<T> from a WeakManagedPtr<U> "
        "where U* is not assignable to T*.");
  }

 private:
  // Allow WeakManagedPtr<T> to access WeakManagedPtr<U> and vice versa.
  template <typename>
  friend class WeakManagedPtr;
};

/// Smart pointer to an object in memory provided by a `Deallocator`.
///
/// This type provides methods for accessing and destroying allocated objects
/// wrapped by RAII-style smart pointers. It is not designed to be used
/// directly, and instead should be extend to create shart pointers that call
/// the base methods at the appropriate time, e.g. `UniquePtr` calls
/// `Destroy` as part of `Reset`.
template <typename T>
class ManagedPtr : public WeakManagedPtr<T> {
 protected:
  using Base = WeakManagedPtr<T>;
  using element_type = typename Base::element_type;

 public:
  // Not copyable.
  ManagedPtr(const ManagedPtr&) = delete;
  ManagedPtr& operator=(const ManagedPtr&) = delete;

  /// `operator bool` is not provided in order to ensure that there is no
  /// confusion surrounding `if (foo)` vs. `if (*foo)`.
  ///
  /// `nullptr` checking should instead use `if (foo == nullptr)`.
  explicit operator bool() const = delete;

  /// Returns the underlying (possibly null) pointer.
  constexpr element_type* get() const noexcept { return ptr_; }

  /// Permits accesses to members of `T` via `ptr->Member`.
  ///
  /// The behavior of this operation is undefined if this `ManagedPtr` is in
  /// an "empty" (`nullptr`) state.
  constexpr element_type* operator->() const noexcept {
    if constexpr (Hardening::kIncludesRobustChecks) {
      PW_ASSERT(ptr_ != nullptr);
    }
    return ptr_;
  }

  /// Returns a reference to any underlying object.
  ///
  /// The behavior of this operation is undefined if this `ManagedPtr` is in
  /// an "empty" (`nullptr`) state.
  constexpr element_type& operator*() const {
    if constexpr (Hardening::kIncludesRobustChecks) {
      PW_ASSERT(ptr_ != nullptr);
    }
    return *ptr_;
  }

  /// Returns a reference to the element at the given index.
  ///
  /// The behavior of this operation is undefined if this `ManagedPtr` is in
  /// an "empty" (`nullptr`) state.
  constexpr element_type& operator[](size_t index) const {
    static_assert(std::is_array_v<T>,
                  "operator[] cannot be called with non-array types");
    if constexpr (Hardening::kIncludesRobustChecks) {
      PW_ASSERT(ptr_ != nullptr);
    }
    return ptr_[index];
  }

 protected:
  constexpr ManagedPtr() = default;

  /// Constructs a `ManagedPtr` from an already-allocated object and size.
  constexpr explicit ManagedPtr(element_type* ptr) : ptr_(ptr) {}

  /// Returns whether this `ManagedPtr` is in an "empty" (`nullptr`) state.
  [[nodiscard]] bool Equals(std::nullptr_t) const { return ptr_ == nullptr; }

  /// Returns whether this `ManagedPtr` points at the same object.
  [[nodiscard]] bool Equals(const ManagedPtr& other) const {
    return ptr_ == other.ptr_;
  }

  /// Copies details from another object without releasing it.
  template <typename U>
  void CopyFrom(const ManagedPtr<U>& other) {
    Base::template CheckAssignable<U>();
    ptr_ = other.ptr_;
  }

  /// Releases an object from being managed by the `ManagedPtr`.
  ///
  /// After this call, the object will have an "empty" (`nullptr`) pointer.
  element_type* Release() {
    element_type* ptr = ptr_;
    ptr_ = nullptr;
    return ptr;
  }

  /// Swaps the managed pointer and deallocator of this and another object.
  void Swap(ManagedPtr& other) noexcept { std::swap(ptr_, other.ptr_); }

  /// Destroys the objects in this object's memory without deallocating it.
  ///
  /// This will fail to compile if it is called with an array type.
  void Destroy() {
    static_assert(!std::is_array_v<T>,
                  "Destroy() cannot be called with array types");
    std::destroy_at(ptr_);
  }

  /// Destroys the objects in this object's memory without deallocating it.
  ///
  /// This will fail to compile if it is called with a non-array type.
  void Destroy(size_t size) {
    static_assert(std::is_array_v<T>,
                  "Destroy(size_t) cannot be called with non-array types");
    std::destroy_n(ptr_, size);
  }

 private:
  // Allow ManagedPtr<T> to access ManagedPtr<U> and vice versa.
  template <typename>
  friend class ManagedPtr;

  /// A pointer to the managed object.
  element_type* ptr_ = nullptr;
};

}  // namespace allocator::internal
}  // namespace pw

/// Returns whether this `ManagedPtr` is in an "empty" (`nullptr`) state.
template <typename T>
bool operator==(std::nullptr_t,
                const pw::allocator::internal::ManagedPtr<T>& ptr) {
  return ptr == nullptr;
}

/// Returns whether this `ManagedPtr` is not in an "empty" (`nullptr`)
/// state.
template <typename T>
bool operator!=(std::nullptr_t,
                const pw::allocator::internal::ManagedPtr<T>& ptr) {
  return ptr != nullptr;
}
