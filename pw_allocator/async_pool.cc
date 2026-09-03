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

#include "pw_allocator/async_pool.h"

namespace pw::allocator {

void AsyncPool::DoDeallocate(void* ptr) {
  pool().Deallocate(ptr);
  waker_.Wake();
}

async2::Poll<void*> AsyncPool::PendAllocate(async2::Context& context) {
  void* ptr = pool().Allocate();
  if (ptr == nullptr) {
    PW_ASYNC_STORE_WAKER(context, waker_, "waiting for pool memory");
    return async2::Pending();
  }
  return async2::Ready(ptr);
}

}  // namespace pw::allocator
