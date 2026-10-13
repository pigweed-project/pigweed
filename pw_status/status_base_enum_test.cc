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

#include <tuple>
#include <type_traits>
#include <utility>

#include "pw_compilation_testing/negative_compilation.h"
#include "pw_result/result.h"
#include "pw_status/status_base.h"
#include "pw_status/try.h"
#include "pw_unit_test/constexpr.h"
#include "pw_unit_test/framework.h"
#include "status_test_enum.h"

namespace {

using PwEnumStatus = ::pw::StatusOf<::pw::status_test::GeneratedEnum::kOk>;

template <typename T, typename = void>
struct HasIsOk : std::false_type {};

template <typename T>
struct HasIsOk<T, std::void_t<decltype(std::declval<T>().IsOk())>>
    : std::true_type {};

class [[nodiscard]] SubclassPwEnumStatus final
    : public pw::StatusBase<SubclassPwEnumStatus,
                            ::pw::status_test::GeneratedEnum::kOk> {
 public:
  constexpr SubclassPwEnumStatus() = default;
  explicit constexpr SubclassPwEnumStatus(::pw::status_test::GeneratedEnum code)
      : StatusBase(code) {}
};

PW_CONSTEXPR_TEST(StatusBaseTest, PwEnumStatus, {
  static_assert(pw::internal::status_has_str_v<PwEnumStatus>);

  PwEnumStatus ok_status;
  PW_TEST_EXPECT_TRUE(ok_status.ok());
  PW_TEST_EXPECT_STREQ(ok_status.str(), "OK");
  PW_TEST_EXPECT_STREQ(pw::internal::StatusToCheckString(ok_status), "OK");
  PW_TEST_EXPECT_EQ(ok_status.code(), ::pw::status_test::GeneratedEnum::kOk);
  PW_TEST_EXPECT_EQ(ok_status, ::pw::status_test::GeneratedEnum::kOk);

  PwEnumStatus not_found(::pw::status_test::GeneratedEnum::kNotFound);
  PW_TEST_EXPECT_FALSE(not_found.ok());
  PW_TEST_EXPECT_STREQ(not_found.str(), "NOT_FOUND");
  PW_TEST_EXPECT_STREQ(pw::internal::StatusToCheckString(not_found),
                       "NOT_FOUND");
  PW_TEST_EXPECT_EQ(not_found.code(),
                    ::pw::status_test::GeneratedEnum::kNotFound);
  PW_TEST_EXPECT_EQ(not_found, ::pw::status_test::GeneratedEnum::kNotFound);

  PwEnumStatus perm(::pw::status_test::GeneratedEnum::kPermissionDenied);
  PW_TEST_EXPECT_FALSE(perm.ok());
  PW_TEST_EXPECT_STREQ(perm.str(), "PERMISSION_DENIED");
  PW_TEST_EXPECT_EQ(perm.code(),
                    ::pw::status_test::GeneratedEnum::kPermissionDenied);

  // Test Update
  ok_status.Update(not_found);
  PW_TEST_EXPECT_FALSE(ok_status.ok());
  PW_TEST_EXPECT_EQ(ok_status, ::pw::status_test::GeneratedEnum::kNotFound);

  // Test Subclass
  SubclassPwEnumStatus sub_ok;
  PW_TEST_EXPECT_TRUE(sub_ok.ok());
  PW_TEST_EXPECT_STREQ(sub_ok.str(), "OK");

  SubclassPwEnumStatus sub_err(
      ::pw::status_test::GeneratedEnum::kPermissionDenied);
  PW_TEST_EXPECT_FALSE(sub_err.ok());
  PW_TEST_EXPECT_STREQ(sub_err.str(), "PERMISSION_DENIED");
  PW_TEST_EXPECT_EQ(sub_err,
                    ::pw::status_test::GeneratedEnum::kPermissionDenied);

  // Test PW_TRY
  constexpr auto TryFunc = [](PwEnumStatus s) -> PwEnumStatus {
    PW_TRY(s);
    return PwEnumStatus(::pw::status_test::GeneratedEnum::kPermissionDenied);
  };
  PW_TEST_EXPECT_EQ(
      TryFunc(PwEnumStatus(::pw::status_test::GeneratedEnum::kOk)),
      ::pw::status_test::GeneratedEnum::kPermissionDenied);
  PW_TEST_EXPECT_EQ(
      TryFunc(PwEnumStatus(::pw::status_test::GeneratedEnum::kNotFound)),
      ::pw::status_test::GeneratedEnum::kNotFound);
});

PW_CONSTEXPR_TEST(StatusBaseTest, StatusEnum, {
  using ::pw::status_test::TestStatus;
  using ::pw::status_test::TestStatusCode;

  static_assert(std::is_enum_v<TestStatus::Code>);
  static_assert(std::is_same_v<TestStatus::Code, TestStatusCode>);
  static_assert(pw::internal::is_status_v<TestStatus>);
  static_assert(pw::internal::status_has_str_v<TestStatus>);

  TestStatus ok_status;
  PW_TEST_EXPECT_OK(ok_status);
  PW_TEST_ASSERT_OK(ok_status);
  PW_TEST_EXPECT_TRUE(ok_status.ok());
  PW_TEST_EXPECT_STREQ(ok_status.str(), "OK");
  PW_TEST_EXPECT_STREQ(pw::internal::StatusToCheckString(ok_status), "OK");
  PW_TEST_EXPECT_EQ(ok_status.code(), TestStatus::Code::kOk);
  PW_TEST_EXPECT_EQ(ok_status, TestStatus::Code::kOk);

  TestStatus not_found = TestStatus::NotFound();
  PW_TEST_EXPECT_FALSE(not_found.ok());
  PW_TEST_EXPECT_STREQ(not_found.str(), "NOT_FOUND");
  PW_TEST_EXPECT_STREQ(pw::internal::StatusToCheckString(not_found),
                       "NOT_FOUND");
  PW_TEST_EXPECT_EQ(not_found.code(), TestStatus::Code::kNotFound);
  PW_TEST_EXPECT_EQ(not_found, TestStatus::Code::kNotFound);
  PW_TEST_EXPECT_EQ(not_found, TestStatus::NotFound());

  TestStatus perm = TestStatus::PermissionDenied();
  PW_TEST_EXPECT_FALSE(perm.ok());
  PW_TEST_EXPECT_STREQ(perm.str(), "PERMISSION_DENIED");
  PW_TEST_EXPECT_EQ(perm.code(), TestStatus::Code::kPermissionDenied);
  PW_TEST_EXPECT_EQ(perm, TestStatus::PermissionDenied());

  // Test Is*() functions. IsOk() is not generated since it duplicates ok().
  static_assert(!HasIsOk<TestStatus>::value);
  PW_TEST_EXPECT_FALSE(ok_status.IsNotFound());
  PW_TEST_EXPECT_FALSE(ok_status.IsPermissionDenied());
  PW_TEST_EXPECT_TRUE(not_found.IsNotFound());
  PW_TEST_EXPECT_FALSE(not_found.IsPermissionDenied());
  PW_TEST_EXPECT_TRUE(perm.IsPermissionDenied());
  PW_TEST_EXPECT_FALSE(perm.IsNotFound());

  // Test Update
  ok_status.Update(not_found);
  PW_TEST_EXPECT_FALSE(ok_status.ok());
  PW_TEST_EXPECT_EQ(ok_status, TestStatus::NotFound());

  // Test PW_TRY
  constexpr auto TryFunc = [](TestStatus s) -> TestStatus {
    PW_TRY(s);
    return TestStatus::PermissionDenied();
  };
  PW_TEST_EXPECT_EQ(TryFunc(TestStatus()), TestStatus::PermissionDenied());
  PW_TEST_EXPECT_EQ(TryFunc(TestStatus::NotFound()), TestStatus::NotFound());

  // Test EnumTraits
  static_assert(pw::EnumTraits<TestStatus::Code>::kName == "TestStatusCode");
  static_assert(pw::EnumTraits<TestStatus::Code>::kFullyQualifiedName ==
                "::pw::status_test::TestStatusCode");
  PW_TEST_EXPECT_TRUE(
      pw::EnumTraits<TestStatus::Code>::IsValid(TestStatus::Code::kOk));
  PW_TEST_EXPECT_FALSE(pw::EnumTraits<TestStatus::Code>::IsValid(
      static_cast<TestStatus::Code>(99)));
});

PW_CONSTEXPR_TEST(StatusBaseTest, StatusEnumNonZeroOk, {
  using ::pw::status_test::NonZeroOkStatus;

  NonZeroOkStatus ready_status;
  PW_TEST_EXPECT_TRUE(ready_status.ok());
  PW_TEST_EXPECT_STREQ(ready_status.str(), "READY");
  PW_TEST_EXPECT_EQ(ready_status.code(), NonZeroOkStatus::Code::kReady);
  PW_TEST_EXPECT_EQ(ready_status, NonZeroOkStatus::Code::kReady);

  NonZeroOkStatus busy = NonZeroOkStatus::Busy();
  PW_TEST_EXPECT_FALSE(busy.ok());
  PW_TEST_EXPECT_STREQ(busy.str(), "BUSY");
  PW_TEST_EXPECT_EQ(busy.code(), NonZeroOkStatus::Code::kBusy);
  PW_TEST_EXPECT_EQ(busy, NonZeroOkStatus::Busy());

  NonZeroOkStatus err = NonZeroOkStatus::Error();
  PW_TEST_EXPECT_FALSE(err.ok());
  PW_TEST_EXPECT_STREQ(err.str(), "ERROR");
  PW_TEST_EXPECT_EQ(err.code(), NonZeroOkStatus::Code::kError);

  // Is*() functions are generated for OK codes not named OK.
  PW_TEST_EXPECT_TRUE(ready_status.IsReady());
  PW_TEST_EXPECT_FALSE(ready_status.IsBusy());
  PW_TEST_EXPECT_FALSE(ready_status.IsError());
  PW_TEST_EXPECT_TRUE(busy.IsBusy());
  PW_TEST_EXPECT_FALSE(busy.IsReady());
  PW_TEST_EXPECT_TRUE(err.IsError());

  ready_status.Update(err);
  PW_TEST_EXPECT_FALSE(ready_status.ok());
  PW_TEST_EXPECT_EQ(ready_status, NonZeroOkStatus::Error());
});

PW_CONSTEXPR_TEST(StatusBaseTest, StatusEnumExplicitConstruction, {
  using ::pw::status_test::TestStatus;

  static_assert(std::is_constructible_v<TestStatus, TestStatus::Code>);
  static_assert(!std::is_convertible_v<TestStatus::Code, TestStatus>);

  TestStatus status(TestStatus::Code::kNotFound);
  PW_TEST_EXPECT_EQ(status, TestStatus::NotFound());
  PW_TEST_EXPECT_EQ(TestStatus(TestStatus::Code::kOk), TestStatus());
});

constexpr pw::Result<int, pw::status_test::TestStatus> ParseInt(int value) {
  if (value < 0) {
    return pw::status_test::TestStatus::NotFound();
  }
  return value;
}

constexpr pw::Result<int, pw::status_test::TestStatus> DoubleInt(int value) {
  PW_TRY_ASSIGN(int parsed, ParseInt(value));
  return parsed * 2;
}

PW_CONSTEXPR_TEST(StatusBaseTest, StatusEnumResult, {
  using ::pw::status_test::TestStatus;

  constexpr pw::Result<int, TestStatus> ok_result = DoubleInt(21);
  PW_TEST_ASSERT_OK(ok_result.status());
  PW_TEST_EXPECT_EQ(*ok_result, 42);

  constexpr pw::Result<int, TestStatus> error = DoubleInt(-1);
  PW_TEST_EXPECT_FALSE(error.ok());
  PW_TEST_EXPECT_EQ(error.status(), TestStatus::NotFound());
  PW_TEST_EXPECT_STREQ(error.status().str(), "NOT_FOUND");
});

[[maybe_unused]] void NegativeCompilationTests() {
#if PW_NC_TEST(NodiscardStatusEnum)
  PW_NC_EXPECT("ignoring return.*value");
  auto fn = []() -> ::pw::status_test::TestStatus {
    return ::pw::status_test::TestStatus();
  };
  fn();
#elif PW_NC_TEST(StatusEnumNoOkFactory)
  PW_NC_EXPECT("Ok");
  std::ignore = ::pw::status_test::TestStatus::Ok();
#endif  // PW_NC_TEST
}

}  // namespace
