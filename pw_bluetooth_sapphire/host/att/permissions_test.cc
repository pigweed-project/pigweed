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

#include "pw_bluetooth_sapphire/internal/host/att/permissions.h"

#include "pw_unit_test/framework.h"

namespace bt::att {
namespace {

const AccessRequirements kDisallowed;
const AccessRequirements kNoSecurityReq(/*encryption=*/false,
                                        /*authentication=*/false,
                                        /*authorization=*/false);
const AccessRequirements kEncryptionReq(/*encryption=*/true,
                                        /*authentication=*/false,
                                        /*authorization=*/false);
const AccessRequirements kEncryptionWithMinKeySizeReq(/*encryption=*/true,
                                                      /*authentication=*/false,
                                                      /*authorization=*/false,
                                                      7);
const AccessRequirements kAuthenticationReq(/*encryption=*/false,
                                            /*authentication=*/true,
                                            /*authorization=*/false);
const AccessRequirements kAuthenticationWithMinKeySizeReq(
    /*encryption=*/false,
    /*authentication=*/true,
    /*authorization=*/false,
    7);
const AccessRequirements kAuthorizationReq(/*encryption=*/false,
                                           /*authentication=*/false,
                                           /*authorization=*/true);
const AccessRequirements kEncryptionAndAuthorizationReq(
    /*encryption=*/true,
    /*authentication=*/false,
    /*authorization=*/true);
const AccessRequirements kAuthenticationAndAuthorizationReq(
    /*encryption=*/false,
    /*authentication=*/true,
    /*authorization=*/true);

const sm::SecurityProperties kNoSecurity(sm::SecurityLevel::kNoSecurity,
                                         16,
                                         /*secure_connections=*/false);
const sm::SecurityProperties kNoSecurityZeroKeySize(
    sm::SecurityLevel::kNoSecurity,
    0,
    /*secure_connections=*/false);
const sm::SecurityProperties kEncrypted(sm::SecurityLevel::kEncrypted,
                                        16,
                                        /*secure_connections=*/false);
const sm::SecurityProperties kEncryptedWithMinKeySize(
    sm::SecurityLevel::kEncrypted,
    7,
    /*secure_connections=*/false);
const sm::SecurityProperties kAuthenticated(sm::SecurityLevel::kAuthenticated,
                                            16,
                                            /*secure_connections=*/false);
const sm::SecurityProperties kAuthenticatedWithMinKeySize(
    sm::SecurityLevel::kAuthenticated,
    7,
    /*secure_connections=*/false);

TEST(PermissionsTest, ReadNotPermittedWhenDisallowed) {
  EXPECT_EQ(ErrorCode::kReadNotPermitted,
            CheckReadPermissions(kDisallowed, kNoSecurity).error_value());
  EXPECT_EQ(ErrorCode::kReadNotPermitted,
            CheckReadPermissions(kDisallowed, kEncrypted).error_value());
  EXPECT_EQ(ErrorCode::kReadNotPermitted,
            CheckReadPermissions(kDisallowed, kAuthenticated).error_value());
}

TEST(PermissionsTest, WriteNotPermittedWhenDisallowed) {
  EXPECT_EQ(ErrorCode::kWriteNotPermitted,
            CheckWritePermissions(kDisallowed, kNoSecurity).error_value());
  EXPECT_EQ(ErrorCode::kWriteNotPermitted,
            CheckWritePermissions(kDisallowed, kEncrypted).error_value());
  EXPECT_EQ(ErrorCode::kWriteNotPermitted,
            CheckWritePermissions(kDisallowed, kAuthenticated).error_value());
}

TEST(PermissionsTest, LinkNotSecure) {
  EXPECT_EQ(fit::ok(), CheckReadPermissions(kNoSecurityReq, kNoSecurity));
  EXPECT_EQ(fit::ok(), CheckWritePermissions(kNoSecurityReq, kNoSecurity));
  EXPECT_EQ(fit::ok(),
            CheckReadPermissions(kNoSecurityReq, kNoSecurityZeroKeySize));
  EXPECT_EQ(fit::ok(),
            CheckWritePermissions(kNoSecurityReq, kNoSecurityZeroKeySize));

  EXPECT_EQ(ErrorCode::kInsufficientAuthentication,
            CheckReadPermissions(kEncryptionReq, kNoSecurity).error_value());
  EXPECT_EQ(ErrorCode::kInsufficientAuthentication,
            CheckWritePermissions(kEncryptionReq, kNoSecurity).error_value());
  EXPECT_EQ(ErrorCode::kInsufficientAuthentication,
            CheckReadPermissions(kEncryptionReq, kNoSecurityZeroKeySize)
                .error_value());
  EXPECT_EQ(ErrorCode::kInsufficientAuthentication,
            CheckWritePermissions(kEncryptionReq, kNoSecurityZeroKeySize)
                .error_value());

  EXPECT_EQ(
      ErrorCode::kInsufficientAuthentication,
      CheckReadPermissions(kAuthenticationReq, kNoSecurity).error_value());
  EXPECT_EQ(
      ErrorCode::kInsufficientAuthentication,
      CheckWritePermissions(kAuthenticationReq, kNoSecurity).error_value());
  EXPECT_EQ(ErrorCode::kInsufficientAuthentication,
            CheckReadPermissions(kAuthenticationReq, kNoSecurityZeroKeySize)
                .error_value());
  EXPECT_EQ(ErrorCode::kInsufficientAuthentication,
            CheckWritePermissions(kAuthenticationReq, kNoSecurityZeroKeySize)
                .error_value());

  EXPECT_EQ(ErrorCode::kInsufficientAuthorization,
            CheckReadPermissions(kAuthorizationReq, kNoSecurity).error_value());
  EXPECT_EQ(
      ErrorCode::kInsufficientAuthorization,
      CheckWritePermissions(kAuthorizationReq, kNoSecurity).error_value());
  EXPECT_EQ(ErrorCode::kInsufficientAuthorization,
            CheckReadPermissions(kAuthorizationReq, kNoSecurityZeroKeySize)
                .error_value());
  EXPECT_EQ(ErrorCode::kInsufficientAuthorization,
            CheckWritePermissions(kAuthorizationReq, kNoSecurityZeroKeySize)
                .error_value());

  // Encryption and authentication requirements take precedence over
  // authorization on an unencrypted link.
  EXPECT_EQ(ErrorCode::kInsufficientAuthentication,
            CheckReadPermissions(kEncryptionAndAuthorizationReq, kNoSecurity)
                .error_value());
  EXPECT_EQ(ErrorCode::kInsufficientAuthentication,
            CheckWritePermissions(kEncryptionAndAuthorizationReq, kNoSecurity)
                .error_value());
  EXPECT_EQ(
      ErrorCode::kInsufficientAuthentication,
      CheckReadPermissions(kAuthenticationAndAuthorizationReq, kNoSecurity)
          .error_value());
  EXPECT_EQ(
      ErrorCode::kInsufficientAuthentication,
      CheckWritePermissions(kAuthenticationAndAuthorizationReq, kNoSecurity)
          .error_value());
}

TEST(PermissionsTest, LinkEncrypted) {
  EXPECT_EQ(fit::ok(), CheckReadPermissions(kNoSecurityReq, kEncrypted));
  EXPECT_EQ(fit::ok(), CheckWritePermissions(kNoSecurityReq, kEncrypted));

  EXPECT_EQ(fit::ok(), CheckReadPermissions(kEncryptionReq, kEncrypted));
  EXPECT_EQ(fit::ok(), CheckWritePermissions(kEncryptionReq, kEncrypted));

  EXPECT_EQ(ErrorCode::kInsufficientAuthentication,
            CheckReadPermissions(kAuthenticationReq, kEncrypted).error_value());
  EXPECT_EQ(
      ErrorCode::kInsufficientAuthentication,
      CheckWritePermissions(kAuthenticationReq, kEncrypted).error_value());

  // An unauthenticated encrypted link with insufficient key size should fail
  // with kInsufficientAuthentication rather than key size error.
  EXPECT_EQ(ErrorCode::kInsufficientAuthentication,
            CheckReadPermissions(kAuthenticationReq, kEncryptedWithMinKeySize)
                .error_value());
  EXPECT_EQ(ErrorCode::kInsufficientAuthentication,
            CheckWritePermissions(kAuthenticationReq, kEncryptedWithMinKeySize)
                .error_value());

  EXPECT_EQ(ErrorCode::kInsufficientAuthorization,
            CheckReadPermissions(kAuthorizationReq, kEncrypted).error_value());
  EXPECT_EQ(ErrorCode::kInsufficientAuthorization,
            CheckWritePermissions(kAuthorizationReq, kEncrypted).error_value());
  EXPECT_EQ(ErrorCode::kInsufficientAuthorization,
            CheckReadPermissions(kAuthorizationReq, kEncryptedWithMinKeySize)
                .error_value());
  EXPECT_EQ(ErrorCode::kInsufficientAuthorization,
            CheckWritePermissions(kAuthorizationReq, kEncryptedWithMinKeySize)
                .error_value());

  // Encryption is satisfied, so encryption + authorization fails with
  // kInsufficientAuthorization; authentication + authorization fails with
  // kInsufficientAuthentication.
  EXPECT_EQ(ErrorCode::kInsufficientAuthorization,
            CheckReadPermissions(kEncryptionAndAuthorizationReq, kEncrypted)
                .error_value());
  EXPECT_EQ(ErrorCode::kInsufficientAuthorization,
            CheckWritePermissions(kEncryptionAndAuthorizationReq, kEncrypted)
                .error_value());
  EXPECT_EQ(ErrorCode::kInsufficientAuthentication,
            CheckReadPermissions(kAuthenticationAndAuthorizationReq, kEncrypted)
                .error_value());
  EXPECT_EQ(
      ErrorCode::kInsufficientAuthentication,
      CheckWritePermissions(kAuthenticationAndAuthorizationReq, kEncrypted)
          .error_value());

  EXPECT_EQ(fit::ok(),
            CheckReadPermissions(kEncryptionWithMinKeySizeReq,
                                 kEncryptedWithMinKeySize));
  EXPECT_EQ(fit::ok(),
            CheckWritePermissions(kEncryptionWithMinKeySizeReq,
                                  kEncryptedWithMinKeySize));

  EXPECT_EQ(ErrorCode::kInsufficientEncryptionKeySize,
            CheckReadPermissions(kEncryptionReq, kEncryptedWithMinKeySize)
                .error_value());
  EXPECT_EQ(ErrorCode::kInsufficientEncryptionKeySize,
            CheckWritePermissions(kEncryptionReq, kEncryptedWithMinKeySize)
                .error_value());
}

TEST(PermissionsTest, LinkAuthenticated) {
  EXPECT_EQ(fit::ok(), CheckReadPermissions(kNoSecurityReq, kAuthenticated));
  EXPECT_EQ(fit::ok(), CheckWritePermissions(kNoSecurityReq, kAuthenticated));

  EXPECT_EQ(fit::ok(), CheckReadPermissions(kEncryptionReq, kAuthenticated));
  EXPECT_EQ(fit::ok(), CheckWritePermissions(kEncryptionReq, kAuthenticated));

  EXPECT_EQ(fit::ok(),
            CheckReadPermissions(kAuthenticationReq, kAuthenticated));
  EXPECT_EQ(fit::ok(),
            CheckWritePermissions(kAuthenticationReq, kAuthenticated));

  // Authorization is decoupled from encryption key size.
  EXPECT_EQ(fit::ok(), CheckReadPermissions(kAuthorizationReq, kAuthenticated));
  EXPECT_EQ(fit::ok(),
            CheckWritePermissions(kAuthorizationReq, kAuthenticated));
  EXPECT_EQ(
      fit::ok(),
      CheckReadPermissions(kAuthorizationReq, kAuthenticatedWithMinKeySize));
  EXPECT_EQ(
      fit::ok(),
      CheckWritePermissions(kAuthorizationReq, kAuthenticatedWithMinKeySize));

  EXPECT_EQ(
      fit::ok(),
      CheckReadPermissions(kEncryptionAndAuthorizationReq, kAuthenticated));
  EXPECT_EQ(
      fit::ok(),
      CheckWritePermissions(kEncryptionAndAuthorizationReq, kAuthenticated));
  EXPECT_EQ(
      fit::ok(),
      CheckReadPermissions(kAuthenticationAndAuthorizationReq, kAuthenticated));
  EXPECT_EQ(fit::ok(),
            CheckWritePermissions(kAuthenticationAndAuthorizationReq,
                                  kAuthenticated));

  EXPECT_EQ(ErrorCode::kInsufficientEncryptionKeySize,
            CheckReadPermissions(kEncryptionReq, kAuthenticatedWithMinKeySize)
                .error_value());
  EXPECT_EQ(ErrorCode::kInsufficientEncryptionKeySize,
            CheckWritePermissions(kEncryptionReq, kAuthenticatedWithMinKeySize)
                .error_value());
  EXPECT_EQ(ErrorCode::kInsufficientEncryptionKeySize,
            CheckReadPermissions(kEncryptionAndAuthorizationReq,
                                 kAuthenticatedWithMinKeySize)
                .error_value());
  EXPECT_EQ(ErrorCode::kInsufficientEncryptionKeySize,
            CheckWritePermissions(kEncryptionAndAuthorizationReq,
                                  kAuthenticatedWithMinKeySize)
                .error_value());

  // Authenticated link with 7-byte key fails kAuthenticationReq (default 16B).
  EXPECT_EQ(
      ErrorCode::kInsufficientEncryptionKeySize,
      CheckReadPermissions(kAuthenticationReq, kAuthenticatedWithMinKeySize)
          .error_value());
  EXPECT_EQ(
      ErrorCode::kInsufficientEncryptionKeySize,
      CheckWritePermissions(kAuthenticationReq, kAuthenticatedWithMinKeySize)
          .error_value());
  EXPECT_EQ(ErrorCode::kInsufficientEncryptionKeySize,
            CheckReadPermissions(kAuthenticationAndAuthorizationReq,
                                 kAuthenticatedWithMinKeySize)
                .error_value());
  EXPECT_EQ(ErrorCode::kInsufficientEncryptionKeySize,
            CheckWritePermissions(kAuthenticationAndAuthorizationReq,
                                  kAuthenticatedWithMinKeySize)
                .error_value());

  EXPECT_EQ(fit::ok(),
            CheckReadPermissions(kAuthenticationWithMinKeySizeReq,
                                 kAuthenticatedWithMinKeySize));
  EXPECT_EQ(fit::ok(),
            CheckWritePermissions(kAuthenticationWithMinKeySizeReq,
                                  kAuthenticatedWithMinKeySize));
}

}  // namespace
}  // namespace bt::att
