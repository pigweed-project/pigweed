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

#include "pw_async2/join.h"

#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/value_future.h"
#include "pw_compilation_testing/negative_compilation.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::async2::BroadcastValueProvider;
using ::pw::async2::DispatcherForTest;
using ::pw::async2::Join;
using ::pw::async2::Pending;
using ::pw::async2::Poll;

static_assert(
    pw::async2::Future<pw::async2::JoinFuture<pw::async2::VoidFuture,
                                              pw::async2::ValueFuture<int>>>);

TEST(JoinFuture, ReturnsReadyWhenAllFuturesAreReady) {
  DispatcherForTest dispatcher;

  BroadcastValueProvider<int> int_provider;
  BroadcastValueProvider<char> char_provider;

  auto future = Join(int_provider.Get(), char_provider.Get());
  EXPECT_EQ(dispatcher.RunInTaskUntilStalled(future), Pending());
  int_provider.Resolve(43);
  EXPECT_EQ(dispatcher.RunInTaskUntilStalled(future), Pending());
  char_provider.Resolve('d');
  auto&& result = dispatcher.RunInTaskUntilStalled(future);
  ASSERT_TRUE(result.IsReady());
  auto&& [i, c] = *result;
  EXPECT_EQ(i, 43);
  EXPECT_EQ(c, 'd');
  EXPECT_TRUE(future.is_complete());
}

#if PW_NC_TEST(ArgumentsToJoinMustBeFutures)
PW_NC_EXPECT("All arguments to Join must be Future types");
void ShouldAssert() {
  auto not_a_future = []() -> int { return 42; };
  auto future = ::pw::async2::Join(not_a_future());
}
#endif  // PW_NC_TEST

}  // namespace
