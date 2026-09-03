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

#include "pw_allocator/deallocator.h"
#include "pw_allocator/forwarding_pool.h"
#include "pw_async2/poll.h"
#include "pw_async2/task.h"
#include "pw_async2/waker.h"

namespace pw::allocator {

/// @submodule{pw_allocator,concrete}

/// A wrapper around a Pool that allows for asynchronous allocation.
///
/// This class is not thread safe. It should only be used from the dispatcher
/// thread, or wrapped to provided synchronized access.
class AsyncPool : public ForwardingPool {
 public:
  constexpr explicit AsyncPool(Pool& pool) : ForwardingPool(pool) {}

  /// Asynchronously allocates a chunk of memory with the fixed layout from the
  /// pool.
  async2::Poll<void*> PendAllocate(async2::Context& context);

 protected:
  /// @copydoc Deallocator::Deallocate
  void DoDeallocate(void* ptr) override;

 private:
  async2::Waker waker_;
};

/// @}

}  // namespace pw::allocator
