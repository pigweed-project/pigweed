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
#include <optional>

#include "pw_allocator/capability.h"
#include "pw_allocator/config.h"
#include "pw_allocator/deallocator.h"
#include "pw_allocator/fragmentation.h"
#include "pw_allocator/layout.h"
#include "pw_allocator/shared_ptr.h"
#include "pw_allocator/unique_ptr.h"
#include "pw_numeric/checked_arithmetic.h"
#include "pw_result/result.h"

namespace pw {

/// @submodule{pw_allocator,core}

// Forward declare to break a circular dependency with shared_ptr.h.
template <typename T>
class SharedPtr;

/// Abstract interface for variable-layout memory allocation.
///
/// The interface makes no guarantees about its implementation. Consumers of the
/// generic interface must not make any assumptions around allocator behavior,
/// thread safety, or performance.
class Allocator : public Deallocator {
 public:
  using Fragmentation = allocator::Fragmentation;

  /// Allocates a block of memory with the specified size and alignment.
  ///
  /// Returns `nullptr` if the allocation cannot be made, or the `layout` has a
  /// size of 0.
  ///
  /// @param[in]  layout      Describes the memory to be allocated.
  [[nodiscard]] void* Allocate(Layout layout);

  /// Constructs an object of type `T` from the given `args`.
  ///
  /// The return value is nullable, as allocating memory for the object may
  /// fail. Callers must check for this error before using the resulting
  /// pointer.
  ///
  /// @tparam     T           A non-array object type, like `int`.
  /// @param[in]  args        Arguments passed to the object constructor.
  template <typename T,
            int&... kExplicitGuard,
            std::enable_if_t<!std::is_array_v<T>, int> = 0,
            typename... Args>
  [[nodiscard]] T* New(Args&&... args) {
    void* ptr = Allocate(Layout::Of<T>());
    return ptr != nullptr ? new (ptr) T(std::forward<Args>(args)...) : nullptr;
  }

  /// Constructs an array of objects.
  ///
  /// The return value is nullable, as allocating memory for the object may
  /// fail. Callers must check for this error before using the resulting
  /// pointer.
  ///
  /// @tparam     T            A bounded array type, like `int[3]`.
  template <typename T,
            int&... kExplicitGuard,
            typename ElementType = std::remove_extent_t<T>,
            std::enable_if_t<is_bounded_array_v<T>, int> = 0>
  [[nodiscard]] ElementType* New() {
    return NewArrayImpl<ElementType>(std::extent_v<T>, alignof(ElementType));
  }

  /// Constructs an array of `count` objects.
  ///
  /// The return value is nullable, as allocating memory for the object may
  /// fail. Callers must check for this error before using the resulting
  /// pointer.
  ///
  /// @tparam     T            An unbounded array type, like `int[]`.
  /// @param[in]  count        Number of objects to allocate.
  template <typename T,
            int&... kExplicitGuard,
            typename ElementType = std::remove_extent_t<T>,
            std::enable_if_t<is_unbounded_array_v<T>, int> = 0>
  [[nodiscard]] ElementType* New(size_t count) {
    return NewArrayImpl<ElementType>(count, alignof(ElementType));
  }

  /// Constructs an `alignment`-byte aligned array of `count` objects.
  //
  /// The return value is nullable, as allocating memory for the object may
  /// fail. Callers must check for this error before using the resulting
  /// pointer.
  ///
  /// @tparam     T            An unbounded array type, like `int[]`.
  /// @param[in]  count        Number of objects to allocate.
  /// @param[in]  alignment    Alignment to use for the start of the array.
  template <typename T,
            int&... kExplicitGuard,
            typename ElementType = std::remove_extent_t<T>,
            std::enable_if_t<is_unbounded_array_v<T>, int> = 0>
  [[nodiscard]] ElementType* New(size_t count, size_t alignment) {
    return NewArrayImpl<ElementType>(count, alignment);
  }

  /// Constructs and object of type `T` from the given `args`, and wraps it in a
  /// `UniquePtr`
  ///
  /// The returned value may contain null if allocating memory for the object
  /// fails. Callers must check for null before using the `UniquePtr`.
  ///
  /// @tparam     T           A non-array object type, like `int`.
  /// @param[in]  args        Arguments passed to the object constructor.
  template <typename T,
            int&... kExplicitGuard,
            std::enable_if_t<!std::is_array_v<T>, int> = 0,
            typename... Args>
  [[nodiscard]] UniquePtr<T> MakeUnique(Args&&... args) {
    return UniquePtr<T>(New<T>(std::forward<Args>(args)...), *this);
  }

  /// Constructs an array of `size` objects, and wraps it in a `UniquePtr`
  ///
  /// The returned value may contain null if allocating memory for the object
  /// fails. Callers must check for null before using the `UniquePtr`.
  ///
  /// @tparam     T            An unbounded array type, like `int[]`.
  /// @param[in]  size         Number of objects to allocate.
  template <typename T, std::enable_if_t<is_unbounded_array_v<T>, int> = 0>
  [[nodiscard]] UniquePtr<T> MakeUnique(size_t size) {
    return MakeUnique<T>(size, alignof(std::remove_extent_t<T>));
  }

  /// Constructs an `alignment`-byte aligned array of `size` objects and wraps
  /// it in a `UniquePtr`.
  ///
  /// The returned value may contain null if allocating memory for the object
  /// fails. Callers must check for null before using the `UniquePtr`.
  ///
  /// @tparam     T            An unbounded array type, like `int[]`.
  /// @param[in]  size         Number of objects to allocate.
  /// @param[in]  alignment    Object alignment.
  template <typename T, std::enable_if_t<is_unbounded_array_v<T>, int> = 0>
  [[nodiscard]] UniquePtr<T> MakeUnique(size_t size, size_t alignment) {
    return UniquePtr<T>(New<T>(size, alignment), size, *this);
  }

  /// Constructs an array of objects and wraps it in a `UniquePtr`.
  ///
  /// The returned value may contain null if allocating memory for the object
  /// fails. Callers must check for null before using the `UniquePtr`.
  ///
  /// @tparam     T            A bounded array type, like `int[3]`.
  template <typename T, std::enable_if_t<is_bounded_array_v<T>, int> = 0>
  [[nodiscard]] UniquePtr<T> MakeUnique() {
    return UniquePtr<T>(New<T>(), std::extent_v<T>, this);
  }

// TODO(b/402489948): Remove when portable atomics are provided by `pw_atomic`.
#if PW_ALLOCATOR_HAS_ATOMICS

  /// Constructs and object of type `T` from the given `args`, and wraps it in a
  /// `SharedPtr`
  ///
  /// The returned value may contain null if allocating memory for the object
  /// fails. Callers must check for null before using the `SharedPtr`.
  ///
  /// @param[in]  args        Arguments passed to the object constructor.
  template <typename T,
            int&... kExplicitGuard,
            std::enable_if_t<!std::is_array_v<T>, int> = 0,
            typename... Args>
  [[nodiscard]] SharedPtr<T> MakeShared(Args&&... args) {
    return SharedPtr<T>::template Create<Args...>(this,
                                                  std::forward<Args>(args)...);
  }

  /// Constructs an array of `size` objects, and wraps it in a `SharedPtr`.
  ///
  /// The returned value may contain null if allocating memory for the object
  /// fails. Callers must check for null before using the `SharedPtr`.
  ///
  /// @tparam     T            An array type.
  /// @param[in]  size         Number of objects to allocate.
  template <typename T, std::enable_if_t<is_unbounded_array_v<T>, int> = 0>
  [[nodiscard]] SharedPtr<T> MakeShared(size_t size) {
    return MakeShared<T>(size, alignof(std::remove_extent_t<T>));
  }

  /// Constructs an `alignment`-byte aligned array of `size` objects, and wraps
  /// it in a `SharedPtr`
  ///
  /// The returned value may contain null if allocating memory for the object
  /// fails. Callers must check for null before using the `SharedPtr`.
  ///
  /// @tparam     T            An array type.
  /// @param[in]  size         Number of objects to allocate.
  /// @param[in]  alignment    Object alignment.
  template <typename T, std::enable_if_t<is_unbounded_array_v<T>, int> = 0>
  [[nodiscard]] SharedPtr<T> MakeShared(size_t size, size_t alignment) {
    return SharedPtr<T>::Create(this, size, alignment);
  }

  /// Constructs an array of objects and wraps it in a `SharedPtr`.
  ///
  /// The returned value may contain null if allocating memory for the object
  /// fails. Callers must check for null before using the `SharedPtr`.
  ///
  /// @tparam     T            A bounded array type, like `int[3]`.
  template <typename T, std::enable_if_t<is_bounded_array_v<T>, int> = 0>
  [[nodiscard]] SharedPtr<T> MakeShared() {
    return SharedPtr<T>::Create(
        this, std::extent_v<T>, alignof(std::remove_extent_t<T>));
  }

// TODO(b/402489948): Remove when portable atomics are provided by `pw_atomic`.
#endif  // PW_ALLOCATOR_HAS_ATOMICS

  /// Modifies the size of an previously-allocated block of memory without
  /// copying any data.
  ///
  /// Returns true if its size was changed without copying data to a new
  /// allocation; otherwise returns false.
  ///
  /// In particular, it always returns true if the `old_layout.size()` equals
  /// `new_size`, and always returns false if the given pointer is null, the
  /// `old_layout.size()` is 0, or the `new_size` is 0.
  ///
  /// @param[in]  ptr           Pointer to previously-allocated memory.
  /// @param[in]  new_size      Requested new size for the memory allocation.
  [[nodiscard]] bool Resize(void* ptr, size_t new_size);

  /// Modifies the size of a previously-allocated block of memory.
  ///
  /// Returns pointer to the modified block of memory, or `nullptr` if the
  /// memory could not be modified.
  ///
  /// The data stored by the memory being modified must be trivially
  /// copyable. If it is not, callers should themselves attempt to `Resize`,
  /// then `Allocate`, move the data, and `Deallocate` as needed.
  ///
  /// If `nullptr` is returned, the block of memory is unchanged. In particular,
  /// if the `new_layout` has a size of 0, the given pointer will NOT be
  /// deallocated.
  ///
  /// TODO(b/331290408): This error condition needs to be better communicated to
  /// module users, who may assume the pointer is freed.
  ///
  /// Unlike `Resize`, providing a null pointer will return a new allocation.
  ///
  /// If the request can be satisfied using `Resize`, the `alignment` parameter
  /// may be ignored.
  ///
  /// @param[in]  ptr         Pointer to previously-allocated memory.
  /// @param[in]  new_layout  Describes the memory to be allocated.
  [[nodiscard]] void* Reallocate(void* ptr, Layout new_layout);

  /// Returns the total bytes that have been allocated by this allocator, or
  /// `size_t(-1)` if this allocator does not track its total allocated bytes.
  size_t GetAllocated() const { return DoGetAllocated(); }

  /// Returns fragmentation information for the allocator's memory region.
  std::optional<Fragmentation> MeasureFragmentation() const {
    return DoMeasureFragmentation();
  }

 protected:
  /// TODO(b/326509341): Remove when downstream consumers migrate.
  constexpr Allocator() = default;

  explicit constexpr Allocator(const Capabilities& capabilities)
      : Deallocator(Capability::kCanAllocateArbitraryLayout | capabilities) {}

  /// @copydoc Allocator::Allocate
  virtual void* DoAllocate(Layout layout) = 0;

  /// @copydoc Allocator::Resize
  virtual bool DoResize([[maybe_unused]] void* ptr,
                        [[maybe_unused]] size_t new_size) {
    return false;
  }

  /// @copydoc Allocator::Reallocate
  ///
  /// The default implementation will first try to `Resize` the data. If that is
  /// unsuccessful, it will allocate an entirely new block, copy existing data,
  /// and deallocate the given block.
  virtual void* DoReallocate(void* ptr, Layout new_layout);

  /// @copydoc Allocator::GetAllocated
  ///
  /// The default implementation simply returns `size_t(-1)`, indicating that
  /// tracking total allocated bytes is not supported.
  virtual size_t DoGetAllocated() const { return size_t(-1); }

  /// @copydoc Allocator::MeasureFragmentation
  ///
  /// The default implementation simply returns `std::nullopt`, indicating that
  /// tracking memory fragmentation is not supported.
  virtual std::optional<Fragmentation> DoMeasureFragmentation() const {
    return std::nullopt;
  }

 private:
  // Helper method for allocating arrays of objects.
  template <typename ElementType>
  ElementType* NewArrayImpl(size_t count, size_t alignment) {
    void* ptr = Allocate(Layout::Of<ElementType[]>(count).Align(alignment));
    return ptr != nullptr ? new (ptr) ElementType[count] : nullptr;
  }
};

namespace allocator {

// TODO(b/376730645): This deprecated alias cannot be removed yet due to a
// downstream dependency.
using Allocator [[deprecated("Use pw::Allocator instead")]] = ::pw::Allocator;

}  // namespace allocator

/// @}

}  // namespace pw
