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

#include "pw_unit_test/framework.h"

#include <cstring>

#include "pw_assert/check.h"
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"

namespace pw {
namespace {

TEST(PigweedTest, ExpectBool) {
  EXPECT_TRUE(true);
  EXPECT_FALSE(false);

  EXPECT_TRUE(1);
  EXPECT_TRUE(1203492);
  EXPECT_TRUE(-1);
  EXPECT_TRUE(0.1f);

  EXPECT_FALSE(0);
  EXPECT_FALSE(0.0f);
  EXPECT_FALSE(-0.0f);
}

TEST(PigweedTest, ExpectBasicComparisons) {
  EXPECT_EQ(1, 1 + 0);
  ASSERT_EQ(1, 1 + 0);

  EXPECT_EQ(0.0f, -0.0f);
  ASSERT_EQ(0.0f, -0.0f);

  EXPECT_NE(-1, 0);
  ASSERT_NE(-1, 0);

  EXPECT_GT(2, 1);
  ASSERT_GT(3, 0);

  EXPECT_GE(1, 1);
  ASSERT_GE(3, 0);

  EXPECT_LT(0, 1);
  ASSERT_LT(-2, 1209);

  EXPECT_LE(-1, 0);
  ASSERT_LE(-2, -2);
}

TEST(PigweedTest, ExpectNearComparisons) {
  EXPECT_NEAR(1, 2, 1);
  ASSERT_NEAR(1, 2, 1);

  EXPECT_NEAR(-5, 5, 10);
  ASSERT_NEAR(-5, 5, 10);

  int x = 17;
  int epsilon = 5;

  EXPECT_NEAR(x, 15, epsilon);
  ASSERT_NEAR(x, 15, epsilon);
}

TEST(PigweedTest, ExpectFloatComparisons) {
  EXPECT_FLOAT_EQ(5.0f, 10.0f / 2);
  ASSERT_FLOAT_EQ(5.0f, 10.0f / 2);

  EXPECT_FLOAT_EQ(-0.5f, -5.0f / 10);
  ASSERT_FLOAT_EQ(-0.5f, -5.0f / 10);

  float x = 17.0f / 20.0f;

  EXPECT_FLOAT_EQ(x, 17.0f / 20.0f);
  ASSERT_FLOAT_EQ(x, 17.0f / 20.0f);
}

TEST(PigweedTest, ExpectDoubleComparisons) {
  EXPECT_DOUBLE_EQ(5.0, 10.0 / 2);
  ASSERT_DOUBLE_EQ(5.0, 10.0 / 2);

  EXPECT_DOUBLE_EQ(-0.5, -5.0 / 10);
  ASSERT_DOUBLE_EQ(-0.5, -5.0 / 10);

  double x = 17.0 / 20.0;

  EXPECT_DOUBLE_EQ(x, 17.0 / 20.0);
  ASSERT_DOUBLE_EQ(x, 17.0 / 20.0);
}

TEST(PigweedTest, ExpectStringEquality) {
  EXPECT_STREQ("", "");
  EXPECT_STREQ("Yes", "Yes");

  char no[] = {'N', 'o', '\0'};
  ASSERT_STREQ("No", no);

  EXPECT_STRNE("NO", "no");
  ASSERT_STRNE("yes", no);

  const char* invalid_string = nullptr;
  EXPECT_STREQ(invalid_string, nullptr);
  EXPECT_STRNE("abc", nullptr);
}

TEST(PigweedTest, SucceedAndFailMacros) {
  SUCCEED();

  // The ADD_FAILURE() and FAIL() macros cause a test to fail if they are
  // reached. Use them, but don't let them run so that this test still passes.
  if (false) {
    ADD_FAILURE();
    FAIL();
  }

  // Without braces, clang-tidy complains if these are multiple statements.
  if (false)
    ADD_FAILURE();

  if (false)
    FAIL();
}

TEST(PigweedTest, SkipMacro) {
  // Without a brace, clang-tidy complains if GTEST_SKIP is multiple statements.
  if (false)
    GTEST_SKIP();

  GTEST_SKIP();
  // This code should not run.
  EXPECT_TRUE(false);
}

TEST(PigweedTest, Logs) {
  EXPECT_TRUE(true) << "This message is ignored";
  EXPECT_FALSE(false) << "This message is ignored";
  EXPECT_EQ(0, 0) << "This message is ignored";
  EXPECT_NE(0, 1) << "This message is ignored";
  EXPECT_GT(1, 0) << "This message is ignored";
  EXPECT_GE(0, 0) << "This message is ignored";
  EXPECT_LT(0, 1) << "This message is ignored";
  EXPECT_LE(0, 0) << "This message is ignored";
  EXPECT_STREQ("", "") << "This message is ignored";
  EXPECT_STRNE("", "?") << "This message is ignored";

  ASSERT_TRUE(true) << "This message is ignored";
  ASSERT_FALSE(false) << "This message is ignored";
  ASSERT_EQ(0, 0) << "This message is ignored";
  ASSERT_NE(0, 1) << "This message is ignored";
  ASSERT_GT(1, 0) << "This message is ignored";
  ASSERT_GE(0, 0) << "This message is ignored";
  ASSERT_LT(0, 1) << "This message is ignored";
  ASSERT_LE(0, 0) << "This message is ignored";
  ASSERT_STREQ("", "") << "This message is ignored";
  ASSERT_STRNE("", "?") << "This message is ignored";

  if (false) {
    ADD_FAILURE() << "This failed!" << 123;
    GTEST_FAIL() << "This failed!" << 123 << '?';
    GTEST_SKIP() << 1.0f << " skips!";
  }
  GTEST_SUCCEED() << "This message is ignored";
}

TEST(PigweedTest, PwOkMatchers) {
  PW_TEST_ASSERT_OK(OkStatus());
  PW_TEST_ASSERT_OK(StatusWithSize(123));
  PW_TEST_ASSERT_OK(Result<int>(123));

  PW_TEST_EXPECT_OK(OkStatus());
  PW_TEST_EXPECT_OK(StatusWithSize(123));
  PW_TEST_EXPECT_OK(Result<int>(123));

  PW_TEST_ASSERT_OK_AND_ASSIGN(auto val, Result<int>(123));
  EXPECT_EQ(val, 123);
  PW_TEST_ASSERT_OK_AND_ASSIGN(auto size, StatusWithSize(123U));
  EXPECT_EQ(size, 123U);
}

TEST(AssertOkAndAssign, AssignsOkValueToNewLvalue) {
  const auto value = Result<int>(5);
  PW_TEST_ASSERT_OK_AND_ASSIGN(int declare_and_assign, value);
  EXPECT_EQ(5, declare_and_assign);
}

TEST(AssertOkAndAssign, AssignsOkValueToExistingLvalue) {
  const auto value = Result<int>(5);
  int existing_value = 0;
  PW_TEST_ASSERT_OK_AND_ASSIGN(existing_value, value);
  EXPECT_EQ(5, existing_value);
}

TEST(AssertOkAndAssign, AssignsExistingLvalueToConstReference) {
  const auto value = Result<int>(5);
  PW_TEST_ASSERT_OK_AND_ASSIGN(const auto& ref, value);
  EXPECT_EQ(5, ref);
}

class CopyMoveCounter {
 public:
  CopyMoveCounter() = delete;
  CopyMoveCounter(int& copies, int& moves) : copies_(&copies), moves_(&moves) {}
  CopyMoveCounter(const CopyMoveCounter& other)
      : copies_(other.copies_), moves_(other.moves_) {
    ++(*copies_);
  }
  CopyMoveCounter(CopyMoveCounter&& other)
      : copies_(other.copies_), moves_(other.moves_) {
    ++(*moves_);
  }
  CopyMoveCounter& operator=(const CopyMoveCounter& other) {
    copies_ = other.copies_;
    moves_ = other.moves_;
    ++(*copies_);
    return *this;
  }
  CopyMoveCounter& operator=(CopyMoveCounter&& other) {
    copies_ = other.copies_;
    moves_ = other.moves_;
    ++(*moves_);
    return *this;
  }

 private:
  int* copies_;
  int* moves_;
};

TEST(AssertOkAndAssign, OkRvalueDoesNotCopy) {
  int copies = 0;
  int moves = 0;
  PW_TEST_ASSERT_OK_AND_ASSIGN([[maybe_unused]] CopyMoveCounter cm,
                               Result(CopyMoveCounter(copies, moves)));
  EXPECT_EQ(copies, 0);
  EXPECT_EQ(moves, 2);
}

TEST(AssertOkAndAssign, OkLvalueMovedDoesNotCopy) {
  int copies = 0;
  int moves = 0;
  Result result(CopyMoveCounter(copies, moves));
  PW_TEST_ASSERT_OK_AND_ASSIGN([[maybe_unused]] CopyMoveCounter cm,
                               std::move(result));
  EXPECT_EQ(copies, 0);
  EXPECT_EQ(moves, 3);
}

TEST(AssertOkAndAssign, OkLvalueCopiesOnce) {
  int copies = 0;
  int moves = 0;
  Result result(CopyMoveCounter(copies, moves));
  PW_TEST_ASSERT_OK_AND_ASSIGN([[maybe_unused]] CopyMoveCounter cm, result);
  EXPECT_EQ(copies, 1);
  EXPECT_EQ(moves, 2);
}

// The following test contents are disabled and is only for checking what
// failure cases would look like. For example, when enabling the test,
// the output is:
// clang-format off
// ERR  pw_unit_test/framework_test.cc:294: Failure
// ERR        Expected: ::pw::internal::ConvertToStatus(Status::Unknown()) == pw::OkStatus()
// ERR          Actual: UNKNOWN == OK
// ERR  pw_unit_test/framework_test.cc:295: Failure
// ERR        Expected: ::pw::internal::ConvertToStatus(Status::Unknown()) == pw::OkStatus()
// ERR          Actual: UNKNOWN == OK
// clang-format on
TEST(TestMatchers, SampleFailures) {
  if (false) {
    PW_TEST_EXPECT_OK(Status::Unknown());
    PW_TEST_ASSERT_OK(Status::Unknown());
  }
}

class SkipOnSetUpTest : public ::testing::Test {
 public:
  void SetUp() override { GTEST_SKIP(); }
};

TEST_F(SkipOnSetUpTest, FailTest) {
  // This code should not run because the test was skipped in SetUp().
  EXPECT_TRUE(false);
}

class NonCopyable {
 public:
  NonCopyable(int value) : value_(value) {}

  NonCopyable(const NonCopyable&) = delete;
  NonCopyable& operator=(const NonCopyable&) = delete;

  bool operator==(const NonCopyable& rhs) const { return value_ == rhs.value_; }
  bool operator!=(const NonCopyable& rhs) const { return value_ != rhs.value_; }

  operator bool() const { return value_ > 0; }

 private:
  const int value_;
};

TEST(PigweedTest, NonCopyableType) {
  EXPECT_TRUE(NonCopyable(6));
  EXPECT_FALSE(NonCopyable(-1));

  const NonCopyable this_one(100);
  EXPECT_EQ(this_one, this_one);
  EXPECT_TRUE(this_one);

  EXPECT_EQ(NonCopyable(5), NonCopyable(5));
  EXPECT_NE(NonCopyable(5), NonCopyable(6));
}

bool Increment(int* i) {
  (*i)++;
  return true;
}

TEST(PigweedTest, MacroArgumentsOnlyAreEvaluatedOnce) {
  int i = 1;

  EXPECT_TRUE(Increment(&i));
  EXPECT_EQ(i, 2);
  ASSERT_TRUE(Increment(&i));
  EXPECT_EQ(i, 3);

  EXPECT_EQ(0x600dbeef, [&i]() {
    i += 1;
    return 0x600dbeef;
  }());

  EXPECT_EQ(i, 4);
}

class ClassWithPrivateMethod {
  FRIEND_TEST(FixtureTest, FriendClass);

 private:
  int Return314() { return 314; }
};

class FixtureTest : public ::testing::Test {
 public:
  FixtureTest() : string_("hello world") {}

  bool ReturnTrue() { return true; }
  int StringLength() { return std::strlen(string_); }

 protected:
  const char* string_;
};

TEST_F(FixtureTest, CustomFixture) {
  EXPECT_TRUE(ReturnTrue());
  EXPECT_EQ(StringLength(), 11);
}

TEST_F(FixtureTest, FriendClass) {
  EXPECT_EQ(ClassWithPrivateMethod().Return314(), 314);
}

class PigweedTestFixture : public ::testing::Test {
 protected:
  PigweedTestFixture() : cool_number_(35) {}

  int cool_number_;
};

TEST_F(PigweedTestFixture, TheNumberIs35) {
  EXPECT_EQ(cool_number_, 35);
  cool_number_ += 1;
  EXPECT_EQ(cool_number_, 36);
}

TEST_F(PigweedTestFixture, YupTheNumberIs35) {
  EXPECT_EQ(cool_number_, 35);
  cool_number_ *= 100;
  EXPECT_EQ(cool_number_, 3500);
}

class Expectations : public ::testing::Test {
 protected:
  Expectations() : cool_number_(3) { PW_CHECK_INT_EQ(cool_number_, 3); }

  ~Expectations() override { PW_CHECK_INT_EQ(cool_number_, 14159); }

  int cool_number_;
};

TEST_F(Expectations, SetCoolNumber) { cool_number_ = 14159; }

class SetUpAndTearDown : public ::testing::Test {
 public:
  static int value;

  static void SetUpTestSuite() {
    value = 1;
    EXPECT_EQ(value, 1);
    value++;
  }

  static void TearDownTestSuite() {
    EXPECT_EQ(value, 7);
    value++;
  }

 protected:
  SetUpAndTearDown() {
    EXPECT_EQ(value, 2);
    value++;
  }

  ~SetUpAndTearDown() override {
    EXPECT_EQ(value, 6);
    value++;
  }

  void SetUp() override {
    EXPECT_EQ(value, 3);
    value++;
  }

  void TearDown() override {
    EXPECT_EQ(value, 5);
    value++;
  }
};

int SetUpAndTearDown::value = 1;

TEST_F(SetUpAndTearDown, MakeSureItIsSet) {
  EXPECT_EQ(value, 4);
  value++;
}

TEST(TestSuiteTearDown, MakeSureItRan) {
  EXPECT_EQ(SetUpAndTearDown::value, 8);
}

class Interleaved : public ::testing::Test {
 public:
  static void SetUpTestSuite() { suites_running++; }
  static void TearDownTestSuite() { suites_running--; }

 protected:
  static int suites_running;
};

int Interleaved::suites_running = 0;

class InterleavedA : public Interleaved {};
class InterleavedB : public Interleaved {};

TEST_F(InterleavedA, Test1) { ASSERT_EQ(suites_running, 1); }
TEST_F(InterleavedB, Test1) { ASSERT_EQ(suites_running, 1); }
TEST_F(Interleaved, Test12) { ASSERT_EQ(suites_running, 1); }
TEST_F(InterleavedB, Test2) { ASSERT_EQ(suites_running, 1); }
TEST_F(InterleavedA, Test2) { ASSERT_EQ(suites_running, 1); }

TEST(PigweedTest, ScopedTrace) {
  // Verify we can call SCOPED_TRACE, even though it is a no-op.
  // TODO: https://pwbug.dev/441096262 - Add SCOPED_TRACE support.
  SCOPED_TRACE("text");
}

TEST(ExpectDeath, StatementIsNotEvaluated) {
  int i = 0;
  EXPECT_DEATH_IF_SUPPORTED(
      {
        i += 1;
        PW_CRASH("This party's over.");
      },
      "This party's over");
  EXPECT_EQ(i, 0);
}

}  // namespace
}  // namespace pw
