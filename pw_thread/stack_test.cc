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

#include "pw_thread/stack.h"

#include <cstdint>
#include <type_traits>

#include "pw_polyfill/language_feature_macros.h"
#include "pw_thread/thread.h"  // for PW_THREAD_GENERIC_CREATION_IS_SUPPORTED
#include "pw_unit_test/framework.h"

namespace {

#if PW_THREAD_GENERIC_CREATION_IS_SUPPORTED

static_assert(std::is_pointer_v<pw::internal::ThreadStackPointer>);

// ThreadStacks must be constant initializable.
alignas(64) PW_CONSTINIT pw::ThreadStack<0> stack_0;
PW_CONSTINIT pw::ThreadStack<1> stack_1;
alignas(128) PW_CONSTINIT pw::ThreadStack<64> stack_64;

TEST(ThreadStack, NeverNullptr) {
  EXPECT_NE(stack_0.native_pointer(), nullptr);
  EXPECT_NE(stack_1.native_pointer(), nullptr);
  EXPECT_NE(stack_64.native_pointer(), nullptr);
}

TEST(ThreadStack, Alignment64) {
  uintptr_t pointer = reinterpret_cast<uintptr_t>(stack_0.native_pointer());
  EXPECT_EQ(pointer % 64, 0u);
}

TEST(ThreadStack, Alignment128) {
  uintptr_t pointer = reinterpret_cast<uintptr_t>(stack_64.native_pointer());
  EXPECT_EQ(pointer % 128, 0u);
}

#endif  // PW_THREAD_GENERIC_CREATION_IS_SUPPORTED

}  // namespace
