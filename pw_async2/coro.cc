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

#include "pw_async2/internal/logging.h"
// logging.h must be included first

#include "pw_async2/coro.h"
#include "pw_log/log.h"

namespace pw::async2::internal {

void* CoroPromiseBase::SharedNew(CoroContext coro_cx,
                                 std::size_t size,
                                 std::size_t align) noexcept {
  PW_LOG_DEBUG("Allocating %zu B coroutine with %zu B alignment", size, align);

  auto ptr = coro_cx.allocator().Allocate(pw::allocator::Layout(size, align));
  if (ptr == nullptr) {
    PW_LOG_ERROR("Failed to allocate space for a coroutine of size %zu.", size);
  }
  return ptr;
}

}  // namespace pw::async2::internal
