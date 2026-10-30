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

namespace bt::att {
namespace {

fit::result<ErrorCode> CheckSecurity(const AccessRequirements& reqs,
                                     const sm::SecurityProperties& security) {
  if (reqs.encryption_required() &&
      security.level() < sm::SecurityLevel::kEncrypted) {
    // If the peer is bonded but the link is not encrypted the "Insufficient
    // Encryption" error should be sent. Our GAP layer always keeps the link
    // encrypted so the authentication procedure needs to fail during
    // connection. We don't distinguish this from the un-paired state.
    // (NOTE: It is possible for the link to be authenticated without encryption
    // in LE Security Mode 2, which we do not support).
    return fit::error(ErrorCode::kInsufficientAuthentication);
  }

  if (reqs.authentication_required() &&
      security.level() < sm::SecurityLevel::kAuthenticated) {
    return fit::error(ErrorCode::kInsufficientAuthentication);
  }

  if (reqs.authorization_required() &&
      security.level() < sm::SecurityLevel::kAuthenticated) {
    return fit::error(ErrorCode::kInsufficientAuthorization);
  }

  // Only check key size if the link is encrypted and encryption/authentication
  // is required.
  if ((reqs.encryption_required() || reqs.authentication_required()) &&
      security.encrypted() &&
      security.enc_key_size() < reqs.min_enc_key_size()) {
    return fit::error(ErrorCode::kInsufficientEncryptionKeySize);
  }

  return fit::ok();
}

}  // namespace

fit::result<ErrorCode> CheckReadPermissions(
    const AccessRequirements& reqs, const sm::SecurityProperties& security) {
  if (!reqs.allowed()) {
    return fit::error(ErrorCode::kReadNotPermitted);
  }
  return CheckSecurity(reqs, security);
}

fit::result<ErrorCode> CheckWritePermissions(
    const AccessRequirements& reqs, const sm::SecurityProperties& security) {
  if (!reqs.allowed()) {
    return fit::error(ErrorCode::kWriteNotPermitted);
  }
  return CheckSecurity(reqs, security);
}

}  // namespace bt::att
