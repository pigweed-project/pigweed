// Copyright 2019 The Pigweed Authors
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

#include <cstring>

#include "pw_status/status.h"
#include "pw_string/string_builder.h"
#include "pw_unit_test/framework.h"

namespace pw {

/// A type which compares equal to itself and FAILs if stringified.
struct ExplodingString {
  friend bool operator==(const ExplodingString&, const ExplodingString&);
};

bool operator==(const ExplodingString&, const ExplodingString&) { return true; }

template <>
inline StatusWithSize ToString(const ExplodingString&, span<char>) {
  ADD_FAILURE();
  return StatusWithSize::Unimplemented();
}

namespace {

TEST(UnknownTypeToString, SmallObject) {
  struct {
    char a = 0xa1;
  } object;

  StringBuffer<64> expected;
  expected << "<1-byte object at 0x" << &object << '>';
  ASSERT_EQ(OkStatus(), expected.status());

  StringBuffer<64> actual;
  actual << object;
  ASSERT_EQ(OkStatus(), actual.status());
  EXPECT_STREQ(expected.c_str(), actual.c_str());
}

TEST(UnknownTypeToString, NineByteObject) {
  struct {
    char a[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  } object;

  StringBuffer<64> expected;
  expected << "<9-byte object at 0x" << &object << '>';
  ASSERT_EQ(OkStatus(), expected.status());

  StringBuffer<64> actual;
  actual << object;
  ASSERT_EQ(OkStatus(), actual.status());
  EXPECT_STREQ(expected.c_str(), actual.c_str());
}

TEST(UnknownTypeToString, TenByteObject) {
  struct {
    char a[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  } object;

  StringBuffer<72> expected;
  expected << "<10-byte object at 0x" << &object << '>';
  ASSERT_EQ(OkStatus(), expected.status());

  StringBuffer<72> actual;
  actual << object;
  ASSERT_EQ(OkStatus(), actual.status());
  EXPECT_STREQ(expected.c_str(), actual.c_str());
}

TEST(ExpectEq, ObjectsAreNotStringifiedUnlessExpectationFails) {
  EXPECT_EQ(ExplodingString{}, ExplodingString{});
}

TEST(TestFilter, MatchesFilterUnionSemantics) {
  unit_test::TestCase case_a{"SuiteA", "Test1", "file.cc"};
  unit_test::TestCase case_b{"SuiteB", "Test2", "file.cc"};
  unit_test::TestCase case_c{"SuiteC", "Test3", "file.cc"};

  // No filters set -> all match
  EXPECT_TRUE(unit_test::internal::Framework::Get().MatchesFilter(case_a));
  EXPECT_TRUE(unit_test::internal::Framework::Get().MatchesFilter(case_b));
  EXPECT_TRUE(unit_test::internal::Framework::Get().MatchesFilter(case_c));

  // Only suite filter
  unit_test::TestCaseFilter suite_filters[] = {{"SuiteA", ""}};
  unit_test::SetTestFilters(suite_filters);
  EXPECT_TRUE(unit_test::internal::Framework::Get().MatchesFilter(case_a));
  EXPECT_FALSE(unit_test::internal::Framework::Get().MatchesFilter(case_b));
  EXPECT_FALSE(unit_test::internal::Framework::Get().MatchesFilter(case_c));

  // Only test case filter
  unit_test::TestCaseFilter case_filters[] = {{"SuiteB", "Test2"}};
  unit_test::SetTestFilters(case_filters);
  EXPECT_FALSE(unit_test::internal::Framework::Get().MatchesFilter(case_a));
  EXPECT_TRUE(unit_test::internal::Framework::Get().MatchesFilter(case_b));
  EXPECT_FALSE(unit_test::internal::Framework::Get().MatchesFilter(case_c));

  // Union of suite and test case filters
  unit_test::TestCaseFilter union_filters[] = {{"SuiteA", ""},
                                               {"SuiteB", "Test2"}};
  unit_test::SetTestFilters(union_filters);
  EXPECT_TRUE(unit_test::internal::Framework::Get().MatchesFilter(case_a));
  EXPECT_TRUE(unit_test::internal::Framework::Get().MatchesFilter(case_b));
  EXPECT_FALSE(unit_test::internal::Framework::Get().MatchesFilter(case_c));

  // Wildcard suite filter (*.Test2 matches Test2 in any suite)
  unit_test::TestCaseFilter wildcard_suite_filters[] = {{"*", "Test2"}};
  unit_test::SetTestFilters(wildcard_suite_filters);
  EXPECT_FALSE(unit_test::internal::Framework::Get().MatchesFilter(case_a));
  EXPECT_TRUE(unit_test::internal::Framework::Get().MatchesFilter(case_b));
  EXPECT_FALSE(unit_test::internal::Framework::Get().MatchesFilter(case_c));

  // Wildcard test name filter (SuiteA.* matches all tests in SuiteA)
  unit_test::TestCaseFilter wildcard_test_filters[] = {{"SuiteA", "*"}};
  unit_test::SetTestFilters(wildcard_test_filters);
  EXPECT_TRUE(unit_test::internal::Framework::Get().MatchesFilter(case_a));
  EXPECT_FALSE(unit_test::internal::Framework::Get().MatchesFilter(case_b));
  EXPECT_FALSE(unit_test::internal::Framework::Get().MatchesFilter(case_c));

  // Suite-level checks
  unit_test::SetTestFilters(union_filters);
  EXPECT_TRUE(unit_test::internal::Framework::Get().MatchesFilter("SuiteA"));
  EXPECT_TRUE(unit_test::internal::Framework::Get().MatchesFilter("SuiteB"));
  EXPECT_FALSE(unit_test::internal::Framework::Get().MatchesFilter("SuiteC"));

  // Clean up
  unit_test::SetTestFilters({});
}

}  // namespace
}  // namespace pw
