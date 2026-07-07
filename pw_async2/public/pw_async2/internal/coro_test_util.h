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

#include <utility>

namespace pw::async2::test {

// Newer versions of the Clang/LLVM toolchain enable HALO optimizations where
// coroutine state may be allocated on the stack if the compiler can prove that
// the coroutine context does not need to persist beyond the lifetime of the
// function that creates it. Some tests need to ensure heap allocation occurs.
// This helper abstracts away the details needed to do so.
template <typename Coro>
Coro&& EnsureNotStackAllocated(Coro&& c) {
#if defined(__GNUC__) || defined(__clang__)
  asm volatile("" : : "g"(&c) : "memory");
#endif
  return std::forward<Coro>(c);
}

}  // namespace pw::async2::test
