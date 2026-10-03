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
#include "pw_status/try.h"
#include "pw_unit_test/constexpr.h"
#include "pw_unit_test/framework.h"
#include "status_test_enum.h"

namespace {

PW_STATUS_TYPE(PwEnumStatus, ::pw::status_test::GeneratedEnum::kOk);

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

}  // namespace
