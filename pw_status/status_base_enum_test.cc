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

#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>

#include "pw_compilation_testing/negative_compilation.h"
#include "pw_result/result.h"
#include "pw_status/message.h"
#include "pw_status/status_base.h"
#include "pw_status/try.h"
#include "pw_unit_test/constexpr.h"
#include "pw_unit_test/framework.h"
#include "status_test_enum.h"

namespace {

using ::pw::internal::kStatusCodeBits;
using ::pw::internal::kStatusMessageType;
using ::pw::internal::StatusMessageType;
using ::pw::internal::StatusStorage;

constexpr bool kMessagesEnabled =
    kStatusMessageType != StatusMessageType::kNone;

using PwEnumStatus = ::pw::StatusOf<::pw::status_test::GeneratedEnum::kOk>;

// Enums registered with PW_ENUM have a known range. They support messages if
// their values are not negative and need at most 16 bits.
static_assert(kStatusCodeBits<::pw::status_test::GeneratedEnum> == 2);
static_assert(kStatusCodeBits<::pw::status_test::NonZeroOkStatusCode> == 2);
static_assert(kStatusCodeBits<::pw::status_test::UnscopedGeneratedError> == 5);
static_assert(kStatusCodeBits<::pw::status_test::SixteenBitGeneratedError> ==
              16);
static_assert(kStatusCodeBits<::pw::status_test::SeventeenBitGeneratedError> ==
              0);
static_assert(kStatusCodeBits<::pw::status_test::SignedGeneratedError> == 0);

// Statuses that do not support messages are only their code in every
// configuration.
static_assert(
    sizeof(
        ::pw::StatusOf<::pw::status_test::SeventeenBitGeneratedError::kOk>) ==
    sizeof(::pw::status_test::SeventeenBitGeneratedError));
static_assert(
    sizeof(::pw::StatusOf<::pw::status_test::SignedGeneratedError::kOk>) ==
    sizeof(::pw::status_test::SignedGeneratedError));
static_assert(sizeof(::pw::status_test::SeventeenBitGeneratedStatus) ==
              sizeof(::pw::status_test::SeventeenBitGeneratedError));
static_assert(sizeof(::pw::status_test::SignedGeneratedStatus) ==
              sizeof(::pw::status_test::SignedGeneratedError));

static_assert(sizeof(PwEnumStatus) ==
              (PW_STATUS_CFG_MESSAGES == PW_STATUS_CFG_MESSAGES_NONE
                   ? sizeof(::pw::status_test::GeneratedEnum)
               : PW_STATUS_CFG_MESSAGES == PW_STATUS_CFG_MESSAGES_TOKENIZED
                   ? sizeof(uint32_t)
                   : 2 * sizeof(const char*)));

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

TEST(StatusBaseTest, Message_PwEnum) {
  using ::pw::status_test::GeneratedEnum;

  const PwEnumStatus not_found(GeneratedEnum::kNotFound,
                               PW_STATUS_MSG("Missing key"));
  EXPECT_EQ(not_found.code(), GeneratedEnum::kNotFound);
  EXPECT_STREQ(not_found.str(), "NOT_FOUND");
  EXPECT_EQ(not_found.has_message(), kMessagesEnabled);
  EXPECT_EQ(not_found, PwEnumStatus(GeneratedEnum::kNotFound));

  const PwEnumStatus ok(GeneratedEnum::kOk, PW_STATUS_MSG("Ignored"));
  EXPECT_TRUE(ok.ok());
  EXPECT_FALSE(ok.has_message());

  // The message is carried through pw::Result.
  const ::pw::Result<int, PwEnumStatus> result(not_found);
  EXPECT_EQ(result.status().code(), GeneratedEnum::kNotFound);
  EXPECT_EQ(result.status().has_message(), kMessagesEnabled);
}

PW_CONSTEXPR_TEST(StatusBaseTest, StatusEnumWithoutMessageSupport, {
  using ::pw::status_test::SeventeenBitGeneratedStatus;
  using ::pw::status_test::SignedGeneratedStatus;

  SeventeenBitGeneratedStatus seventeen_bit =
      SeventeenBitGeneratedStatus::Last();
  PW_TEST_EXPECT_FALSE(seventeen_bit.ok());
  PW_TEST_EXPECT_TRUE(seventeen_bit.IsLast());
  PW_TEST_EXPECT_FALSE(seventeen_bit.has_message());

  SignedGeneratedStatus signed_status = SignedGeneratedStatus::Negative();
  PW_TEST_EXPECT_FALSE(signed_status.ok());
  PW_TEST_EXPECT_TRUE(signed_status.IsNegative());
  PW_TEST_EXPECT_FALSE(signed_status.has_message());
});

// GeneratedEnum has values 0 to 2, so its code uses the low 2 bits of the word.
PW_CONSTEXPR_TEST(StatusStorageTest, Token_GeneratedEnum, {
  using ::pw::status_test::GeneratedEnum;
  using Storage = StatusStorage<GeneratedEnum::kOk, StatusMessageType::kToken>;

  static_assert(sizeof(Storage) == sizeof(uint32_t));
  static_assert(Storage::kTokenMask == 0xFFFFFFFCu);

  Storage ok;
  PW_TEST_EXPECT_TRUE(ok.ok());
  PW_TEST_EXPECT_EQ(ok.code(), GeneratedEnum::kOk);
  PW_TEST_EXPECT_FALSE(ok.has_message());
  PW_TEST_EXPECT_EQ(ok.message(), 0u);

  Storage code_only(GeneratedEnum::kPermissionDenied);
  PW_TEST_EXPECT_FALSE(code_only.ok());
  PW_TEST_EXPECT_EQ(code_only.code(), GeneratedEnum::kPermissionDenied);
  PW_TEST_EXPECT_FALSE(code_only.has_message());

  Storage err(GeneratedEnum::kNotFound, 0xAABBCCDC);
  PW_TEST_EXPECT_FALSE(err.ok());
  PW_TEST_EXPECT_EQ(err.code(), GeneratedEnum::kNotFound);
  PW_TEST_EXPECT_TRUE(err.has_message());
  PW_TEST_EXPECT_EQ(err.message(), 0xAABBCCDCu);

  Storage ok_with_token(GeneratedEnum::kOk, 0xAABBCCDC);
  PW_TEST_EXPECT_TRUE(ok_with_token.ok());
  PW_TEST_EXPECT_FALSE(ok_with_token.has_message());
});

// The OK code does not have to be 0.
PW_CONSTEXPR_TEST(StatusStorageTest, Token_NonZeroOkCode, {
  using ::pw::status_test::NonZeroOkStatusCode;
  using Storage =
      StatusStorage<NonZeroOkStatusCode::kReady, StatusMessageType::kToken>;

  Storage ok;
  PW_TEST_EXPECT_TRUE(ok.ok());
  PW_TEST_EXPECT_EQ(ok.code(), NonZeroOkStatusCode::kReady);

  Storage busy(NonZeroOkStatusCode::kBusy, 0xAABBCCDC);
  PW_TEST_EXPECT_FALSE(busy.ok());
  PW_TEST_EXPECT_EQ(busy.code(), NonZeroOkStatusCode::kBusy);
  PW_TEST_EXPECT_EQ(busy.message(), 0xAABBCCDCu);

  Storage busy_code_only(NonZeroOkStatusCode::kBusy);
  PW_TEST_EXPECT_FALSE(busy_code_only.ok());
  PW_TEST_EXPECT_FALSE(busy_code_only.has_message());

  Storage ok_with_token(NonZeroOkStatusCode::kReady, 0xAABBCCDC);
  PW_TEST_EXPECT_TRUE(ok_with_token.ok());
  PW_TEST_EXPECT_FALSE(ok_with_token.has_message());
});

// Codes are stored as is, not as an offset from the smallest enumerator, so
// that the code's bits are the value that is tokenized for the enum.
// UnscopedGeneratedError's values are 10 and 20, which need 5 bits.
PW_CONSTEXPR_TEST(StatusStorageTest, Token_NonZeroMinimum, {
  using ::pw::status_test::kUnscopedFailed;
  using ::pw::status_test::kUnscopedOk;
  using Storage = StatusStorage<kUnscopedOk, StatusMessageType::kToken>;

  static_assert(Storage::kTokenMask == 0xFFFFFFE0u);

  Storage ok;
  PW_TEST_EXPECT_TRUE(ok.ok());
  PW_TEST_EXPECT_EQ(ok.code(), kUnscopedOk);
  PW_TEST_EXPECT_FALSE(ok.has_message());

  Storage err(kUnscopedFailed, 0xAABBCCC0);
  PW_TEST_EXPECT_FALSE(err.ok());
  PW_TEST_EXPECT_EQ(err.code(), kUnscopedFailed);
  PW_TEST_EXPECT_EQ(err.message(), 0xAABBCCC0u);
});

PW_CONSTEXPR_TEST(StatusStorageTest, Token_SixteenBitCode, {
  using ::pw::status_test::SixteenBitGeneratedError;
  using Storage =
      StatusStorage<SixteenBitGeneratedError::kOk, StatusMessageType::kToken>;

  static_assert(Storage::kTokenMask == 0xFFFF0000u);

  Storage err(SixteenBitGeneratedError::kLast, 0xFFFF0000);
  PW_TEST_EXPECT_FALSE(err.ok());
  PW_TEST_EXPECT_EQ(err.code(), SixteenBitGeneratedError::kLast);
  PW_TEST_EXPECT_EQ(err.message(), 0xFFFF0000u);
});

// Codes are not range checked or masked, so that a value that does not fit in
// the code's bits can never be mistaken for the OK code.
PW_CONSTEXPR_TEST(StatusStorageTest, Token_OutOfRangeCodeIsNeverOk, {
  using ::pw::status_test::GeneratedEnum;
  using Storage = StatusStorage<GeneratedEnum::kOk, StatusMessageType::kToken>;

  PW_TEST_EXPECT_FALSE(Storage(static_cast<GeneratedEnum>(4)).ok());
  PW_TEST_EXPECT_FALSE(Storage(static_cast<GeneratedEnum>(4), 0xFFFFFFFC).ok());
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
#elif PW_NC_TEST(StatusEnumRejectsSeventeenBitMessage)
  PW_NC_EXPECT("This status type cannot carry a message");
  std::ignore = ::pw::status_test::SeventeenBitGeneratedStatus::Last(
      PW_STATUS_MSG("Nope"));
#elif PW_NC_TEST(StatusEnumRejectsSignedMessage)
  PW_NC_EXPECT("This status type cannot carry a message");
  std::ignore =
      ::pw::status_test::SignedGeneratedStatus::Negative(PW_STATUS_MSG("Nope"));
#endif  // PW_NC_TEST
}

}  // namespace
