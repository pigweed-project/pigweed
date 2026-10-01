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
#include <cstdint>
#include <type_traits>

#include "pw_allocator/config.h"
#include "pw_allocator/framing_allocator.h"
#include "pw_allocator/unique_ptr.h"
#include "pw_tokenizer/tokenize.h"

#if PW_ALLOCATOR_HAS_ATOMICS
#include "pw_allocator/internal/control_block.h"
#include "pw_allocator/shared_ptr.h"
#endif  // PW_ALLOCATOR_HAS_ATOMICS

/// Creates a ``SourceLocation`` representing the location it was invoked at.
#define PW_ALLOCATOR_ORIGIN()                                    \
  ::pw::allocator::SourceLocation {                              \
    .file = PW_TOKENIZE_STRING_EXPR(__FILE__), .line = __LINE__, \
  }

/// Calls ``expr`` on ``alloc`` and then sets the origin on the value it
/// returns.
#define PW_ALLOCATOR_SET_ORIGIN(alloc, ...) \
  (alloc).SetOrigin(PW_ALLOCATOR_ORIGIN(), (alloc).__VA_ARGS__)

namespace pw::allocator {

/// Represents a location in a source file.
struct SourceLocation {
  tokenizer::Token file = 0;
  uint32_t line = 0;
};

/// An Allocator that allows storing source location info with each allocation.
///
/// Source location is not stored automatically in order to preserve the
/// Allocator API. Instead callers can utilize the fact that ``SetOrigin``
/// accepts ``nullptr`` and returns the given pointer to easily add origination
/// information to allocations:
///
/// @code{.cpp}
///   T* t = alloc.SetOrigin(PW_ALLOCATOR_ORIGIN(), alloc.New<T>(my_args));
/// @endcode
///
/// Alternatively, the ``PW_ALLOCATOR_SET_ORIGIN`` macro can do the same a bit
/// more succinctly for methods that return a pointer:
///
/// @code{.cpp}
///   T* t = PW_ALLOCATOR_SET_ORIGIN(alloc, New<T>(my_args));
/// @endcode
///
/// @note It is up to the caller to decide whether to update the origin when
/// resizing or reallocating, i.e. whether to track where the original
/// allocation was made or where the allocation was most recently modified.
class OriginAllocator : public FramingAllocator<SourceLocation> {
 private:
  using Base = FramingAllocator<SourceLocation>;

 public:
  constexpr explicit OriginAllocator(Allocator& allocator) : Base(allocator) {}

  /// Returns the source location that made the given allocation.
  ///
  /// Pointer must be to an allocation from this allocator.
  SourceLocation GetOrigin(const void* ptr) const;

  /// Returns the source location that made the given `UniquePtr` allocation.
  template <typename T>
  SourceLocation GetOrigin(const UniquePtr<T>& ptr) const {
    return GetOrigin(ptr.get());
  }

#if PW_ALLOCATOR_HAS_ATOMICS
  /// Returns the source location that made the given `SharedPtr` allocation.
  ///
  /// For shared pointers created via ``MakeShared``, this returns the location
  /// where the shared object was created. For shared pointers converted from an
  /// existing allocation (e.g. from a ``UniquePtr``), this returns the location
  /// associated with the shared pointer conversion rather than the underlying
  /// data object.
  template <typename T>
  SourceLocation GetOrigin(const SharedPtr<T>& ptr) const {
    const auto& handle =
        allocator::internal::ControlBlockHandle::GetInstance_DO_NOT_USE();
    return GetOrigin(ptr.GetControlBlock(handle));
  }
#endif  // PW_ALLOCATOR_HAS_ATOMICS

  /// Sets the originating source location and returns the given pointer.
  ///
  /// Pointer must be null or to an allocation from this allocator.
  ///
  /// Use this method like this:
  /// @code{.cpp}
  ///   T* ptr =
  ///     alloc.SetOrigin(PW_ALLOCATOR_ORIGIN(), alloc.New<T>(args...));
  /// @endcode
  template <typename T>
  [[nodiscard]] T* SetOrigin(SourceLocation origin, T* ptr) {
    DoSetOrigin(origin, const_cast<std::remove_cv_t<T>*>(ptr));
    return ptr;
  }

  /// Sets the originating source location and returns the given unique pointer.
  ///
  /// Use this method like this:
  /// @code{.cpp}
  ///   pw::UniquePtr<T> ptr =
  ///     alloc.SetOrigin(PW_ALLOCATOR_ORIGIN(), alloc.MakeUnique<T>(args...));
  /// @endcode
  template <typename T>
  [[nodiscard]] UniquePtr<T> SetOrigin(SourceLocation origin,
                                       UniquePtr<T> ptr) {
    DoSetOrigin(origin, const_cast<std::remove_cv_t<T>*>(ptr.get()));
    return ptr;
  }

#if PW_ALLOCATOR_HAS_ATOMICS
  /// Sets the originating source location and returns the given shared pointer.
  ///
  /// Use this method like this:
  /// @code{.cpp}
  ///   pw::SharedPtr<T> ptr =
  ///     alloc.SetOrigin(PW_ALLOCATOR_ORIGIN(), alloc.MakeShared<T>(args...));
  /// @endcode
  ///
  /// @note This method is primarily intended for use with ``MakeShared``. If
  /// used with a shared pointer converted from an existing allocation (such as
  /// one created from a ``UniquePtr``), the origin information will be attached
  /// to the shared pointer's reference-tracking metadata rather than the
  /// underlying data object.
  template <typename T>
  [[nodiscard]] SharedPtr<T> SetOrigin(SourceLocation origin,
                                       SharedPtr<T> ptr) {
    const auto& handle =
        allocator::internal::ControlBlockHandle::GetInstance_DO_NOT_USE();
    DoSetOrigin(origin, ptr.GetControlBlock(handle));
    return ptr;
  }
#endif  // PW_ALLOCATOR_HAS_ATOMICS

 private:
  /// @copydoc OriginAllocator::SetOrigin
  void DoSetOrigin(SourceLocation origin, void* ptr);
};

}  // namespace pw::allocator
