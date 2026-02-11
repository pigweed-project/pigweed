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

#ifndef PW_ALLOCATOR_PUBLIC_PW_ALLOCATOR_SHARED_PTR_H_
#define PW_ALLOCATOR_PUBLIC_PW_ALLOCATOR_SHARED_PTR_H_

#include "pw_allocator/config.h"

// TODO(b/402489948): Remove when portable atomics are provided by `pw_atomic`.
#if PW_ALLOCATOR_HAS_ATOMICS

#include <cstddef>
#include <cstdint>
#include <utility>

#include "pw_allocator/allocator.h"
#include "pw_allocator/deallocator.h"
#include "pw_allocator/internal/control_block.h"
#include "pw_allocator/internal/managed_ptr.h"
#include "pw_allocator/layout.h"
#include "pw_allocator/unique_ptr.h"
#include "pw_multibuf/v2/properties.h"

namespace pw {

/// @submodule{pw_allocator,core}

// Forward declarations.
template <typename T>
class SharedPtr;

template <typename To, typename From>
constexpr SharedPtr<To> static_pointer_cast(const SharedPtr<From>& p) noexcept;

template <typename To, typename From>
constexpr SharedPtr<To> const_pointer_cast(const SharedPtr<From>& p) noexcept;

template <typename T>
class WeakPtr;

namespace multibuf::v2 {

template <Property...>
class BasicMultiBuf;

}  // namespace multibuf::v2

/// A `std::shared_ptr<T>`-like type that integrates with `pw::Allocator`.
///
/// This is a RAII smart pointer that deallocates any memory it points to when
/// every pointer to the same object has gone out of scope.
///
/// Notable differences from `std::shared_ptr<T>` include:
///
/// - It cannot be constructed from a `T*`. Use
///   `Allocator::MakeShared<T>(...)` instead.
/// - Aliasing constructors are not supported to encourage memory safety.
/// - Constructing a `SharedPtr` from a `UniquePtr` takes an lvalue-reference,
///   and is fallible..
///
/// @tparam   T   The type being pointed to. This may be an array type, e.g.
///           `pw::SharedPtr<T[]>`.
template <typename T>
class SharedPtr final : public ::pw::allocator::internal::ManagedPtr<T> {
 private:
  using Base = ::pw::allocator::internal::ManagedPtr<T>;
  using ControlBlock = ::pw::allocator::internal::ControlBlock;

 public:
  using element_type = typename Base::element_type;
  using weak_type = WeakPtr<T>;

  /// Releases any currently-held value.
  ~SharedPtr() { reset(); }

  /// Constructs and object of type `T` from the given `args`, and wraps it in a
  /// `SharedPtr`.
  ///
  /// The returned value may contain null if allocating memory for the object
  /// fails. Callers must check for null before using the `SharedPtr`.
  ///
  /// NOTE: Instances of this type are most commonly constructed using
  /// `Allocator::MakeShared`.
  ///
  /// @param[in]  args         Arguments passed to the object constructor.
  template <typename... Args>
  static SharedPtr Create(Allocator* allocator, Args&&... args);

  /// Constructs an array of `count` objects, and wraps it in a `UniquePtr`
  ///
  /// The returned value may contain null if allocating memory for the object
  /// fails. Callers must check for null before using the `UniquePtr`.
  ///
  /// NOTE: Instances of this type are most commonly constructed using
  /// `Allocator::MakeShared`.
  ///
  /// @param[in]  allocator    Used to allocate memory.
  /// @param[in]  count        Number of objects to allocate.
  /// @param[in]  alignment    Alignment requirement for the array.
  static SharedPtr Create(Allocator* allocator, size_t count, size_t alignment);

  /// Creates an empty (`nullptr`) instance.
  ///
  /// NOTE: Instances of this type are most commonly constructed using
  /// `Allocator::MakeShared`.
  constexpr SharedPtr() noexcept = default;

  /// Constructs a `SharedPtr` from an already-allocated value.
  ///
  /// NOTE: Instances of this type are most commonly constructed using
  /// `Allocator::MakeShared`.
  constexpr SharedPtr(element_type* value, ControlBlock* control_block)
      : Base(value), control_block_(control_block) {}

  /// Creates an empty (`nullptr`) instance.
  ///
  /// NOTE: Instances of this type are most commonly constructed using
  /// `Allocator::MakeShared`.
  constexpr SharedPtr(std::nullptr_t) noexcept : SharedPtr() {}

  /// Copy-constructs a `SharedPtr<T>` from a `SharedPtr<T>`.
  constexpr SharedPtr(const SharedPtr& other) noexcept : SharedPtr() {
    *this = other;
  }

  /// Copy-constructs a `SharedPtr<T>` from a `SharedPtr<U>`.
  ///
  /// This allows not only pure move construction where `T == U`, but also
  /// converting construction where `T` is a base class of `U`.
  template <typename U,
            typename = std::enable_if_t<std::is_assignable_v<T*&, U*>>>
  constexpr SharedPtr(const SharedPtr<U>& other) noexcept : SharedPtr() {
    *this = other;
  }

  /// Move-constructs a `SharedPtr<T>` from a `SharedPtr<U>`.
  ///
  /// This allows not only pure move construction where `T == U`, but also
  /// converting construction where `T` is a base class of `U`, like
  /// `SharedPtr<Base> base(deallocator.MakeShared<Child>());`.
  template <typename U,
            typename = std::enable_if_t<std::is_assignable_v<T*&, U*>>>
  SharedPtr(SharedPtr<U>&& other) noexcept : SharedPtr() {
    *this = std::move(other);
  }

  /// Constructs a `SharedPtr<T>` from a `UniquePtr<T>`.
  ///
  /// NOTE: This constructor differs from `SharedPtr(std::unique_ptr&&)`
  /// in that it takes an l-value reference to the `UniquePtr`. This constructor
  /// must allocate a shared pointer control block. If that allocation fails,
  /// the `UniquePtr` is unmodified and still retains ownership of its object,
  /// and an empty `SharedPtr` is returned. If the allocation succeeds, the
  /// `UniquePtr` will be reset and a `SharedPtr` to the object will be
  /// returned.
  SharedPtr(UniquePtr<T>& owned) noexcept;

  /// Copy-assigns a `SharedPtr<T>` from a `SharedPtr<T>`.
  ///
  /// This operation releases the value currently stored in `this`.
  constexpr SharedPtr& operator=(const SharedPtr& other) noexcept {
    operator= <T>(other);
    return *this;
  }

  /// Copy-assigns a `SharedPtr<T>` from a `SharedPtr<U>`.
  ///
  /// This operation releases the value currently stored in `this`.
  ///
  /// This allows not only pure copy assignment where `T == U`, but also
  /// converting assignment where `T` is a base class of `U`.
  template <typename U,
            typename = std::enable_if_t<std::is_assignable_v<T*&, U*>>>
  constexpr SharedPtr& operator=(const SharedPtr<U>& other) noexcept;

  /// Move-assigns a `SharedPtr<T>` from a `SharedPtr<U>`.
  ///
  /// This operation releases the value currently stored in `this`.
  ///
  /// This allows not only pure move assignment where `T == U`, but also
  /// converting assignment where `T` is a base class of `U`.
  template <typename U,
            typename = std::enable_if_t<std::is_assignable_v<T*&, U*>>>
  SharedPtr& operator=(SharedPtr<U>&& other) noexcept;

  /// Sets this `SharedPtr` to null, releasing any currently-held value.
  ///
  /// After this function returns, this `SharedPtr` will be in an "empty"
  /// (`nullptr`) state until a new value is assigned.
  SharedPtr& operator=(std::nullptr_t) noexcept;

  // Conversions

  /// Explicit conversion operator for downcasting.
  ///
  /// If an arbitrary type `A` derives from another type `B`, a shared pointer
  /// to `A` can be automatically upcast to one of type `B`. This operator
  /// performs the reverse operation with an explicit cast.
  ///
  /// @code{.cpp}
  /// pw::SharedPtr<A> a1 = allocator.MakeShared<A>();
  /// pw::SharedPtr<B> b = a1;
  /// pw::SharedPtr<A> a2 = static_cast<pw::SharedPtr<A>>(b);
  /// @endcode
  template <typename U>
  constexpr explicit operator SharedPtr<U>() const {
    return static_pointer_cast<U>(*this);
  }

  /// Creates a new SharedPtr by static casting the given shared pointer.
  ///
  /// If an arbitrary type `A` derives from another type `B`, a shared pointer
  /// to `A` can be automatically upcast to one of type `B`. This function can
  /// perform the reverse operation and allows specifying only the element
  /// type.
  ///
  /// @code{.cpp}
  /// pw::SharedPtr<A> a1 = allocator.MakeShared<A>();
  /// pw::SharedPtr<B> b = a1;
  /// pw::SharedPtr<A> a2 = static_pointer_cast<A>(b);
  /// @endcode
  template <typename To, typename From>
  friend constexpr SharedPtr<To> static_pointer_cast(
      const SharedPtr<From>& p) noexcept;

  /// Creates a new SharedPtr by const casting the given shared pointer.
  ///
  /// For an arbitrary type `A`, a shared pointer to `A` can be automatically
  /// converted to one of type `const A`. This function can perform the reverse
  /// operation.
  ///
  /// @code{.cpp}
  /// pw::SharedPtr<const A> a1 = allocator.MakeShared<const A>();
  /// pw::SharedPtr<A> a2 = const_pointer_cast<A>(a1);
  /// @endcode
  template <typename To, typename From>
  friend constexpr SharedPtr<To> const_pointer_cast(
      const SharedPtr<From>& p) noexcept;

  // Comparisons

  [[nodiscard]] friend constexpr bool operator==(const SharedPtr& lhs,
                                                 std::nullptr_t) {
    return lhs.Equals(nullptr);
  }
  [[nodiscard]] friend constexpr bool operator==(std::nullptr_t,
                                                 const SharedPtr& rhs) {
    return rhs.Equals(nullptr);
  }
  [[nodiscard]] friend constexpr bool operator==(const SharedPtr& lhs,
                                                 const SharedPtr& rhs) {
    return lhs.Equals(rhs) && lhs.control_block_ == rhs.control_block_;
  }

  [[nodiscard]] friend constexpr bool operator!=(const SharedPtr& lhs,
                                                 std::nullptr_t) {
    return !lhs.Equals(nullptr);
  }
  [[nodiscard]] friend constexpr bool operator!=(std::nullptr_t,
                                                 const SharedPtr& rhs) {
    return !rhs.Equals(nullptr);
  }
  [[nodiscard]] friend constexpr bool operator!=(const SharedPtr& lhs,
                                                 const SharedPtr& rhs) {
    return !(lhs == rhs);
  }

  // Accessors

  /// Returns the number of elements allocated.
  ///
  /// This will fail to compile if it is called on a non-array type SharedPtr.
  constexpr size_t size() const {
    static_assert(std::is_array_v<T>,
                  "size() cannot be called on non-array types");
    return control_block_ == nullptr
               ? 0
               : (control_block_->size() / sizeof(element_type));
  }

  /// Returns the number of shared pointers to the associated object, or 0 if
  /// this object is empty.
  constexpr int32_t use_count() const {
    return control_block_ == nullptr ? 0 : control_block_->num_shared();
  }

  /// Checks whether `this` precedes `other` based on an ordering of their
  /// control blocks.
  template <typename PtrType>
  bool owner_before(const PtrType& other) const noexcept {
    return control_block_ < other.control_block();
  }

  // Mutators

  /// Resets this object to an empty state.
  ///
  /// If this was the last shared pointer to the associated object, it is
  /// destroyed. If this is the last shared or weak pointer associated with the
  /// control block, it is deallocated.
  void reset() noexcept;

  /// Swaps the managed pointer and deallocator of this and another object.
  void swap(SharedPtr& other) noexcept;

  /// Returns the allocator that owns this `SharedPtr` or `nullptr` if the
  /// `SharedPtr` is empty.
  Allocator* allocator() const {
    return control_block_ == nullptr ? nullptr : control_block_->allocator();
  }

 private:
  using Layout = allocator::Layout;

  // Allow SharedPtrs, WeakPtrs, and MultiBufs to access control blocks.
  template <typename>
  friend class SharedPtr;

  template <typename>
  friend class WeakPtr;

  template <multibuf::v2::Property...>
  friend class multibuf::v2::BasicMultiBuf;

  ControlBlock* control_block() const { return control_block_; }

  /// Copies details from another object without releasing it.
  template <typename U,
            typename = std::enable_if_t<std::is_assignable_v<T*&, U*>>>
  void CopyFrom(const SharedPtr<U>& other);

  /// Disassociates this object from its associated object and control block,
  /// returning it to an empty state.
  void Release();

  /// Statically checks whether `T` and `U` are both or neither array types.
  template <typename U>
  constexpr void CheckArrayTypes();

  ControlBlock* control_block_ = nullptr;
};

/// @}

// Template method implementations.

template <typename T>
SharedPtr<T>::SharedPtr(UniquePtr<T>& owned) noexcept {
  size_t size = sizeof(element_type);
  if constexpr (std::is_array_v<T>) {
    size *= owned.size();
  }
  control_block_ = ControlBlock::Create(owned.deallocator(), owned.get(), size);
  if (control_block_ != nullptr) {
    Base::CopyFrom(owned);
    owned.Release();
  }
}

template <typename T>
template <typename... Args>
SharedPtr<T> SharedPtr<T>::Create(Allocator* allocator, Args&&... args) {
  static_assert(!std::is_array_v<T>);
  auto* control_block = ControlBlock::Create(allocator, Layout::Of<T>());
  if (control_block == nullptr) {
    return nullptr;
  }
  auto* t = new (control_block->data()) T(std::forward<Args>(args)...);
  return SharedPtr<T>(t, control_block);
}

template <typename T>
SharedPtr<T> SharedPtr<T>::Create(Allocator* allocator,
                                  size_t count,
                                  size_t alignment) {
  static_assert(allocator::internal::is_unbounded_array_v<T>);
  Layout layout = Layout::Of<T>(count).Align(alignment);
  auto* control_block = ControlBlock::Create(allocator, layout);
  if (control_block == nullptr) {
    return nullptr;
  }
  auto* t = new (control_block->data()) element_type[count];
  return SharedPtr<T>(t, control_block);
}

template <typename T>
template <typename U, typename>
constexpr SharedPtr<T>& SharedPtr<T>::operator=(
    const SharedPtr<U>& other) noexcept {
  CheckArrayTypes<U>();
  reset();
  CopyFrom(other);
  if (control_block_ != nullptr) {
    control_block_->IncrementShared();
  }
  return *this;
}

template <typename T>
template <typename U, typename>
SharedPtr<T>& SharedPtr<T>::operator=(SharedPtr<U>&& other) noexcept {
  CheckArrayTypes<U>();
  reset();
  CopyFrom(other);
  other.Release();
  return *this;
}

template <typename T>
SharedPtr<T>& SharedPtr<T>::operator=(std::nullptr_t) noexcept {
  reset();
  return *this;
}

template <typename To, typename From>
constexpr SharedPtr<To> static_pointer_cast(const SharedPtr<From>& p) noexcept {
  using PtrType = typename SharedPtr<To>::element_type*;
  if (p.control_block_ != nullptr) {
    p.control_block_->IncrementShared();
  }
  return SharedPtr<To>{static_cast<PtrType>(p.get()), p.control_block_};
}

template <typename To, typename From>
constexpr SharedPtr<To> const_pointer_cast(const SharedPtr<From>& p) noexcept {
  using PtrType = typename SharedPtr<To>::element_type*;
  if (p.control_block_ != nullptr) {
    p.control_block_->IncrementShared();
  }
  return SharedPtr<To>{const_cast<PtrType>(p.get()), p.control_block_};
}

template <typename T>
void SharedPtr<T>::reset() noexcept {
  if (*this == nullptr) {
    return;
  }
  auto action = control_block_->DecrementShared();
  if (action == ControlBlock::Action::kNone) {
    // Other `SharedPtr`s associated with this control block remain.
    Release();
    return;
  }

  // This was the last `SharedPtr` associated with this control block.
  Allocator* allocator = control_block_->allocator();
  if (!Base::HasCapability(allocator, allocator::kSkipsDestroy)) {
    if constexpr (std::is_array_v<T>) {
      Base::Destroy(size());
    } else {
      Base::Destroy();
    }
  }

  if (action == ControlBlock::Action::kExpire) {
    // Keep the control block. Trying to promote any of the remaining
    // `WeakPtr`s will fail.
    Base::Resize(allocator, control_block_, sizeof(ControlBlock));
  } else {
    // No `WeakPtr`s remain, and all of the memory can be freed.
    std::destroy_at(control_block_);
    Base::Deallocate(allocator, control_block_);
  }

  Release();
}

template <typename T>
void SharedPtr<T>::swap(SharedPtr<T>& other) noexcept {
  Base::Swap(other);
  std::swap(control_block_, other.control_block_);
}

template <typename T>
template <typename U, typename>
void SharedPtr<T>::CopyFrom(const SharedPtr<U>& other) {
  CheckArrayTypes<U>();
  Base::CopyFrom(other);
  control_block_ = other.control_block_;
}

template <typename T>
void SharedPtr<T>::Release() {
  Base::Release();
  control_block_ = nullptr;
}

template <typename T>
template <typename U>
constexpr void SharedPtr<T>::CheckArrayTypes() {
  if constexpr (std::is_array_v<T>) {
    static_assert(std::is_array_v<U>,
                  "non-array type used with SharedPtr<T[]>");
  } else {
    static_assert(!std::is_array_v<U>, "array type used with SharedPtr<T>");
  }
}

}  // namespace pw

// TODO(b/402489948): Remove when portable atomics are provided by `pw_atomic`.
#endif  // PW_ALLOCATOR_HAS_ATOMICS

#endif  // PW_ALLOCATOR_PUBLIC_PW_ALLOCATOR_SHARED_PTR_H_
