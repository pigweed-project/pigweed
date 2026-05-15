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

#include "examples/custom_allocator.h"

#include <cstdint>

#include "examples/custom_allocator_test_harness.h"
#include "examples/named_u32.h"
#include "pw_allocator/fuzzing.h"
#include "pw_allocator/testing.h"
#include "pw_containers/vector.h"
#include "pw_fuzzer/fuzztest.h"
#include "pw_unit_test/framework.h"

namespace {

using AllocatorForTest = ::pw::allocator::test::AllocatorForTest<256>;

// DOCSTAG: [pw_allocator-examples-custom_allocator-unit_test]
// TODO: b/328076428 - Use pwrev/193642.
TEST(CustomAllocatorExample, MakeUnique) {
  AllocatorForTest allocator;
  constexpr size_t kThreshold = sizeof(examples::NamedU32) * 3;
  examples::CustomAllocator custom(allocator, kThreshold);
  // log_basic::test::LogCache<32> logs;

  auto ptr1 = custom.MakeUnique<examples::NamedU32>("test", 111u);
  auto ptr2 = custom.MakeUnique<examples::NamedU32>("test", 222u);
  auto ptr3 = custom.MakeUnique<examples::NamedU32>("test", 333u);
  // EXPECT_FALSE(logs.HasNext());

  auto ptr4 = custom.MakeUnique<examples::NamedU32>("test", 444u);
  // ASSERT_TRUE(logs.HasNext());
  // log_basic::test::LogMessage log = logs.Next();
  // EXPECT_EQ(log.level, PW_LOG_LEVEL_INFO);

  // StringBuffer<40> expected;
  // expected << "more than " << kThreshold << " bytes allocated.";
  // EXPECT_STREQ(log.message.c_str(), expected.c_str());
}
// DOCSTAG: [pw_allocator-examples-custom_allocator-unit_test]

// DOCSTAG: [pw_allocator-examples-custom_allocator-fuzz_test]
void NeverCrashes(const pw::Vector<pw::allocator::test::Request>& requests) {
  static examples::CustomAllocatorTestHarness harness;
  harness.HandleRequests(requests);
}

constexpr size_t kMaxRequests = 256;
constexpr size_t kMaxSize = examples::CustomAllocatorTestHarness::kCapacity / 2;
FUZZ_TEST(CustomAllocatorFuzzTest, NeverCrashes)
    .WithDomains(
        pw::allocator::test::ArbitraryRequests<kMaxRequests, kMaxSize>());
// DOCSTAG: [pw_allocator-examples-custom_allocator-fuzz_test]

}  // namespace
