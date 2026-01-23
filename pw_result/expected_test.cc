// Copyright 2023 The Pigweed Authors
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

#include "pw_result/expected.h"

#include "pw_status/try.h"
#include "pw_string/string.h"
#include "pw_string/string_builder.h"
#include "pw_unit_test/framework.h"

namespace pw {
namespace {

struct Defaults {
  Defaults() = default;
  Defaults(const Defaults&) = default;
  Defaults(Defaults&&) = default;
  Defaults& operator=(const Defaults&) = default;
  Defaults& operator=(Defaults&&) = default;
};

struct NoDefaultConstructor {
  NoDefaultConstructor() = delete;
  NoDefaultConstructor(std::nullptr_t) {}
};

struct NoCopy {
  NoCopy(const NoCopy&) = delete;
  NoCopy(NoCopy&&) = default;
  NoCopy& operator=(const NoCopy&) = delete;
  NoCopy& operator=(NoCopy&&) = default;
};

struct NoCopyNoMove {
  NoCopyNoMove(const NoCopyNoMove&) = delete;
  NoCopyNoMove(NoCopyNoMove&&) = delete;
  NoCopyNoMove& operator=(const NoCopyNoMove&) = delete;
  NoCopyNoMove& operator=(NoCopyNoMove&&) = delete;
};

struct NonTrivialDestructor {
  ~NonTrivialDestructor() {}
};

namespace test_constexpr {
// Expected and unexpected are constexpr types.
constexpr expected<int, int> kExpectedConstexpr1;
constexpr expected<int, int> kExpectedConstexpr2{5};
constexpr expected<int, int> kExpectedConstexpr3 = unexpected<int>(42);
constexpr unexpected<int> kExpectedConstexprUnexpected{50};
static_assert(kExpectedConstexpr1.has_value());
static_assert(kExpectedConstexpr1.value() == 0);
static_assert(kExpectedConstexpr2.has_value());
static_assert(kExpectedConstexpr2.value() == 5);
static_assert(!kExpectedConstexpr3.has_value());
static_assert(kExpectedConstexpr3.error() == 42);
static_assert(kExpectedConstexprUnexpected.error() == 50);
}  // namespace test_constexpr

namespace test_default_construction {
// Default constructible if and only if T is default constructible.
static_assert(
    std::is_default_constructible<expected<Defaults, Defaults>>::value);
static_assert(!std::is_default_constructible<
              expected<NoDefaultConstructor, Defaults>>::value);
static_assert(std::is_default_constructible<
              expected<Defaults, NoDefaultConstructor>>::value);
static_assert(!std::is_default_constructible<
              expected<NoDefaultConstructor, NoDefaultConstructor>>::value);
// Never default constructible.
static_assert(!std::is_default_constructible<unexpected<Defaults>>::value);
static_assert(
    !std::is_default_constructible<unexpected<NoDefaultConstructor>>::value);
}  // namespace test_default_construction

namespace test_copy_construction {
// Copy constructible if and only if both types are copy constructible.
static_assert(std::is_copy_constructible<expected<Defaults, Defaults>>::value);
static_assert(!std::is_copy_constructible<expected<Defaults, NoCopy>>::value);
static_assert(!std::is_copy_constructible<expected<NoCopy, Defaults>>::value);
static_assert(!std::is_copy_constructible<expected<NoCopy, NoCopy>>::value);
// Copy constructible if and only if E is copy constructible.
static_assert(std::is_copy_constructible<unexpected<Defaults>>::value);
static_assert(!std::is_copy_constructible<unexpected<NoCopy>>::value);
}  // namespace test_copy_construction

namespace test_copy_assignment {
// Copy assignable if and only if both types are copy assignable.
static_assert(std::is_copy_assignable<expected<Defaults, Defaults>>::value);
static_assert(!std::is_copy_assignable<expected<Defaults, NoCopy>>::value);
static_assert(!std::is_copy_assignable<expected<NoCopy, Defaults>>::value);
static_assert(!std::is_copy_assignable<expected<NoCopy, NoCopy>>::value);
// Copy assignable if and only if E is copy assignable.
static_assert(std::is_copy_assignable<unexpected<Defaults>>::value);
static_assert(!std::is_copy_assignable<unexpected<NoCopy>>::value);
}  // namespace test_copy_assignment

namespace test_move_construction {
// Move constructible if and only if both types are move constructible.
static_assert(std::is_move_constructible<expected<Defaults, Defaults>>::value);
static_assert(
    !std::is_move_constructible<expected<Defaults, NoCopyNoMove>>::value);
static_assert(
    !std::is_move_constructible<expected<NoCopyNoMove, Defaults>>::value);
static_assert(
    !std::is_move_constructible<expected<NoCopyNoMove, NoCopyNoMove>>::value);
// Move constructible if and only if E is move constructible.
static_assert(std::is_move_constructible<unexpected<Defaults>>::value);
static_assert(!std::is_move_constructible<unexpected<NoCopyNoMove>>::value);
}  // namespace test_move_construction

namespace test_move_assignment {
// Move assignable if and only if both types are move assignable.
static_assert(std::is_move_assignable<expected<Defaults, Defaults>>::value);
static_assert(
    !std::is_move_assignable<expected<Defaults, NoCopyNoMove>>::value);
static_assert(
    !std::is_move_assignable<expected<NoCopyNoMove, Defaults>>::value);
static_assert(
    !std::is_move_assignable<expected<NoCopyNoMove, NoCopyNoMove>>::value);
// Move assignable if and only if E is move assignable.
static_assert(std::is_move_assignable<unexpected<Defaults>>::value);
static_assert(!std::is_move_assignable<unexpected<NoCopyNoMove>>::value);
}  // namespace test_move_assignment

namespace test_trivial_destructor {
// Destructor is trivial if and only if both types are trivially destructible.
static_assert(
    std::is_trivially_destructible<expected<Defaults, Defaults>>::value);
static_assert(!std::is_trivially_destructible<
              expected<NonTrivialDestructor, Defaults>>::value);
static_assert(!std::is_trivially_destructible<
              expected<Defaults, NonTrivialDestructor>>::value);
static_assert(!std::is_trivially_destructible<
              expected<NonTrivialDestructor, NonTrivialDestructor>>::value);
// Destructor is trivial if and only if E is trivially destructible.
static_assert(std::is_trivially_destructible<unexpected<Defaults>>::value);
static_assert(
    !std::is_trivially_destructible<unexpected<NonTrivialDestructor>>::value);
}  // namespace test_trivial_destructor

using SmallString = pw::InlineString<2>;

constexpr char kRecoverySentinel = 0x04;  // Arbitrary non-printable sentinel.

SmallString Itoa(int x) {
  SmallString result;
  if ((StringBuilder(result) << x).status() != OkStatus()) {
    ADD_FAILURE();
  }
  return result;
}

expected<int, std::string_view> FailableFunction1(bool fail, int num) {
  if (fail) {
    return unexpected("FailableFunction1");
  }
  return num;
}

expected<SmallString, std::string_view> FailableFunction2(bool fail, int num) {
  if (fail) {
    return unexpected("FailableFunction2");
  }

  return Itoa(num);
}

expected<int, const char*> FailOnOdd(int x) {
  if (x % 2) {
    return unexpected("odd");
  }
  return x;
}

expected<SmallString, const char*> ItoaFailOnNegative(int x) {
  if (x < 0) {
    return unexpected("negative");
  }
  return Itoa(x);
}

expected<char, const char*> GetSecondChar(std::string_view s) {
  if (s.size() < 2) {
    return unexpected("string too small");
  }
  return s[1];
}

expected<char, std::string_view> RecoverStringTooSmall(std::string_view err) {
  if (err == "string too small") {
    return kRecoverySentinel;
  }
  return unexpected(err);
}

int Decrement(int x) { return x - 1; }

template <class T, class E>
expected<void, E> Consume(const expected<T, E>& e) {
  return e.transform([](auto) {});
}

TEST(ExpectedTest, HoldIntValueSuccess) {
  auto x = FailableFunction1(false, 10);
  ASSERT_TRUE(x.has_value());
  EXPECT_EQ(x.value(), 10);
  EXPECT_EQ(*x, 10);
  EXPECT_EQ(x.value_or(33), 10);
  EXPECT_EQ(x.error_or("no error"), "no error");
}

TEST(ExpectedTest, HoldIntValueFail) {
  auto x = FailableFunction1(true, 10);
  ASSERT_FALSE(x.has_value());
  EXPECT_EQ(x.error(), "FailableFunction1");
  EXPECT_EQ(x.value_or(33), 33);
  EXPECT_EQ(x.error_or("no error"), "FailableFunction1");
}

TEST(ExpectedTest, HoldStringValueSuccess) {
  auto x = FailableFunction2(false, 42);
  ASSERT_TRUE(x.has_value());
  EXPECT_EQ(x.value(), "42");
  EXPECT_EQ(*x, "42");
  EXPECT_EQ(x.value_or("33"), "42");
  EXPECT_EQ(x.error_or("no error"), "no error");
}

TEST(ExpectedTest, HoldStringValueFail) {
  auto x = FailableFunction2(true, 42);
  ASSERT_FALSE(x.has_value());
  EXPECT_EQ(x.error(), "FailableFunction2");
  EXPECT_EQ(x.value_or("33"), "33");
  EXPECT_EQ(x.error_or("no error"), "FailableFunction2");
}

TEST(ExpectedTest, MonadicOperation) {
  auto f = [](expected<int, const char*> value) {
    return value.and_then(FailOnOdd)
        .transform(Decrement)
        .transform(Decrement)
        .and_then(ItoaFailOnNegative)
        .and_then(GetSecondChar);
  };
  EXPECT_EQ(f(26).value_or(0), '4');
  EXPECT_EQ(f(26).error_or("no error"), "no error");
  EXPECT_EQ(f(25).value_or(0), 0);
  EXPECT_EQ(f(25).error_or("no error"), "odd");
  EXPECT_EQ(f(0).value_or(0), 0);
  EXPECT_EQ(f(0).error_or("no error"), "negative");
  EXPECT_EQ(f(4).value_or(0), 0);
  EXPECT_EQ(f(4).error_or("no error"), "string too small");
  EXPECT_TRUE(Consume(f(26)).has_value());
  EXPECT_EQ(Consume(f(25)).error_or("no error"), "odd");
  EXPECT_EQ(Consume(f(0)).error_or("no error"), "negative");
  EXPECT_EQ(Consume(f(4)).error_or("no error"), "string too small");

  EXPECT_EQ(f(26).or_else(RecoverStringTooSmall).value_or(0), '4');
  EXPECT_EQ(f(26).or_else(RecoverStringTooSmall).error_or("no error"),
            "no error");
  EXPECT_EQ(f(25).or_else(RecoverStringTooSmall).value_or(0), 0);
  EXPECT_EQ(f(25).or_else(RecoverStringTooSmall).error_or("no error"), "odd");
  EXPECT_EQ(f(0).or_else(RecoverStringTooSmall).value_or(0), 0);
  EXPECT_EQ(f(0).or_else(RecoverStringTooSmall).error_or("no error"),
            "negative");
  EXPECT_EQ(f(4).or_else(RecoverStringTooSmall).value_or(0), kRecoverySentinel);
  EXPECT_EQ(f(4).or_else(RecoverStringTooSmall).error_or("no error"),
            "no error");
}

class ExpectedVoidTest : public ::testing::Test {
 protected:
  constexpr static expected<void, int> kSuccess{};
  constexpr static int kErrorValue = -5;
  constexpr static int kRecoverableErrorValue = 5;

  expected<void, int> Func1() {
    ++func1_invocations;
    if (func1_result.has_value()) {
      return unexpected(*func1_result);
    }
    return kSuccess;
  }

  expected<void, int> Func2() {
    ++func2_invocations;
    if (func2_result.has_value()) {
      return unexpected(*func2_result);
    }
    return kSuccess;
  }

  expected<void, int> Func3() {
    ++func3_invocations;
    if (func3_result.has_value()) {
      return unexpected(*func3_result);
    }
    return kSuccess;
  }

  expected<void, int> RecoverIfNotNegative(int err) {
    ++recover_if_not_negative_invocations;
    if (err < 0) {
      return unexpected(err);
    }
    return kSuccess;
  }

  void Func1Returns(std::optional<int> value) { func1_result = value; }
  void Func2Returns(std::optional<int> value) { func2_result = value; }
  void Func3Returns(std::optional<int> value) { func3_result = value; }

  size_t CountFunc1Invocations() { return func1_invocations; }
  size_t CountFunc2Invocations() { return func2_invocations; }
  size_t CountFunc3Invocations() { return func3_invocations; }
  size_t CountRecoverIfNotNegativeInvocations() {
    return recover_if_not_negative_invocations;
  }

  template <typename Return, typename... Args>
  auto Bind(Return (ExpectedVoidTest::*member_func)(Args...)) {
    return [this, member_func](Args... args) {
      return (this->*member_func)(args...);
    };
  }

  expected<void, int> RunSequence() {
    using Self = ExpectedVoidTest;
    return Func1()
        .and_then(Bind(&Self::Func2))
        .and_then(Bind(&Self::Func3))
        .or_else(Bind(&Self::RecoverIfNotNegative));
  }

 private:
  std::optional<int> func1_result = std::nullopt;
  std::optional<int> func2_result = std::nullopt;
  std::optional<int> func3_result = std::nullopt;

  size_t func1_invocations = 0;
  size_t func2_invocations = 0;
  size_t func3_invocations = 0;
  size_t recover_if_not_negative_invocations = 0;
};

TEST_F(ExpectedVoidTest, VoidAllSuccess) {
  EXPECT_TRUE(RunSequence().has_value());

  EXPECT_EQ(1u, CountFunc1Invocations());
  EXPECT_EQ(1u, CountFunc2Invocations());
  EXPECT_EQ(1u, CountFunc3Invocations());
  EXPECT_EQ(0u, CountRecoverIfNotNegativeInvocations());
}

TEST_F(ExpectedVoidTest, VoidOneFailureFirst) {
  Func1Returns(kErrorValue);

  auto result = RunSequence();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), kErrorValue);

  EXPECT_EQ(1u, CountFunc1Invocations());
  EXPECT_EQ(0u, CountFunc2Invocations());
  EXPECT_EQ(0u, CountFunc3Invocations());
  EXPECT_EQ(1u, CountRecoverIfNotNegativeInvocations());
}

TEST_F(ExpectedVoidTest, VoidOneFailureMiddle) {
  Func2Returns(kErrorValue);

  auto result = RunSequence();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), kErrorValue);

  EXPECT_EQ(1u, CountFunc1Invocations());
  EXPECT_EQ(1u, CountFunc2Invocations());
  EXPECT_EQ(0u, CountFunc3Invocations());
  EXPECT_EQ(1u, CountRecoverIfNotNegativeInvocations());
}

TEST_F(ExpectedVoidTest, VoidOneFailureLast) {
  Func3Returns(kErrorValue);

  auto result = RunSequence();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), kErrorValue);

  EXPECT_EQ(1u, CountFunc1Invocations());
  EXPECT_EQ(1u, CountFunc2Invocations());
  EXPECT_EQ(1u, CountFunc3Invocations());
  EXPECT_EQ(1u, CountRecoverIfNotNegativeInvocations());
}

TEST_F(ExpectedVoidTest, VoidOneFailureFirstRecovers) {
  Func1Returns(kRecoverableErrorValue);

  auto result = RunSequence();
  EXPECT_TRUE(result.has_value());

  EXPECT_EQ(1u, CountFunc1Invocations());
  EXPECT_EQ(0u, CountFunc2Invocations());
  EXPECT_EQ(0u, CountFunc3Invocations());
  EXPECT_EQ(1u, CountRecoverIfNotNegativeInvocations());
}

TEST_F(ExpectedVoidTest, VoidOneFailureMiddleRecovers) {
  Func2Returns(kRecoverableErrorValue);

  auto result = RunSequence();
  EXPECT_TRUE(result.has_value());

  EXPECT_EQ(1u, CountFunc1Invocations());
  EXPECT_EQ(1u, CountFunc2Invocations());
  EXPECT_EQ(0u, CountFunc3Invocations());
  EXPECT_EQ(1u, CountRecoverIfNotNegativeInvocations());
}

TEST_F(ExpectedVoidTest, VoidOneFailureLastRecovers) {
  Func3Returns(kRecoverableErrorValue);

  auto result = RunSequence();
  EXPECT_TRUE(result.has_value());

  EXPECT_EQ(1u, CountFunc1Invocations());
  EXPECT_EQ(1u, CountFunc2Invocations());
  EXPECT_EQ(1u, CountFunc3Invocations());
  EXPECT_EQ(1u, CountRecoverIfNotNegativeInvocations());
}

TEST_F(ExpectedVoidTest, VoidTransform) {
  auto result = expected<void, const char*>().transform([] { return 100; });
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 100);
  EXPECT_EQ(result, 100);
}

TEST_F(ExpectedVoidTest, VoidTransformError) {
  auto result = expected<void, int>(unexpect, 100).transform_error([](int x) {
    return x * 2;
  });
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), 200);
}

}  // namespace
}  // namespace pw
