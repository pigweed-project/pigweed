// Copyright 2024 The Pigweed Authors
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
#include <mutex>

#include "pw_allocator/forwarding_allocator.h"
#include "pw_sync/borrow.h"
#include "pw_sync/lock_annotations.h"
#include "pw_sync/no_lock.h"

namespace pw::allocator {

/// @submodule{pw_allocator,forwarding}

/// Wraps an `Allocator` with a lock to synchronize access.
///
/// Depending on the `LockType`, this object may be thread- and/or interrupt-
/// safe. For example, `SynchronizedAllocator<pw::sync::Mutex>` is thread-safe,
/// while `SynchronizedAllocator<pw::sync::InterruptSpinLock>` is thread- and
/// interrupt-safe.
///
/// @tparam LockType  The type of the lock used to synchronize allocator access.
///                   Must be default-constructible.
template <typename LockType>
class SynchronizedAllocator final : public ForwardingAllocator {
 private:
  using Base = ForwardingAllocator;
  using Borrowable = sync::Borrowable<pw::Allocator, LockType>;
  using BorrowedPointer = sync::BorrowedPointer<pw::Allocator, LockType>;

 public:
  constexpr SynchronizedAllocator(const Capabilities& capabilities) noexcept
      : Base(capabilities) {}

  constexpr explicit SynchronizedAllocator(pw::Allocator& allocator) noexcept
      : Base(allocator), borrowable_(Borrowable(allocator, lock_)) {}

  /// @copydoc ForwardingAllocator::Init
  void Init(pw::Allocator& allocator) {
    std::lock_guard lock(lock_);
    Base::Init(allocator);
    borrowable_.emplace(allocator, lock_);
  }

  /// Returns a borrowed pointer to the allocator.
  ///
  /// When an allocator being wrapped implements an interface that extends
  /// `pw::Allocator`, this method can be used to safely access a downcastable
  /// pointer. The usual warnings apply to the returned value; namely the caller
  /// MUST NOT leak the raw pointer.
  ///
  /// Example:
  /// @code{.cpp}
  ///   pw::allocator::BestFitAllocator<> best_fit(heap);
  ///   pw::allocator::SynchronizedAllocator<pw::sync::Mutex> synced(best_fit);
  ///   // ...
  ///   auto borrowed = synced.Borrow();
  ///   auto allocator =
  ///     static_cast<pw::allocator::BestFitAllocator<>&>(*borrowed);
  /// @endcode
  BorrowedPointer Borrow() const {
    PW_ASSERT(borrowable_.has_value());
    return borrowable_->acquire();
  }

 protected:
  /// @copydoc Allocator::Allocate
  void* DoAllocate(Layout layout) override {
    std::lock_guard lock(lock_);
    return Base::DoAllocate(layout);
  }

  /// @copydoc Allocator::Deallocate
  void DoDeallocate(void* ptr) override {
    std::lock_guard lock(lock_);
    return Base::DoDeallocate(ptr);
  }

  /// @copydoc Allocator::Resize
  bool DoResize(void* ptr, size_t new_size) override {
    std::lock_guard lock(lock_);
    return Base::DoResize(ptr, new_size);
  }

  /// @copydoc Allocator::Reallocate
  void* DoReallocate(void* ptr, Layout new_layout) override {
    auto borrowed = Borrow();
    return borrowed->Reallocate(ptr, new_layout);
  }

  /// @copydoc Allocator::GetAllocated
  size_t DoGetAllocated() const override {
    std::lock_guard lock(lock_);
    return Base::DoGetAllocated();
  }

  /// @copydoc Allocator::DoMeasureFragmentation
  std::optional<Fragmentation> DoMeasureFragmentation() const override {
    std::lock_guard lock(lock_);
    return Base::DoMeasureFragmentation();
  }

  /// @copydoc Deallocator::GetInfo
  Result<Layout> DoGetInfo(InfoType info_type, const void* ptr) const override {
    std::lock_guard lock(lock_);
    return Base::DoGetInfo(info_type, ptr);
  }

 private:
  mutable LockType lock_;
  std::optional<Borrowable> borrowable_;
};

/// Tag type used to indicate synchronization is NOT desired.
///
/// This can be useful with allocator parameters for module configuration, e.g.
/// PW_MALLOC_LOCK_TYPE.
using NoSync = pw::sync::NoLock;

/// @}

}  // namespace pw::allocator
