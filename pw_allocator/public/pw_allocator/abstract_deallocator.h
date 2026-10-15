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

#include <type_traits>
#include <utility>

#include "pw_allocator/capability.h"
#include "pw_allocator/deallocator.h"
#include "pw_allocator/layout.h"
#include "pw_result/result.h"
#include "pw_status/status.h"

namespace pw::allocator {
namespace internal {

/// Mix-in that provides default implementations of optional `pw::Deallocator`
/// methods for a `Base` type that derives from `pw::Deallocator` (such as
/// `pw::Deallocator`, `pw::Allocator`, or `pw::allocator::Pool`).
template <typename Base>
class BasicAbstractDeallocator : public Base {
 protected:
  static_assert(std::is_base_of_v<pw::Deallocator, Base>,
                "Base must derive from pw::Deallocator");

  using Base::Base;

  /// @copydoc Deallocator::GetInfo
  Result<Layout> DoGetInfo(typename Base::InfoType,
                           const void*) const override {
    return Status::Unimplemented();
  }
};

}  // namespace internal

/// @submodule{pw_allocator,concrete}

/// A memory deallocator that provides default implementations for optional
/// methods.
///
/// This type isn't instantiated directly, but is provided to make it easier to
/// define other deallocator types.
class AbstractDeallocator
    : public internal::BasicAbstractDeallocator<pw::Deallocator> {
 private:
  using Base = internal::BasicAbstractDeallocator<pw::Deallocator>;

 protected:
  constexpr explicit AbstractDeallocator(Capabilities capabilities = 0)
      : Base(capabilities) {}
};

/// @}

}  // namespace pw::allocator
