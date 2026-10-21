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

#include "pw_thread/attrs.h"

#include "pw_compilation_testing/negative_compilation.h"
#include "pw_polyfill/language_feature_macros.h"
#include "pw_thread/priority.h"
#include "pw_thread/stack.h"
#include "pw_thread/thread.h"  // for PW_THREAD_GENERIC_CREATION_IS_SUPPORTED
#include "pw_unit_test/constexpr.h"
#include "pw_unit_test/framework.h"

namespace {

PW_CONSTEXPR_TEST(ThreadAttrs, Defaults, {
  pw::ThreadAttrs attrs;
  PW_TEST_EXPECT_STREQ(attrs.name(), "");
  PW_TEST_EXPECT_EQ(attrs.priority(), pw::ThreadPriority::Default());
  PW_TEST_EXPECT_EQ(attrs.stack_size_bytes(),
                    pw::thread::backend::kDefaultStackSizeBytes);
  PW_TEST_EXPECT_EQ(attrs.native_stack_pointer(), nullptr);
  PW_TEST_EXPECT_FALSE(attrs.has_external_stack());
});

PW_CONSTEXPR_TEST(ThreadAttrs, SetAttributes, {
  pw::ThreadAttrs attrs = pw::ThreadAttrs()
                              .set_name("hello")
                              .set_priority(pw::ThreadPriority::High())
                              .set_stack_size_bytes(123);
  PW_TEST_EXPECT_STREQ(attrs.name(), "hello");
  PW_TEST_EXPECT_EQ(attrs.priority(), pw::ThreadPriority::High());
  PW_TEST_EXPECT_EQ(attrs.stack_size_bytes(), 123u);
  PW_TEST_EXPECT_EQ(attrs.native_stack_pointer(), nullptr);
  PW_TEST_EXPECT_FALSE(attrs.has_external_stack());
});

// Declaring a ThreadStack requires generic thread creation.
#if PW_THREAD_GENERIC_CREATION_IS_SUPPORTED

PW_CONSTINIT pw::ThreadStack<0> stack;

PW_CONSTEXPR_TEST(ThreadAttrs, ExternalStack, {
  pw::ThreadAttrs attrs = pw::ThreadAttrs().set_stack(stack);

  PW_TEST_EXPECT_TRUE(attrs.has_external_stack());
  PW_TEST_EXPECT_NE(attrs.native_stack_pointer(), nullptr);
});

TEST(ThreadAttrs, ExternalStackStackSize) {
  pw::ThreadAttrs attrs = pw::ThreadAttrs().set_stack(stack);
  EXPECT_EQ(attrs.native_stack_size(), attrs.native_stack().size());
}

#if PW_NC_TEST(CannotSetStackSizeWithExternalStack)
PW_NC_EXPECT("PW_ASSERT\(!has_external_stack\(\)\)");
[[maybe_unused]] constexpr pw::ThreadAttrs kTestAttrs =
    pw::ThreadAttrs().set_stack(stack).set_stack_size_bytes(1);
#endif  // PW_NC_TEST

#if PW_NC_TEST(CannotAccessNativeStackWithoutStackSet)
PW_NC_EXPECT("PW_ASSERT\(has_external_stack\(\)\)");
[[maybe_unused]] constexpr auto kTest = pw::ThreadAttrs().native_stack();
#endif  // PW_NC_TEST

#endif  // PW_THREAD_GENERIC_CREATION_IS_SUPPORTED

}  // namespace
