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

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "pw_allocator/layout.h"

namespace pw {

// Forward declarations to prevent including "allocator.h", et al. which include
// this file.
class Allocator;
class Deallocator;

// Forward declarations for friending.
template <typename T>
class SharedPtr;

namespace multibuf {

namespace v1_adapter::internal {
class ChunkAllocator;
}  // namespace v1_adapter::internal

namespace v2::internal {
class GenericMultiBuf;
}  // namespace v2::internal

}  // namespace multibuf

namespace allocator::internal {

/// Object metadata used to track ref counts of shared and weak pointers.
///
/// This control block is associated with an object, and will destroy it when
/// no shared pointers remain. It will deallocate any associated memory when
/// no weak or shared pointers remain.
class ControlBlock final {
 public:
  enum class Action {
    /// On return from a `Decrement` method, no action is needed.
    kNone,

    /// On return from a `Decrement` method, no shared pointers remain and the
    /// associated object can be destroyed. Weak pointers remain, so the control
    /// block memory should not be deallocated.
    kExpire,

    /// On return from a `Decrement` method, no shared or weak pointers remain
    /// and the associated memory can be deallocated.
    kFree,
  };

  /// Destructor.
  ///
  /// If this control block was `Create`d for previously allocated memory, that
  /// memory is freed.
  ~ControlBlock();

  /// Returns the allocator that allocated the control block.
  constexpr Allocator* allocator() const { return allocator_; }

  /// Returns a pointer to the memory location of the associated object.
  constexpr void* data() const { return data_; }

  /// Returns the number of bytes pointed at by the shared pointer.
  constexpr size_t size() const { return GetSize(); }

  /// Returns the number of active shared pointers to the associated object.
  ///
  /// Although the count cannot be negative, this is returned as a signed
  /// integer in order to more closely align with `std::shared_ptr::use_count()`
  /// and `std::weak_ptr::use_count`.
  ///
  /// This method is thread-safe.
  int32_t num_shared() const noexcept;

  /// If the count of shared pointers is nonzero, increments the count of
  /// shared pointers atomically and returns true; otherwise returns false.
  ///
  /// This method is thread-safe.
  bool IncrementShared();

  /// If the count of either weak or shared pointers is nonzero, increments the
  /// count of weak pointers atomically and returns true; otherwise returns
  /// false.
  ///
  /// This method is thread-safe.
  bool IncrementWeak();

  /// Atomically decrements the count of shared pointers associated with this
  /// control block.
  ///
  /// This method is thread-safe.
  Action DecrementShared();

  /// Atomically decrements the count of weak pointers associated with this
  /// control block.
  ///
  /// This method is thread-safe.
  Action DecrementWeak();

 private:
  // Allow SharedPtrs and MultiBufs to create control blocks.
  template <typename T>
  friend class ::pw::SharedPtr;
  friend class ::pw::multibuf::v1_adapter::internal::ChunkAllocator;
  friend class ::pw::multibuf::v2::internal::GenericMultiBuf;

  /// Factory method for allocating memory with an attached control block.
  ///
  /// If allocation fails, this call will return null.
  ///
  /// @param  allocator   Allocator to use to provide memory.
  /// @param  layout      Layout of an object to allocate and associate with a
  ///                     control block.
  static ControlBlock* Create(Allocator* allocator, Layout layout);

  /// Factory method for allocating a control block for previously allocated
  /// memory.
  ///
  /// The parameters are expected to come from a `UniquePtr`.
  ///
  /// If allocation fails, this call will return null.
  ///
  /// @param  deallocator Deallocator to use to provide memory. Must have the
  ///                     `kCanAllocateArbitraryLayout` capability, i.e. be an
  ///                     upcast allocator.
  /// @param  data        Previously allocated memory.
  /// @param  size        Size of the previously allocated memory, in bytes.
  static ControlBlock* Create(Deallocator* deallocator,
                              void* data,
                              size_t size);

  /// Creates a new control block with an initial shared pointer count of 1.
  ControlBlock(Allocator* allocator, void* data, size_t size, bool coallocated);

  // Accessors and mutators that handling unpacking values
  constexpr bool IsCoallocated() const { return coallocated_and_size_ & 1u; }
  constexpr size_t GetSize() const { return coallocated_and_size_ >> 1u; }

  /// Returns a count of weak pointers from a packed value.
  static constexpr uint16_t UnpackWeak(uint32_t num_weak_and_shared) {
    return num_weak_and_shared >> 16;
  }

  /// Returns a count of shared pointers from a packed value.
  static constexpr uint16_t UnpackShared(uint32_t num_weak_and_shared) {
    return num_weak_and_shared & 0xFFFF;
  }

  /// Returns a packed value that combines a count of weak pointers and a count
  /// of shared pointers.
  static constexpr uint32_t Pack(uint16_t num_weak, uint16_t num_shared) {
    return (static_cast<uint32_t>(num_weak) << 16) | (num_shared & 0xFFFF);
  }

  Allocator* allocator_;
  void* data_;
  const size_t coallocated_and_size_ = 0;

  // Combines 2 16-bit counts for weak and shared pointers, respectively.
  mutable std::atomic_uint32_t num_weak_and_shared_;
};

}  // namespace allocator::internal
}  // namespace pw
