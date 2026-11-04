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

#include "pw_result/result.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_string/string.h"
#include "pw_unit_test/framework.h"

namespace {

// Upstream-only `internal_strict_warnings` (gated on `pigweed_is_root()`)
// enables `-Wextra-semi`. Including an extra semicolon after a function
// definition verifies at compile time that `@pigweed`'s internal-only strict
// warnings are not leaked to downstream non-root consumers.
pw::Result<int> AddValues(int a, int b) { return a + b; };

TEST(NonRootModuleTest, UsesPigweedLibrariesFromExternalWorkspace) {
  pw::Result<int> sum = AddValues(20, 22);
  ASSERT_EQ(sum.status(), pw::OkStatus());
  EXPECT_EQ(*sum, 42);

  pw::InlineString<16> greeting("pigweed");
  EXPECT_EQ(greeting.size(), 7u);

  constexpr int kValues[] = {1, 2, 3};
  pw::span<const int> span_values(kValues);
  EXPECT_EQ(span_values.size(), 3u);
}

}  // namespace
