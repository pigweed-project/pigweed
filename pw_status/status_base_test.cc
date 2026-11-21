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

#include "pw_status/status_base.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <type_traits>
#include <utility>

#include "pw_compilation_testing/negative_compilation.h"
#include "pw_enum/traits.h"
#include "pw_status/message.h"
#include "pw_status/status.h"
#include "pw_status/try.h"
#include "pw_tokenizer/hash.h"
#include "pw_tokenizer/tokenize.h"
#include "pw_unit_test/constexpr.h"
#include "pw_unit_test/framework.h"

namespace custom_err {

enum class CustomError : uint8_t {
  kOk = 0,
  kFailed = 1,
  kTimeout = 2,
  kOther = 3,
};

constexpr const char* PwEnumToString(CustomError e) {
  switch (e) {
    case CustomError::kOk:
      return "Ok";
    case CustomError::kFailed:
      return "Failed";
    case CustomError::kTimeout:
      return "Timeout";
    case CustomError::kOther:
      return "Other";
  }
  return "Unknown";
}

enum UnscopedError {
  kUnscopedOk = 10,
  kUnscopedFailed = 20,
};

enum class SignedError : int16_t {
  kOk = 0,
  kNegative = -1,
};

}  // namespace custom_err

namespace {

using ::custom_err::CustomError;
using ::custom_err::kUnscopedFailed;
using ::custom_err::kUnscopedOk;
using ::custom_err::SignedError;
using ::custom_err::UnscopedError;
using ::pw::StatusBase;
using ::pw::StatusMessages;
using ::pw::StatusOf;
using ::pw::internal::kStatusCodeBits;
using ::pw::internal::kStatusMessageType;
using ::pw::internal::StatusMessageType;
using ::pw::internal::StatusStorage;

// Whether PW_STATUS_CFG_MESSAGES enables messages. The tests in this file pass
// either way.
constexpr bool kMessagesEnabled =
    kStatusMessageType != StatusMessageType::kNone;

// Hand-written StatusBase subclass. CustomError is not registered with PW_ENUM,
// so this status does not support messages.
class [[nodiscard]] TestStatus final
    : public StatusBase<TestStatus, CustomError::kOk> {
 public:
  constexpr TestStatus() noexcept = default;
  explicit constexpr TestStatus(CustomError code) noexcept : StatusBase(code) {}
};

using AliasStatus = StatusOf<CustomError::kOk>;

using UnscopedStatus = StatusOf<kUnscopedOk>;
using SignedStatus = StatusOf<SignedError::kOk>;

class [[nodiscard]] IntStatus final : public StatusBase<IntStatus, 200> {
 public:
  constexpr IntStatus() noexcept = default;
  explicit constexpr IntStatus(int code) noexcept : StatusBase(code) {}
};

// Statuses that support messages: their codes are enums that have
// pw::EnumTraits, as pw_Status does, and need at most 16 bits.
class [[nodiscard]] MessageStatus final
    : public StatusBase<MessageStatus, PW_STATUS_OK> {
 public:
  constexpr MessageStatus() noexcept = default;
  explicit constexpr MessageStatus(pw_Status code) noexcept
      : StatusBase(code) {}
  constexpr MessageStatus(pw_Status code, Message message) noexcept
      : StatusBase(code, message) {}
};

using AliasMessageStatus = StatusOf<PW_STATUS_OK>;

// Has a code that supports messages, but opts out of them.
using NoMessageStatus = StatusOf<PW_STATUS_OK, StatusMessages::kDisabled>;

static_assert(std::is_nothrow_default_constructible_v<AliasStatus>);
static_assert(std::is_nothrow_constructible_v<AliasStatus, CustomError>);
static_assert(std::is_nothrow_default_constructible_v<AliasMessageStatus>);
static_assert(std::is_nothrow_constructible_v<AliasMessageStatus, pw_Status>);
static_assert(noexcept(std::declval<const AliasMessageStatus&>().ok()));
static_assert(noexcept(std::declval<const AliasMessageStatus&>().code()));
static_assert(
    noexcept(std::declval<const AliasMessageStatus&>().has_message()));
static_assert(noexcept(std::declval<const AliasMessageStatus&>().message()));

static_assert(std::is_same_v<TestStatus::Code, CustomError>);
static_assert(std::is_same_v<AliasStatus::Code, CustomError>);
static_assert(std::is_same_v<IntStatus::Code, int>);

// The bits a code needs to share a word with a message token; 0 if it cannot.
static_assert(kStatusCodeBits<pw_Status> == 5);
static_assert(kStatusCodeBits<CustomError> == 0, "No pw::EnumTraits");
static_assert(kStatusCodeBits<UnscopedError> == 0, "No pw::EnumTraits");
static_assert(kStatusCodeBits<SignedError> == 0, "No pw::EnumTraits");
static_assert(kStatusCodeBits<int> == 0, "Not an enum");

// Statuses that do not support messages are only their code in every
// configuration.
static_assert(sizeof(NoMessageStatus) == sizeof(pw_Status));
static_assert(sizeof(TestStatus) == sizeof(CustomError));
static_assert(sizeof(AliasStatus) == sizeof(CustomError));
static_assert(sizeof(SignedStatus) == sizeof(SignedError));
static_assert(sizeof(IntStatus) == sizeof(int));

// Without messages, every status is only its code. Tokenized messages share one
// 32-bit word with the code; string literal messages store a pointer next to
// the code.
constexpr size_t kExpectedMessageStatusSize =
    PW_STATUS_CFG_MESSAGES == PW_STATUS_CFG_MESSAGES_NONE ? sizeof(pw_Status)
    : PW_STATUS_CFG_MESSAGES == PW_STATUS_CFG_MESSAGES_TOKENIZED
        ? sizeof(uint32_t)
        : 2 * sizeof(const char*);
static_assert(sizeof(pw::Status) == kExpectedMessageStatusSize);
static_assert(sizeof(MessageStatus) == kExpectedMessageStatusSize);

static_assert(
    std::is_base_of_v<StatusBase<pw::Status, PW_STATUS_OK>, pw::Status>);
static_assert(std::is_same_v<pw::Status::Code, pw_Status>);

PW_CONSTEXPR_TEST(StatusBaseTest, StatusInheritance, {
  pw::Status ok_status;
  PW_TEST_EXPECT_TRUE(ok_status.ok());
  PW_TEST_EXPECT_EQ(ok_status, pw::OkStatus());

  pw::Status not_found = pw::Status::NotFound();
  PW_TEST_EXPECT_FALSE(not_found.ok());
  PW_TEST_EXPECT_TRUE(not_found.IsNotFound());
  PW_TEST_EXPECT_FALSE(not_found.IsCancelled());
  PW_TEST_EXPECT_EQ(not_found.code(), PW_STATUS_NOT_FOUND);
});

// StatusBase cannot be directly instantiated.
static_assert(
    !std::is_default_constructible_v<StatusBase<TestStatus, CustomError::kOk>>);
static_assert(!std::is_constructible_v<StatusBase<TestStatus, CustomError::kOk>,
                                       CustomError>);
static_assert(
    !std::is_copy_constructible_v<StatusBase<TestStatus, CustomError::kOk>>);
static_assert(
    !std::is_move_constructible_v<StatusBase<TestStatus, CustomError::kOk>>);

// Derived status types require explicit construction from the underlying code.
static_assert(!std::is_convertible_v<CustomError, TestStatus>);
static_assert(std::is_constructible_v<TestStatus, CustomError>);
static_assert(!std::is_convertible_v<CustomError, AliasStatus>);
static_assert(std::is_constructible_v<AliasStatus, CustomError>);
static_assert(!std::is_convertible_v<int, IntStatus>);
static_assert(std::is_constructible_v<IntStatus, int>);

// Different status types cannot be implicitly converted to each other.
static_assert(!std::is_convertible_v<AliasStatus, TestStatus>);
static_assert(!std::is_convertible_v<TestStatus, AliasStatus>);
static_assert(!std::is_convertible_v<pw::Status, TestStatus>);
static_assert(!std::is_convertible_v<TestStatus, pw::Status>);

PW_CONSTEXPR_TEST(StatusBaseTest, DefaultConstruction, {
  TestStatus status;
  PW_TEST_EXPECT_TRUE(status.ok());
  PW_TEST_EXPECT_EQ(status.code(), CustomError::kOk);
});

PW_CONSTEXPR_TEST(StatusBaseTest, ConstructWithCode, {
  TestStatus ok_status(CustomError::kOk);
  PW_TEST_EXPECT_TRUE(ok_status.ok());
  PW_TEST_EXPECT_EQ(ok_status.code(), CustomError::kOk);

  TestStatus err_status(CustomError::kFailed);
  PW_TEST_EXPECT_FALSE(err_status.ok());
  PW_TEST_EXPECT_EQ(err_status.code(), CustomError::kFailed);
});

PW_CONSTEXPR_TEST(StatusBaseTest, ExplicitConstruction, {
  constexpr auto MakeStatus = [](CustomError e) { return TestStatus(e); };
  PW_TEST_EXPECT_TRUE(MakeStatus(CustomError::kOk).ok());
  PW_TEST_EXPECT_EQ(MakeStatus(CustomError::kTimeout).code(),
                    CustomError::kTimeout);
});

PW_CONSTEXPR_TEST(StatusBaseTest, CopyAndMoveAssignment, {
  TestStatus s1(CustomError::kFailed);
  TestStatus s2;
  s2 = s1;
  PW_TEST_EXPECT_EQ(s2.code(), CustomError::kFailed);

  TestStatus s3;
  s3 = std::move(s1);
  PW_TEST_EXPECT_EQ(s3.code(), CustomError::kFailed);

  s2 = TestStatus(CustomError::kOk);
  PW_TEST_EXPECT_TRUE(s2.ok());
});

PW_CONSTEXPR_TEST(StatusBaseTest, Update, {
  TestStatus status;
  status.Update(TestStatus(CustomError::kFailed));
  PW_TEST_EXPECT_EQ(status.code(), CustomError::kFailed);

  // Subsequent updates should not overwrite the first error.
  status.Update(TestStatus(CustomError::kTimeout));
  PW_TEST_EXPECT_EQ(status.code(), CustomError::kFailed);

  status.Update(TestStatus(CustomError::kOk));
  PW_TEST_EXPECT_EQ(status.code(), CustomError::kFailed);

  // Updating OK with OK remains OK.
  TestStatus ok_status;
  ok_status.Update(TestStatus(CustomError::kOk));
  PW_TEST_EXPECT_TRUE(ok_status.ok());
});

class [[nodiscard]] StatusWithDetail final
    : public StatusBase<StatusWithDetail, CustomError::kOk> {
 public:
  constexpr StatusWithDetail() = default;
  explicit constexpr StatusWithDetail(CustomError code, uint8_t detail = 0)
      : StatusBase(code), detail_(detail) {}

  [[nodiscard]] constexpr uint8_t detail() const { return detail_; }

 private:
  uint8_t detail_ = 0;
};

PW_CONSTEXPR_TEST(StatusBaseTest, UpdatePreservesDerivedState, {
  StatusWithDetail status;
  status.Update(StatusWithDetail(CustomError::kFailed, 42));
  PW_TEST_EXPECT_EQ(status.code(), CustomError::kFailed);
  PW_TEST_EXPECT_EQ(status.detail(), 42u);

  status.Update(StatusWithDetail(CustomError::kTimeout, 99));
  PW_TEST_EXPECT_EQ(status.code(), CustomError::kFailed);
  PW_TEST_EXPECT_EQ(status.detail(), 42u);
});

PW_CONSTEXPR_TEST(StatusBaseTest, Equality_StatusStatus, {
  PW_TEST_EXPECT_EQ(TestStatus(CustomError::kOk), TestStatus(CustomError::kOk));
  PW_TEST_EXPECT_EQ(TestStatus(CustomError::kFailed),
                    TestStatus(CustomError::kFailed));
  PW_TEST_EXPECT_NE(TestStatus(CustomError::kOk),
                    TestStatus(CustomError::kFailed));
  PW_TEST_EXPECT_NE(TestStatus(CustomError::kFailed),
                    TestStatus(CustomError::kTimeout));
});

PW_CONSTEXPR_TEST(StatusBaseTest, Equality_StatusCode, {
  TestStatus ok_status(CustomError::kOk);
  TestStatus err_status(CustomError::kFailed);

  PW_TEST_EXPECT_EQ(ok_status, CustomError::kOk);
  PW_TEST_EXPECT_EQ(CustomError::kOk, ok_status);
  PW_TEST_EXPECT_NE(ok_status, CustomError::kFailed);
  PW_TEST_EXPECT_NE(CustomError::kFailed, ok_status);

  PW_TEST_EXPECT_EQ(err_status, CustomError::kFailed);
  PW_TEST_EXPECT_EQ(CustomError::kFailed, err_status);
  PW_TEST_EXPECT_NE(err_status, CustomError::kOk);
  PW_TEST_EXPECT_NE(CustomError::kOk, err_status);
});

PW_CONSTEXPR_TEST(StatusBaseTest, IgnoreError, {
  TestStatus(CustomError::kFailed).IgnoreError();
  AliasStatus(CustomError::kFailed).IgnoreError();
});

PW_CONSTEXPR_TEST(StatusBaseTest, PwTry, {
  constexpr auto TryFunc = [](TestStatus s) -> TestStatus {
    PW_TRY(s);
    return TestStatus(CustomError::kOther);
  };

  PW_TEST_EXPECT_EQ(TryFunc(TestStatus(CustomError::kOk)), CustomError::kOther);
  PW_TEST_EXPECT_EQ(TryFunc(TestStatus(CustomError::kFailed)),
                    CustomError::kFailed);
  PW_TEST_EXPECT_EQ(TryFunc(TestStatus(CustomError::kTimeout)),
                    CustomError::kTimeout);
});

PW_CONSTEXPR_TEST(StatusBaseTest, ConvertToStatus, {
  static_assert(
      std::is_same_v<decltype(pw::internal::ConvertToStatus(TestStatus())),
                     TestStatus>);
  static_assert(
      std::is_same_v<decltype(pw::internal::ConvertToStatus(pw::OkStatus())),
                     pw::Status>);
  static_assert(
      std::is_same_v<decltype(pw::internal::ConvertToStatus(PW_STATUS_OK)),
                     pw::Status>);
  PW_TEST_EXPECT_EQ(pw::internal::ConvertToStatus(TestStatus()),
                    CustomError::kOk);
  PW_TEST_EXPECT_EQ(pw::internal::ConvertToStatus(pw::OkStatus()),
                    pw::OkStatus());
  PW_TEST_EXPECT_EQ(pw::internal::ConvertToStatus(PW_STATUS_OK),
                    pw::OkStatus());
});

PW_CONSTEXPR_TEST(StatusBaseTest, StatusOf_Basic, {
  AliasStatus status;
  PW_TEST_EXPECT_TRUE(status.ok());
  PW_TEST_EXPECT_EQ(status.code(), CustomError::kOk);

  AliasStatus err(CustomError::kTimeout);
  PW_TEST_EXPECT_FALSE(err.ok());
  PW_TEST_EXPECT_EQ(err, CustomError::kTimeout);
  PW_TEST_EXPECT_EQ(CustomError::kTimeout, err);

  err.Update(AliasStatus(CustomError::kFailed));
  PW_TEST_EXPECT_EQ(err, CustomError::kTimeout);
});

PW_CONSTEXPR_TEST(StatusBaseTest, StatusOf_PwTry, {
  constexpr auto TryFunc = [](AliasStatus s) -> AliasStatus {
    PW_TRY(s);
    return AliasStatus(CustomError::kOther);
  };

  PW_TEST_EXPECT_EQ(TryFunc(AliasStatus(CustomError::kOk)),
                    CustomError::kOther);
  PW_TEST_EXPECT_EQ(TryFunc(AliasStatus(CustomError::kFailed)),
                    CustomError::kFailed);
});

PW_CONSTEXPR_TEST(StatusBaseTest, CodeAlias, {
  AliasStatus m(AliasStatus::Code::kTimeout);
  PW_TEST_EXPECT_EQ(m, AliasStatus::Code::kTimeout);
  PW_TEST_EXPECT_EQ(m.code(), CustomError::kTimeout);

  TestStatus t(TestStatus::Code::kFailed);
  PW_TEST_EXPECT_EQ(t, TestStatus::Code::kFailed);
});

PW_CONSTEXPR_TEST(StatusBaseTest, UnscopedEnum, {
  UnscopedStatus status;
  PW_TEST_EXPECT_TRUE(status.ok());
  PW_TEST_EXPECT_EQ(status, kUnscopedOk);

  status = UnscopedStatus(kUnscopedFailed);
  PW_TEST_EXPECT_FALSE(status.ok());
  PW_TEST_EXPECT_EQ(status, kUnscopedFailed);
});

PW_CONSTEXPR_TEST(StatusBaseTest, SignedEnum, {
  SignedStatus status(SignedError::kNegative);
  PW_TEST_EXPECT_FALSE(status.ok());
  PW_TEST_EXPECT_EQ(status.code(), SignedError::kNegative);
});

PW_CONSTEXPR_TEST(StatusBaseTest, NonEnumType_Int, {
  IntStatus status;
  PW_TEST_EXPECT_TRUE(status.ok());
  PW_TEST_EXPECT_EQ(status.code(), 200);
  PW_TEST_EXPECT_EQ(status, 200);
  PW_TEST_EXPECT_EQ(200, status);

  IntStatus not_found(404);
  PW_TEST_EXPECT_FALSE(not_found.ok());
  PW_TEST_EXPECT_EQ(not_found.code(), 404);
  PW_TEST_EXPECT_EQ(not_found, 404);
  PW_TEST_EXPECT_NE(not_found, 200);
  PW_TEST_EXPECT_NE(not_found, status);

  status.Update(not_found);
  PW_TEST_EXPECT_FALSE(status.ok());
  PW_TEST_EXPECT_EQ(status, 404);

  // Subsequent updates do not overwrite the first error.
  status.Update(IntStatus(500));
  PW_TEST_EXPECT_EQ(status, 404);
});

// DOCSTAG: [pw_status-status_base-custom_enum]
enum class DriverError : uint8_t {
  kOk,
  kBusFault,
  kTimeout,
};

using DriverStatus = pw::StatusOf<DriverError::kOk>;

constexpr DriverStatus WriteByte(uint8_t byte) {
  if (byte == 0xFF) {
    return DriverStatus(DriverError::kBusFault);
  }
  return DriverStatus();
}

// Propagate custom errors using PW_TRY.
constexpr DriverStatus SendPacket(uint8_t header, uint8_t payload) {
  PW_TRY(WriteByte(header));
  PW_TRY(WriteByte(payload));
  return DriverStatus();
}
// DOCSTAG: [pw_status-status_base-custom_enum]

constexpr bool HandlePacket() {
  // DOCSTAG: [pw_status-status_base-caller]
  DriverStatus status = SendPacket(0x01, 0x02);
  if (status.ok()) {
    // Packet sent successfully.
  } else if (status == DriverError::kBusFault) {
    // Handle bus fault.
  }
  // DOCSTAG: [pw_status-status_base-caller]
  return status.ok();
}

// DOCSTAG: [pw_status-status_base-non_enum]
// StatusBase can also be used with non-enum types such as integers.
class [[nodiscard]] HttpStatus final : public pw::StatusBase<HttpStatus, 200> {
 public:
  constexpr HttpStatus() = default;
  explicit constexpr HttpStatus(int code) : StatusBase(code) {}
};

constexpr HttpStatus FetchResource(bool resource_found) {
  if (!resource_found) {
    return HttpStatus(404);  // Explicit construction for non-OK codes
  }
  return HttpStatus();  // Defaults to 200 OK
}
// DOCSTAG: [pw_status-status_base-non_enum]

constexpr bool HandleRequest() {
  // DOCSTAG: [pw_status-status_base-non_enum_caller]
  HttpStatus status = FetchResource(true);
  if (status.ok()) {
    // Request succeeded (200).
  } else {
    // Handle HTTP error code (e.g. status.code() == 404).
  }
  // DOCSTAG: [pw_status-status_base-non_enum_caller]
  return status.ok();
}

PW_CONSTEXPR_TEST(StatusBaseTest, ExampleCustomEnum, {
  DriverStatus ok_status = SendPacket(0x01, 0x02);
  PW_TEST_EXPECT_TRUE(ok_status.ok());
  PW_TEST_EXPECT_TRUE(HandlePacket());

  DriverStatus fail_status = SendPacket(0xFF, 0x02);
  PW_TEST_EXPECT_FALSE(fail_status.ok());
  PW_TEST_EXPECT_EQ(fail_status, DriverError::kBusFault);
  PW_TEST_EXPECT_EQ(fail_status.code(), DriverError::kBusFault);
});

// DOCSTAG: [pw_status-status_base-subclass]
enum class SensorError : uint8_t {
  kOk,
  kBusy,
  kCalibrationFailed,
};

class [[nodiscard]] SensorStatus final
    : public pw::StatusBase<SensorStatus, SensorError::kOk> {
 public:
  constexpr SensorStatus() = default;
  explicit constexpr SensorStatus(Code code) : StatusBase(code) {}

  // Subclasses may add domain-specific helpers.
  [[nodiscard]] constexpr bool IsRetryable() const {
    return code() == Code::kBusy;
  }
};
// DOCSTAG: [pw_status-status_base-subclass]

PW_CONSTEXPR_TEST(StatusBaseTest, ExampleSubclass, {
  SensorStatus ok_status;
  PW_TEST_EXPECT_TRUE(ok_status.ok());
  PW_TEST_EXPECT_FALSE(ok_status.IsRetryable());

  SensorStatus busy(SensorError::kBusy);
  PW_TEST_EXPECT_FALSE(busy.ok());
  PW_TEST_EXPECT_TRUE(busy.IsRetryable());
  PW_TEST_EXPECT_FALSE(
      SensorStatus(SensorError::kCalibrationFailed).IsRetryable());
});

#if defined(__cpp_using_enum)
// C++20 subclass pattern
class [[nodiscard]] Cxx20DriverStatus final
    : public pw::StatusBase<Cxx20DriverStatus, DriverError::kOk> {
 public:
  constexpr Cxx20DriverStatus() = default;
  explicit constexpr Cxx20DriverStatus(DriverError code) : StatusBase(code) {}
  using enum DriverError;
};

constexpr Cxx20DriverStatus Cxx20WriteByte(uint8_t byte) {
  if (byte == 0xFF) {
    return Cxx20DriverStatus(Cxx20DriverStatus::kBusFault);
  }
  return Cxx20DriverStatus();
}

constexpr Cxx20DriverStatus Cxx20SendPacket(uint8_t header, uint8_t payload) {
  PW_TRY(Cxx20WriteByte(header));
  PW_TRY(Cxx20WriteByte(payload));
  return Cxx20DriverStatus();
}

PW_CONSTEXPR_TEST(StatusBaseTest, Cxx20SubclassPattern, {
  Cxx20DriverStatus ok_status = Cxx20SendPacket(0x01, 0x02);
  PW_TEST_EXPECT_TRUE(ok_status.ok());

  Cxx20DriverStatus fail_status = Cxx20SendPacket(0xFF, 0x02);
  PW_TEST_EXPECT_FALSE(fail_status.ok());
  PW_TEST_EXPECT_EQ(fail_status, Cxx20DriverStatus::kBusFault);
  PW_TEST_EXPECT_EQ(fail_status.code(), DriverError::kBusFault);
});
#endif  // defined(__cpp_using_enum)

PW_CONSTEXPR_TEST(StatusBaseTest, ExampleNonEnum, {
  HttpStatus ok_status = FetchResource(true);
  PW_TEST_EXPECT_TRUE(ok_status.ok());
  PW_TEST_EXPECT_EQ(ok_status.code(), 200);
  PW_TEST_EXPECT_TRUE(HandleRequest());

  HttpStatus err_status = FetchResource(false);
  PW_TEST_EXPECT_FALSE(err_status.ok());
  PW_TEST_EXPECT_EQ(err_status.code(), 404);
  PW_TEST_EXPECT_EQ(err_status, 404);
});

PW_CONSTEXPR_TEST(StatusBaseTest, Str, {
  TestStatus ok_status(CustomError::kOk);
  PW_TEST_EXPECT_STREQ(ok_status.str(), "Ok");

  TestStatus failed_status(CustomError::kFailed);
  PW_TEST_EXPECT_STREQ(failed_status.str(), "Failed");

  AliasStatus alias_ok;
  PW_TEST_EXPECT_STREQ(alias_ok.str(), "Ok");
  AliasStatus alias_fail(CustomError::kFailed);
  PW_TEST_EXPECT_STREQ(alias_fail.str(), "Failed");
});

// DOCSTAG: [pw_status-status_base-message]
pw::Status ReadSector(int sector) {
  if (sector < 0) {
    return pw::Status::InvalidArgument(
        PW_STATUS_MSG("Sector cannot be negative"));
  }
  return pw::OkStatus();
}
// DOCSTAG: [pw_status-status_base-message]

TEST(StatusBaseTest, ExampleMessage) {
  const pw::Status invalid = ReadSector(-1);
  EXPECT_TRUE(invalid.IsInvalidArgument());
  EXPECT_EQ(invalid.has_message(), kMessagesEnabled);

  const pw::Status ok = ReadSector(0);
  PW_TEST_EXPECT_OK(ok);
  EXPECT_FALSE(ok.has_message());
}

TEST(StatusBaseTest, Message_NoneByDefault) {
  const MessageStatus ok;
  EXPECT_FALSE(ok.has_message());
  EXPECT_TRUE(ok.message().empty());

  const MessageStatus failed(PW_STATUS_INTERNAL);
  EXPECT_FALSE(failed.has_message());
  EXPECT_TRUE(failed.message().empty());

  EXPECT_TRUE(MessageStatus::Message().empty());

  // Statuses that do not support messages have the same API.
  const TestStatus unsupported(CustomError::kFailed);
  EXPECT_FALSE(unsupported.has_message());
  EXPECT_TRUE(unsupported.message().empty());
}

TEST(StatusBaseTest, Message_StoredIfEnabled) {
  const MessageStatus failed(PW_STATUS_DATA_LOSS,
                             PW_STATUS_MSG("Disk read failure"));
  EXPECT_FALSE(failed.ok());
  EXPECT_EQ(failed.code(), PW_STATUS_DATA_LOSS);
  EXPECT_STREQ(failed.str(), "DATA_LOSS");
  EXPECT_EQ(failed.has_message(), kMessagesEnabled);
  EXPECT_EQ(failed.message().empty(), !kMessagesEnabled);

  const AliasMessageStatus timeout(PW_STATUS_DEADLINE_EXCEEDED,
                                   PW_STATUS_MSG("No response"));
  EXPECT_EQ(timeout.code(), PW_STATUS_DEADLINE_EXCEEDED);
  EXPECT_EQ(timeout.has_message(), kMessagesEnabled);
}

TEST(StatusBaseTest, Message_EveryCodeKeepsItsValue) {
  for (pw_Status code : pw::EnumTraits<pw_Status>::kValues) {
    const MessageStatus status(code, PW_STATUS_MSG("Same message"));
    EXPECT_EQ(status.code(), code);
    EXPECT_EQ(status.ok(), code == PW_STATUS_OK);
    EXPECT_EQ(status.has_message(), kMessagesEnabled && code != PW_STATUS_OK);
  }
}

// A message with no text is still a message. Only a status that was not given
// a message has none.
TEST(StatusBaseTest, Message_EmptyStringIsAMessage) {
  const MessageStatus failed(PW_STATUS_INTERNAL, PW_STATUS_MSG(""));
  EXPECT_EQ(failed.code(), PW_STATUS_INTERNAL);
  EXPECT_EQ(failed.has_message(), kMessagesEnabled);
  EXPECT_EQ(failed.message().empty(), !kMessagesEnabled);
}

TEST(StatusBaseTest, Message_DroppedForOk) {
  const MessageStatus ok(PW_STATUS_OK, PW_STATUS_MSG("Ignored"));
  EXPECT_TRUE(ok.ok());
  EXPECT_FALSE(ok.has_message());
  EXPECT_TRUE(ok.message().empty());
}

TEST(StatusBaseTest, Message_IgnoredByEquality) {
  const MessageStatus with_message(PW_STATUS_INTERNAL,
                                   PW_STATUS_MSG("Disk read failure"));
  EXPECT_EQ(with_message, MessageStatus(PW_STATUS_INTERNAL));
  EXPECT_EQ(with_message, PW_STATUS_INTERNAL);
  EXPECT_EQ(with_message,
            MessageStatus(PW_STATUS_INTERNAL, PW_STATUS_MSG("Other text")));
  EXPECT_NE(with_message, MessageStatus(PW_STATUS_UNKNOWN));
}

TEST(StatusBaseTest, Message_KeptByUpdate) {
  MessageStatus status;
  status.Update(
      MessageStatus(PW_STATUS_INTERNAL, PW_STATUS_MSG("First error")));
  EXPECT_EQ(status.code(), PW_STATUS_INTERNAL);
  EXPECT_EQ(status.has_message(), kMessagesEnabled);

  // Later errors do not replace the first error or its message.
  status.Update(MessageStatus(PW_STATUS_UNKNOWN));
  EXPECT_EQ(status.code(), PW_STATUS_INTERNAL);
  EXPECT_EQ(status.has_message(), kMessagesEnabled);
}

TEST(StatusBaseTest, Message_KeptByTry) {
  auto fn = []() -> AliasMessageStatus {
    PW_TRY(AliasMessageStatus(PW_STATUS_INTERNAL, PW_STATUS_MSG("Inner")));
    return AliasMessageStatus();
  };
  const AliasMessageStatus result = fn();
  EXPECT_EQ(result.code(), PW_STATUS_INTERNAL);
  EXPECT_EQ(result.has_message(), kMessagesEnabled);
}

TEST(StatusBaseTest, Message_MovesToAnotherCode) {
  const MessageStatus failed(PW_STATUS_INTERNAL,
                             PW_STATUS_MSG("Disk read failure"));
  const MessageStatus unavailable(PW_STATUS_UNAVAILABLE, failed.message());
  EXPECT_EQ(unavailable.code(), PW_STATUS_UNAVAILABLE);
  EXPECT_EQ(unavailable.has_message(), kMessagesEnabled);

  const MessageStatus no_message(PW_STATUS_UNAVAILABLE,
                                 MessageStatus().message());
  EXPECT_FALSE(no_message.has_message());
}

TEST(StatusBaseTest, Message_PwStatus) {
  struct Case {
    pw::Status (*factory)(pw::Status::Message);
    pw_Status code;
  };
  constexpr Case kCases[] = {
      {&pw::Status::Cancelled, PW_STATUS_CANCELLED},
      {&pw::Status::Unknown, PW_STATUS_UNKNOWN},
      {&pw::Status::InvalidArgument, PW_STATUS_INVALID_ARGUMENT},
      {&pw::Status::DeadlineExceeded, PW_STATUS_DEADLINE_EXCEEDED},
      {&pw::Status::NotFound, PW_STATUS_NOT_FOUND},
      {&pw::Status::AlreadyExists, PW_STATUS_ALREADY_EXISTS},
      {&pw::Status::PermissionDenied, PW_STATUS_PERMISSION_DENIED},
      {&pw::Status::ResourceExhausted, PW_STATUS_RESOURCE_EXHAUSTED},
      {&pw::Status::FailedPrecondition, PW_STATUS_FAILED_PRECONDITION},
      {&pw::Status::Aborted, PW_STATUS_ABORTED},
      {&pw::Status::OutOfRange, PW_STATUS_OUT_OF_RANGE},
      {&pw::Status::Unimplemented, PW_STATUS_UNIMPLEMENTED},
      {&pw::Status::Internal, PW_STATUS_INTERNAL},
      {&pw::Status::Unavailable, PW_STATUS_UNAVAILABLE},
      {&pw::Status::DataLoss, PW_STATUS_DATA_LOSS},
      {&pw::Status::Unauthenticated, PW_STATUS_UNAUTHENTICATED},
  };
  static_assert(std::size(kCases) + 1 ==
                pw::EnumTraits<pw_Status>::kDistinctValueCount);

  for (const Case& test : kCases) {
    const pw::Status status = test.factory(PW_STATUS_MSG("Detail"));
    EXPECT_EQ(status.code(), test.code);
    EXPECT_EQ(status.has_message(), kMessagesEnabled);
    EXPECT_EQ(status, pw::Status(test.code));
  }

  const pw::Status not_found =
      pw::Status::NotFound(PW_STATUS_MSG("No such key"));
  EXPECT_TRUE(not_found.IsNotFound());
  EXPECT_STREQ(not_found.str(), "NOT_FOUND");

  const pw::Status internal(PW_STATUS_INTERNAL, not_found.message());
  EXPECT_TRUE(internal.IsInternal());
  EXPECT_EQ(internal.has_message(), kMessagesEnabled);

  const pw::Status ok(PW_STATUS_OK, PW_STATUS_MSG("Ignored"));
  EXPECT_TRUE(ok.ok());
  EXPECT_FALSE(ok.has_message());

  EXPECT_FALSE(pw::Status::DataLoss().has_message());
}

TEST(StatusBaseTest, Message_Noexcept) {
  auto message = PW_STATUS_MSG("missing");
  static_assert(noexcept(MessageStatus(PW_STATUS_NOT_FOUND, message)));
  static_assert(noexcept(AliasMessageStatus(PW_STATUS_NOT_FOUND, message)));
  static_assert(noexcept(pw::Status(PW_STATUS_NOT_FOUND, message)));
  static_assert(noexcept(pw::Status::NotFound(message)));
}

#if defined(__cpp_constexpr) && __cpp_constexpr >= 202211L

// A status with a message can be created in a constant expression in C++23.
constexpr AliasMessageStatus kConstantWithMessage(PW_STATUS_INTERNAL,
                                                  PW_STATUS_MSG("Constant"));
static_assert(kConstantWithMessage.code() == PW_STATUS_INTERNAL);
static_assert(kConstantWithMessage.has_message() == kMessagesEnabled);

#else
#if PW_NC_TEST(MessageNotConstexprBeforeCpp23)
PW_NC_EXPECT("constant expression|non-'?constexpr");
constexpr AliasMessageStatus kNotConstexpr(PW_STATUS_INTERNAL,
                                           PW_STATUS_MSG("Constant"));
#endif  // PW_NC_TEST
#endif  // __cpp_constexpr >= 202211L

// A PW_STATUS_MSG is evaluated for the storage of the status it is passed to.
TEST(StatusMessageTest, StoredFormDependsOnStorageType) {
  using NoStorage = StatusStorage<PW_STATUS_OK, StatusMessageType::kNone>;
  using StringStorage =
      StatusStorage<PW_STATUS_OK, StatusMessageType::kStringLiteral>;
  using TokenStorage = StatusStorage<PW_STATUS_OK, StatusMessageType::kToken>;

  const auto enabled = _PW_STATUS_MSG_ENABLED("Sector CRC error");
  const auto enabled_empty = _PW_STATUS_MSG_ENABLED("");

  static_assert(std::is_same_v<decltype(enabled.Evaluate<NoStorage>()),
                               pw::internal::EmptyStatusMessage>);

  // Messages are stored as the text that is logged after the status code: the
  // message in parentheses, after a space.
  EXPECT_STREQ(enabled.Evaluate<StringStorage>(), " (Sector CRC error)");
  EXPECT_STREQ(enabled_empty.Evaluate<StringStorage>(), " ()");

  // The token is the one PW_TOKENIZE_STRING_MASK gives the stored string with
  // the storage's token mask. pw_Status codes use the low 5 bits.
  EXPECT_EQ(enabled.Evaluate<TokenStorage>(),
            pw::tokenizer::Hash(" (Sector CRC error)") & 0xFFFFFFE0u);
  EXPECT_EQ(enabled_empty.Evaluate<TokenStorage>(),
            pw::tokenizer::Hash(" ()") & 0xFFFFFFE0u);

  const auto disabled = _PW_STATUS_MSG_DISABLED("Sector CRC error");
  static_assert(std::is_same_v<decltype(disabled.Evaluate<NoStorage>()),
                               pw::internal::EmptyStatusMessage>);
  static_assert(std::is_same_v<decltype(disabled.Evaluate<StringStorage>()),
                               pw::internal::EmptyStatusMessage>);
  static_assert(std::is_same_v<decltype(disabled.Evaluate<TokenStorage>()),
                               pw::internal::EmptyStatusMessage>);
  static_cast<void>(disabled);

  const auto message = PW_STATUS_MSG("Sector CRC error");
  EXPECT_EQ(!pw::internal::IsEmpty(message.Evaluate<StringStorage>()),
            kMessagesEnabled);
}

// Message tokens are in a domain named for the status code's domain.
PW_CONSTEXPR_TEST(StatusMessageTest, DomainIsCodeDomainWithSuffix, {
  PW_TEST_EXPECT_STREQ(PW_STATUS_MSG_DOMAIN("::my::Code"),
                       "::my::Code::Message");
  PW_TEST_EXPECT_STREQ(pw::internal::kStatusMessageDomain<pw_Status>.chars,
                       "::pw::Status::Message");
  PW_TEST_EXPECT_EQ(sizeof(pw::internal::kStatusMessageDomain<pw_Status>.chars),
                    sizeof("::pw::Status::Message"));
});

// Token 0 means "no message", so a message never uses it.
PW_CONSTEXPR_TEST(StatusMessageTest, ZeroHashUsesLowestMaskBit, {
  using pw::internal::NonZeroStatusMessageToken;
  PW_TEST_EXPECT_EQ(NonZeroStatusMessageToken(0u, UINT32_MAX), 1u);
  PW_TEST_EXPECT_EQ(NonZeroStatusMessageToken(0u, 0xFFFFFFE0u), 0x20u);
  PW_TEST_EXPECT_EQ(NonZeroStatusMessageToken(0x12345660u, 0xFFFFFFE0u),
                    0x12345660u);
});

// The storage classes are tested directly so that every layout is covered in
// every configuration.

PW_CONSTEXPR_TEST(StatusStorageTest, None, {
  using Storage = StatusStorage<CustomError::kOk, StatusMessageType::kNone>;
  static_assert(sizeof(Storage) == sizeof(CustomError));

  Storage ok;
  PW_TEST_EXPECT_TRUE(ok.ok());
  PW_TEST_EXPECT_FALSE(ok.has_message());

  Storage err(CustomError::kOther, pw::internal::EmptyStatusMessage{});
  PW_TEST_EXPECT_FALSE(err.ok());
  PW_TEST_EXPECT_EQ(err.code(), CustomError::kOther);
  PW_TEST_EXPECT_FALSE(err.has_message());
});

PW_CONSTEXPR_TEST(StatusStorageTest, StringLiteral, {
  using Storage =
      StatusStorage<PW_STATUS_OK, StatusMessageType::kStringLiteral>;
  static_assert(sizeof(Storage) == 2 * sizeof(const char*));

  // No message is a null pointer.
  Storage ok;
  PW_TEST_EXPECT_TRUE(ok.ok());
  PW_TEST_EXPECT_FALSE(ok.has_message());
  PW_TEST_EXPECT_TRUE(ok.message() == nullptr);

  Storage code_only(PW_STATUS_INTERNAL);
  PW_TEST_EXPECT_FALSE(code_only.ok());
  PW_TEST_EXPECT_FALSE(code_only.has_message());
  PW_TEST_EXPECT_TRUE(code_only.message() == nullptr);

  Storage no_message(PW_STATUS_INTERNAL, nullptr);
  PW_TEST_EXPECT_FALSE(no_message.has_message());

  Storage err(PW_STATUS_INTERNAL, " (Detail)");
  PW_TEST_EXPECT_FALSE(err.ok());
  PW_TEST_EXPECT_EQ(err.code(), PW_STATUS_INTERNAL);
  PW_TEST_EXPECT_TRUE(err.has_message());
  PW_TEST_EXPECT_STREQ(err.message(), " (Detail)");

  // An empty string is a message, unlike a null pointer.
  Storage empty_message(PW_STATUS_INTERNAL, "");
  PW_TEST_EXPECT_TRUE(empty_message.has_message());
  PW_TEST_EXPECT_STREQ(empty_message.message(), "");

  Storage ok_with_message(PW_STATUS_OK, " (Detail)");
  PW_TEST_EXPECT_TRUE(ok_with_message.ok());
  PW_TEST_EXPECT_FALSE(ok_with_message.has_message());
  PW_TEST_EXPECT_TRUE(ok_with_message.message() == nullptr);
});

// pw_Status codes are 0 to 16, so the code uses the low 5 bits of the word.
PW_CONSTEXPR_TEST(StatusStorageTest, Token_PwStatus, {
  using Storage = StatusStorage<PW_STATUS_OK, StatusMessageType::kToken>;

  static_assert(sizeof(Storage) == sizeof(uint32_t));
  static_assert(Storage::kCodeBits == 5);
  static_assert(Storage::kTokenMask == 0xFFFFFFE0u);

  Storage ok;
  PW_TEST_EXPECT_TRUE(ok.ok());
  PW_TEST_EXPECT_EQ(ok.code(), PW_STATUS_OK);
  PW_TEST_EXPECT_FALSE(ok.has_message());
  PW_TEST_EXPECT_EQ(ok.message(), 0u);

  Storage code_only(PW_STATUS_UNAUTHENTICATED);
  PW_TEST_EXPECT_FALSE(code_only.ok());
  PW_TEST_EXPECT_EQ(code_only.code(), PW_STATUS_UNAUTHENTICATED);
  PW_TEST_EXPECT_FALSE(code_only.has_message());

  Storage err(PW_STATUS_UNAUTHENTICATED, 0xFFFFFFE0);
  PW_TEST_EXPECT_FALSE(err.ok());
  PW_TEST_EXPECT_EQ(err.code(), PW_STATUS_UNAUTHENTICATED);
  PW_TEST_EXPECT_TRUE(err.has_message());
  PW_TEST_EXPECT_EQ(err.message(), 0xFFFFFFE0u);

  Storage other(PW_STATUS_CANCELLED, 0x12345660);
  PW_TEST_EXPECT_EQ(other.code(), PW_STATUS_CANCELLED);
  PW_TEST_EXPECT_EQ(other.message(), 0x12345660u);

  Storage ok_with_token(PW_STATUS_OK, 0xFFFFFFE0);
  PW_TEST_EXPECT_TRUE(ok_with_token.ok());
  PW_TEST_EXPECT_FALSE(ok_with_token.has_message());
  PW_TEST_EXPECT_EQ(ok_with_token.message(), 0u);
});

[[maybe_unused]] void NonEnumStatusBaseStr() {
#if PW_NC_TEST(NonEnumStatusBaseStr)
  PW_NC_EXPECT("only supported for status types with enum codes");
  IntStatus status;
  [[maybe_unused]] const char* str = status.str();
#elif PW_NC_TEST(NodiscardStatusOf)
  PW_NC_EXPECT("ignoring return.*value");
  auto fn = []() -> AliasStatus { return AliasStatus(); };
  fn();
#elif PW_NC_TEST(StatusOfRequiresEnum)
  PW_NC_EXPECT("StatusOf requires an enum status code");
  StatusOf<0> status;
  status.IgnoreError();
#elif PW_NC_TEST(ImplicitConstructionFails)
  PW_NC_EXPECT("no viable conversion|cannot convert|non-scalar type");
  AliasStatus s = CustomError::kFailed;
  (void)s;
#elif PW_NC_TEST(RawStringLiteralMessageRejected)
  PW_NC_EXPECT("deleted");
  AliasMessageStatus status(PW_STATUS_INTERNAL, "raw literal not allowed");
  status.IgnoreError();
#elif PW_NC_TEST(RawStringLiteralMessageRejectedByPwStatus)
  PW_NC_EXPECT("deleted");
  pw::Status status(PW_STATUS_INTERNAL, "raw literal not allowed");
  status.IgnoreError();
#elif PW_NC_TEST(NonLiteralStatusMessageRejected)
  PW_NC_EXPECT("expected|string literal operator");
  const char* runtime_msg = "not a literal";
  AliasMessageStatus status(PW_STATUS_INTERNAL, PW_STATUS_MSG(runtime_msg));
  status.IgnoreError();
#elif PW_NC_TEST(WideStringStatusMessageRejected)
  PW_NC_EXPECT("static_cast|no matching function");
  AliasMessageStatus status(PW_STATUS_INTERNAL, PW_STATUS_MSG(L"wide"));
  status.IgnoreError();
#elif PW_NC_TEST(MessageRejectedIfDisabledForType)
  PW_NC_EXPECT("This status type cannot carry a message");
  NoMessageStatus status(PW_STATUS_INTERNAL, PW_STATUS_MSG("Opted out"));
  status.IgnoreError();
#elif PW_NC_TEST(MessageRejectedForEnumWithoutEnumTraits)
  PW_NC_EXPECT("This status type cannot carry a message");
  AliasStatus status(CustomError::kFailed, PW_STATUS_MSG("No PW_ENUM"));
  status.IgnoreError();
#elif PW_NC_TEST(MessageRejectedForIntegerCode)
  PW_NC_EXPECT("This status type cannot carry a message");
  StatusBase<IntStatus, 200>::Message message = PW_STATUS_MSG("Integer");
  static_cast<void>(message);
#elif PW_NC_TEST(MessageFromAnotherStatusTypeRejected)
  PW_NC_EXPECT("no matching|no viable|cannot convert|could not convert");
  const pw::Status not_found =
      pw::Status::NotFound(PW_STATUS_MSG("No such key"));
  MessageStatus status(PW_STATUS_NOT_FOUND, not_found.message());
  status.IgnoreError();
#elif PW_NC_TEST(TokenStorageRejectsCodeWithoutEnumTraits)
  PW_NC_EXPECT("does not support messages");
  StatusStorage<CustomError::kOk, StatusMessageType::kToken> storage;
  static_cast<void>(storage);
#endif  // PW_NC_TEST
}

}  // namespace
