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

#include "pw_bluetooth_sapphire/internal/host/sm/types.h"

#include <cpp-string/string_printf.h>
#include <pw_assert/check.h>
#include <pw_preprocessor/compiler.h>

#include <utility>

#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/util.h"
#include "pw_bluetooth_sapphire/internal/host/sm/smp.h"

namespace bt::sm {
namespace {
const char* const kInspectLevelPropertyName = "level";
const char* const kInspectEncryptedPropertyName = "encrypted";
const char* const kInspectSecureConnectionsPropertyName = "secure_connections";
const char* const kInspectAuthenticatedPropertyName = "authenticated";
const char* const kInspectKeyTypePropertyName = "key_type";

bool IsEncryptedKey(hci_spec::LinkKeyType lk_type) {
  return (lk_type == hci_spec::LinkKeyType::kDebugCombination ||
          lk_type == hci_spec::LinkKeyType::kUnauthenticatedCombination192 ||
          lk_type == hci_spec::LinkKeyType::kUnauthenticatedCombination256 ||
          lk_type == hci_spec::LinkKeyType::kAuthenticatedCombination192 ||
          lk_type == hci_spec::LinkKeyType::kAuthenticatedCombination256);
}

bool IsAuthenticatedKey(hci_spec::LinkKeyType lk_type) {
  return (lk_type == hci_spec::LinkKeyType::kAuthenticatedCombination192 ||
          lk_type == hci_spec::LinkKeyType::kAuthenticatedCombination256);
}

bool IsSecureConnectionsKey(hci_spec::LinkKeyType lk_type) {
  return (lk_type == hci_spec::LinkKeyType::kUnauthenticatedCombination256 ||
          lk_type == hci_spec::LinkKeyType::kAuthenticatedCombination256);
}

}  // namespace

bool HasKeysToDistribute(PairingFeatures features, bool is_bredr) {
  return DistributableKeys(features.local_key_distribution, is_bredr) ||
         DistributableKeys(features.remote_key_distribution, is_bredr);
}

const char* LevelToString(SecurityLevel level) {
  PW_MODIFY_DIAGNOSTICS_PUSH();
  PW_MODIFY_DIAGNOSTIC(ignored, "-Wswitch-enum");
  switch (level) {
    case SecurityLevel::kEncrypted:
      return "encrypted";
    case SecurityLevel::kAuthenticated:
      return "Authenticated";
    case SecurityLevel::kSecureAuthenticated:
      return "Authenticated with Secure Connections and 128-bit key";
    default:
      break;
  }
  PW_MODIFY_DIAGNOSTICS_POP();
  return "not secure";
}

SecurityProperties::SecurityProperties()
    : SecurityProperties(/*encrypted=*/false,
                         /*authenticated=*/false,
                         /*secure_connections=*/false,
                         0u) {}

SecurityProperties::SecurityProperties(bool encrypted,
                                       bool authenticated,
                                       bool secure_connections,
                                       size_t enc_key_size)
    : properties_(0u), enc_key_size_(enc_key_size) {
  properties_ |= (encrypted ? Property::kEncrypted : 0u);
  properties_ |= (authenticated ? Property::kAuthenticated : 0u);
  properties_ |= (secure_connections ? Property::kSecureConnections : 0u);
}

SecurityProperties::SecurityProperties(SecurityLevel level,
                                       size_t enc_key_size,
                                       bool secure_connections)
    : SecurityProperties((level >= SecurityLevel::kEncrypted),
                         (level >= SecurityLevel::kAuthenticated),
                         secure_connections,
                         enc_key_size) {}
// All BR/EDR link keys, even those from legacy pairing or based on 192-bit EC
// points, are stored in 128 bits, according to Core Spec v5.0, Vol 2, Part H
// Section 3.1 "Key Types."
SecurityProperties::SecurityProperties(hci_spec::LinkKeyType lk_type,
                                       std::optional<size_t> enc_key_size)
    : SecurityProperties(IsEncryptedKey(lk_type),
                         IsAuthenticatedKey(lk_type),
                         IsSecureConnectionsKey(lk_type),
                         enc_key_size.value_or(0)) {
  PW_DCHECK(lk_type != hci_spec::LinkKeyType::kChangedCombination,
            "Can't infer security information from a Changed Combination Key");
}

SecurityProperties::SecurityProperties(const SecurityProperties& other) {
  *this = other;
}

SecurityProperties& SecurityProperties::operator=(
    const SecurityProperties& other) {
  properties_ = other.properties_;
  enc_key_size_ = other.enc_key_size_;
  return *this;
}

SecurityLevel SecurityProperties::level() const {
  auto level = SecurityLevel::kNoSecurity;
  if (properties_ & Property::kEncrypted) {
    level = SecurityLevel::kEncrypted;
    if (properties_ & Property::kAuthenticated) {
      level = SecurityLevel::kAuthenticated;
      if (enc_key_size_ == kMaxEncryptionKeySize &&
          (properties_ & Property::kSecureConnections)) {
        level = SecurityLevel::kSecureAuthenticated;
      }
    }
  }
  return level;
}

hci_spec::LinkKeyType SecurityProperties::GetLinkKeyType() const {
  if (level() == SecurityLevel::kNoSecurity) {
    // Sapphire considers legacy pairing keys to have security level
    // kNoSecurity. Returning kCombination type since the kLocalUnit and
    // kRemoteUnit key types are deprecated.
    //
    // TODO(fxbug.dev/42113587): Implement BR/EDR security database
    return hci_spec::LinkKeyType::kCombination;
  }

  if (authenticated()) {
    if (secure_connections()) {
      return hci_spec::LinkKeyType::kAuthenticatedCombination256;
    } else {
      return hci_spec::LinkKeyType::kAuthenticatedCombination192;
    }
  } else {
    if (secure_connections()) {
      return hci_spec::LinkKeyType::kUnauthenticatedCombination256;
    } else {
      return hci_spec::LinkKeyType::kUnauthenticatedCombination192;
    }
  }
}

std::string SecurityProperties::ToString() const {
  if (level() == SecurityLevel::kNoSecurity) {
    return "[no security]";
  }
  // inclusive-language: disable
  return bt_lib_cpp_string::StringPrintf(
      "[%s%s%skey size: %zu]",
      encrypted() ? "encrypted " : "",
      authenticated() ? "authenticated (MITM) " : "",
      secure_connections() ? "secure connections " : "legacy authentication ",
      enc_key_size());
  // inclusive-language: enable
}

bool SecurityProperties::IsAsSecureAs(const SecurityProperties& other) const {
  // clang-format off
  return
    (encrypted() || !other.encrypted()) &&
    (authenticated() || !other.authenticated()) &&
    (secure_connections() || !other.secure_connections()) &&
    (enc_key_size_ >= other.enc_key_size_);
  // clang-format on
}

void SecurityProperties::AttachInspect(inspect::Node& parent,
                                       std::string name) {
  inspect_node_ = parent.CreateChild(name);

  inspect_properties_.level = inspect_node_.CreateString(
      kInspectLevelPropertyName, LevelToString(level()));
  inspect_properties_.encrypted =
      inspect_node_.CreateBool(kInspectEncryptedPropertyName, encrypted());
  inspect_properties_.secure_connections = inspect_node_.CreateBool(
      kInspectSecureConnectionsPropertyName, secure_connections());
  inspect_properties_.authenticated = inspect_node_.CreateBool(
      kInspectAuthenticatedPropertyName, authenticated());
  inspect_properties_.key_type = inspect_node_.CreateString(
      kInspectKeyTypePropertyName,
      hci_spec::LinkKeyTypeToString(GetLinkKeyType()));
}

LTK::LTK(const SecurityProperties& security, const hci_spec::LinkKey& key)
    : security_(security), key_(key) {}

void LTK::AttachInspect(inspect::Node& parent, std::string name) {
  security_.AttachInspect(parent, std::move(name));
}

Key::Key(const SecurityProperties& security, const UInt128& value)
    : security_(security), value_(value) {}

}  // namespace bt::sm
