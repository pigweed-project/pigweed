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

#include <cstdint>
#include <type_traits>
#include <utility>

#include "pw_compilation_testing/negative_compilation.h"
#include "pw_status/status.h"
#include "pw_status/try.h"
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

}  // namespace custom_err

namespace {

using ::custom_err::CustomError;
using ::pw::StatusBase;

class [[nodiscard]] TestStatus final
    : public StatusBase<TestStatus, CustomError::kOk> {
 public:
  constexpr TestStatus() = default;
  explicit constexpr TestStatus(CustomError code) : StatusBase(code) {}
};

PW_STATUS_TYPE(MacroStatus, CustomError::kOk);

enum UnscopedError {
  kUnscopedOk = 10,
  kUnscopedFailed = 20,
};

PW_STATUS_TYPE(UnscopedStatus, kUnscopedOk);

enum class SignedError : int16_t {
  kOk = 0,
  kNegative = -1,
};

PW_STATUS_TYPE(SignedStatus, SignedError::kOk);

class [[nodiscard]] IntStatus final : public StatusBase<IntStatus, 200> {
 public:
  constexpr IntStatus() = default;
  explicit constexpr IntStatus(int code) : StatusBase(code) {}
};

static_assert(std::is_same_v<TestStatus::Code, CustomError>);
static_assert(std::is_same_v<MacroStatus::Code, CustomError>);
static_assert(sizeof(TestStatus) == sizeof(uint8_t));
static_assert(sizeof(MacroStatus) == sizeof(uint8_t));
static_assert(sizeof(SignedStatus) == sizeof(int16_t));
static_assert(std::is_same_v<IntStatus::Code, int>);
static_assert(sizeof(IntStatus) == sizeof(int));

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
static_assert(!std::is_convertible_v<CustomError, MacroStatus>);
static_assert(std::is_constructible_v<MacroStatus, CustomError>);
static_assert(!std::is_convertible_v<int, IntStatus>);
static_assert(std::is_constructible_v<IntStatus, int>);

// Different status types cannot be implicitly converted to each other.
static_assert(!std::is_convertible_v<MacroStatus, TestStatus>);
static_assert(!std::is_convertible_v<TestStatus, MacroStatus>);
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
  MacroStatus(CustomError::kFailed).IgnoreError();
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

PW_CONSTEXPR_TEST(StatusBaseTest, MacroStatus_Basic, {
  MacroStatus status;
  PW_TEST_EXPECT_TRUE(status.ok());
  PW_TEST_EXPECT_EQ(status.code(), CustomError::kOk);

  MacroStatus err(CustomError::kTimeout);
  PW_TEST_EXPECT_FALSE(err.ok());
  PW_TEST_EXPECT_EQ(err, CustomError::kTimeout);
  PW_TEST_EXPECT_EQ(CustomError::kTimeout, err);

  err.Update(MacroStatus(CustomError::kFailed));
  PW_TEST_EXPECT_EQ(err, CustomError::kTimeout);
});

PW_CONSTEXPR_TEST(StatusBaseTest, MacroStatus_PwTry, {
  constexpr auto TryFunc = [](MacroStatus s) -> MacroStatus {
    PW_TRY(s);
    return MacroStatus(CustomError::kOther);
  };

  PW_TEST_EXPECT_EQ(TryFunc(MacroStatus(CustomError::kOk)),
                    CustomError::kOther);
  PW_TEST_EXPECT_EQ(TryFunc(MacroStatus(CustomError::kFailed)),
                    CustomError::kFailed);
});

PW_CONSTEXPR_TEST(StatusBaseTest, CodeAlias, {
  MacroStatus m(MacroStatus::Code::kTimeout);
  PW_TEST_EXPECT_EQ(m, MacroStatus::Code::kTimeout);
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

PW_STATUS_TYPE(DriverStatus, DriverError::kOk);

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

  MacroStatus macro_ok;
  PW_TEST_EXPECT_STREQ(macro_ok.str(), "Ok");
  MacroStatus macro_fail(CustomError::kFailed);
  PW_TEST_EXPECT_STREQ(macro_fail.str(), "Failed");
});

[[maybe_unused]] void NonEnumStatusBaseStr() {
#if PW_NC_TEST(NonEnumStatusBaseStr)
  PW_NC_EXPECT("only supported for status types with enum codes");
  IntStatus status;
  [[maybe_unused]] const char* str = status.str();
#elif PW_NC_TEST(NodiscardMacroStatus)
  PW_NC_EXPECT("ignoring return.*value");
  auto fn = []() -> MacroStatus { return MacroStatus(); };
  fn();
#elif PW_NC_TEST(StatusTypeMacroRequiresEnum)
  PW_NC_EXPECT("PW_STATUS_TYPE requires an enum status code");
  PW_STATUS_TYPE(IntMacroStatus, 0);
  IntMacroStatus status;
  status.IgnoreError();
#elif PW_NC_TEST(ImplicitConstructionFails)
  PW_NC_EXPECT("no viable conversion|cannot convert|non-scalar type");
  MacroStatus s = CustomError::kFailed;
  (void)s;
#endif  // PW_NC_TEST
}

}  // namespace
