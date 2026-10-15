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

#include "pw_allocator/abstract_deallocator.h"
#include "pw_allocator/capability.h"
#include "pw_allocator/layout.h"
#include "pw_allocator/pool.h"

namespace pw::allocator {

/// @submodule{pw_allocator,concrete}

/// A memory pool that provides default implementations for optional methods.
///
/// This type isn't instantiated directly, but is provided to make it easier to
/// define other pool types.
class AbstractPool : public internal::BasicAbstractDeallocator<Pool> {
 private:
  using Base = internal::BasicAbstractDeallocator<Pool>;

 protected:
  constexpr explicit AbstractPool(const Layout& layout)
      : AbstractPool(0, layout) {}

  constexpr AbstractPool(Capabilities capabilities, const Layout& layout)
      : Base(capabilities, layout) {}
};

/// @}

}  // namespace pw::allocator
