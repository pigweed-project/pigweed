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
#include <optional>

#include "pw_allocator/layout.h"
#include "pw_allocator/pool.h"
#include "pw_result/result.h"

namespace pw::allocator {

/// A pool that is implemented by forwarding calls to another pool.
///
/// This type can be used as a generic base type for forwarding pools that
/// provide additional behavior to another pool. It provides default
/// implementations of the virtual methods from `Deallocator` and `Pool`,
/// allowing authors of derived types to avoid boilerplate.
///
/// @note It is an error for a derived type to override any of the methods below
/// with an implementation that calls any of the corresponding public methods.
/// If it does, the public, NVI-style method will dispatch to the derived type's
/// override, leading to an infinite recursion. Instead, use the `pool`
/// accessor or call this type's implementation methods directly:
///
/// @code{.cpp}
/// class MyPool : public pw::allocator::ForwardingPool {
///  private:
///   void* Allocate() override {
///     // INCORRECT! This will infinitely recurse.
///     // return pw::allocator::ForwardingPool::Allocate();
///
///     // Correct: This dispatches to the wrapped pool.
///     return pool().Allocate();
///   }
///
///   void Deallocate(void* ptr) override {
///     // Also correct: This dispatches to the wrapped pool.
///     pw::allocator::ForwardingPool::DoDeallocate(ptr);
///   }
/// }
/// @endcode
class ForwardingPool : public Pool {
 protected:
  /// Creates a forwarding pool without setting its wrapped pool.
  ///
  /// This constructor should be used when the forwarding pool needs to be
  /// created before the pool being wrapped. The pool must be set using `Init`
  /// before any other method is called. The given `capabilities` and `layout`
  /// must match those of the pool subsequently provided to `Init`.
  constexpr ForwardingPool(const Capabilities& capabilities,
                           Layout layout) noexcept
      : Pool(capabilities, layout) {}

  /// Creates a forwarding pool that wraps a given pool.
  ///
  /// This constructor should be used when the forwarding pool is created after
  /// the pool being wrapped.
  ///
  /// The pool must remain valid for the lifetime of this object.
  constexpr explicit ForwardingPool(Pool& pool) noexcept
      : Pool(pool.capabilities(), pool.layout()) {
    Init(pool);
  }

  /// Sets the pool being wrapped.
  ///
  /// It is an error to call this method if a pool was provided to the
  /// constructor. The `capabilities` of the given pool must match those of
  /// previously provided to the constructor.
  ///
  /// The pool must remain valid for the lifetime of this object.
  constexpr void Init(Pool& pool) {
    PW_ASSERT(pool_ == nullptr);
    PW_ASSERT(capabilities() == pool.capabilities());
    PW_ASSERT(layout() == pool.layout());
    pool_ = &pool;
  }

  // Accessors

  constexpr Pool& pool() {
    PW_ASSERT(pool_ != nullptr);
    return *pool_;
  }

  constexpr const Pool& pool() const {
    PW_ASSERT(pool_ != nullptr);
    return *pool_;
  }

  // API methods

  /// @copydoc Pool::Allocate
  void* DoAllocate() override { return pool().Allocate(); }

  /// @copydoc Deallocator::Deallocate
  void DoDeallocate(void* ptr) override { return pool().Deallocate(ptr); }

  /// @copydoc Deallocator::GetInfo
  Result<Layout> DoGetInfo(InfoType info_type, const void* ptr) const override {
    return Deallocator::GetInfo(pool(), info_type, ptr);
  }

 private:
  Pool* pool_ = nullptr;
};

}  // namespace pw::allocator
