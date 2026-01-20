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

#include "context_usage.h"

#include "pw_interrupt/fake_context.h"
#include "pw_unit_test/framework.h"

namespace pw::interrupt::examples {

// Example unit test of a library that uses `//pw_interrupt:context`, which uses
// `//pw_interrupt:fake_context` to allow the unit test to control what the
// library observes the state to be.

TEST(ContextUsageTest, ReturnsFalseByDefault) {
  // By default fake_context will report that the current thread is not in an
  // interrupt context.
  EXPECT_FALSE(ExampleInInterruptContext());
}

TEST(ContextUsageTest, CanUseACallbackToOverrideTheDefault) {
  // fake_context allows a callback function to be set to obtain the value to
  // return for any `pw::interrupt::InInterruptContext` call.

  bool retval = false;
  pw::interrupt::fake::SetInInterruptContextCallback([&] { return retval; });

  EXPECT_FALSE(ExampleInInterruptContext());

  retval = true;
  EXPECT_TRUE(ExampleInInterruptContext());
}

}  // namespace pw::interrupt::examples
