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

#pragma once
#include <lib/fit/function.h>

#include <optional>
#include <string>
#include <unordered_map>

#include "pw_bluetooth_sapphire/internal/host/common/inspect.h"
#include "pw_bluetooth_sapphire/internal/host/common/uint128.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/link_key.h"
#include "pw_bluetooth_sapphire/internal/host/sm/smp.h"

namespace bt::sm {

const std::unordered_map<Code, size_t> kCodeToPayloadSize{
    {kSecurityRequest, sizeof(AuthReqField)},
    {kPairingRequest, sizeof(PairingRequestParams)},
    {kPairingResponse, sizeof(PairingResponseParams)},
    {kPairingConfirm, sizeof(PairingConfirmValue)},
    {kPairingRandom, sizeof(PairingRandomValue)},
    {kPairingFailed, sizeof(PairingFailedParams)},
    {kEncryptionInformation, sizeof(EncryptionInformationParams)},
    {kCentralIdentification, sizeof(CentralIdentificationParams)},
    {kIdentityInformation, sizeof(IRK)},
    {kIdentityAddressInformation, sizeof(IdentityAddressInformationParams)},
    {kPairingPublicKey, sizeof(PairingPublicKeyParams)},
    {kPairingDHKeyCheck, sizeof(PairingDHKeyCheckValueE)},
};

// The available algorithms used to generate the cross-transport key during
// pairing.
enum CrossTransportKeyAlgo {
  // Use only the H6 function during cross-transport derivation (v5.2 Vol. 3
  // Part H 2.2.10).
  kUseH6,

  // Use the H7 function during cross-transport derivation (v5.2 Vol. 3 Part
  // H 2.2.11).
  kUseH7
};

// Represents the features exchanged during Pairing Phase 1.
struct PairingFeatures final {
  // True if the local device is in the "initiator" role.
  bool initiator = false;

  // True if LE Secure Connections pairing should be used. Otherwise, LE Legacy
  // Pairing should be used.
  bool secure_connections = false;

  // True if pairing is to be performed with bonding, false if not
  bool will_bond = false;

  // If present, prescribes the algorithm to use during cross-transport key
  // derivation. If not present, cross-transport key derivation should not take
  // place.
  std::optional<CrossTransportKeyAlgo> generate_ct_key;

  // Indicates the key generation model used for Phase 2.
  PairingMethod method = PairingMethod::kJustWorks;

  // The negotiated encryption key size.
  uint8_t encryption_key_size = 0;

  // The keys that we must distribute to the peer.
  KeyDistGenField local_key_distribution = 0;

  // The keys that will be distributed to us by the peer.
  KeyDistGenField remote_key_distribution = 0;
};

constexpr KeyDistGenField DistributableKeys(KeyDistGenField keys,
                                            bool is_bredr) {
  // The link key field never affects the distributed keys. It only has meaning
  // when the devices use LE Secure Connections, where it means the devices
  // should generate the BR/EDR Link Key locally.
  keys &= ~KeyDistGen::kLinkKey;
  if (is_bredr) {
    // In BR/EDR, EncKey is repurposed to mean the devices should generate the
    // LE LTK locally.
    keys &= ~KeyDistGen::kEncKey;
  }
  return keys;
}

// Returns a bool indicating whether `features` calls for the devices to
// exchange key information during the Key Distribution/Generation Phase 3.
bool HasKeysToDistribute(PairingFeatures features, bool is_bredr);

// Each enum variant corresponds to either:
// 1) a LE security mode 1 level (Core Spec v5.4, Vol 3, Part C 10.2.1)
// 2) a BR/EDR security mode 4 level (Core Spec v5.4, Vol 3, Part C, 5.2.2.8)
// Fuchsia only supports encryption based security
// TODO(fxbug.dev/42113587): Implement BR/EDR security database
enum class SecurityLevel {
  // No encryption
  kNoSecurity = 1,

  // inclusive-language: ignore
  // Encrypted without MITM protection (unauthenticated)
  kEncrypted = 2,

  // inclusive-language: ignore
  // Encrypted with MITM protection (authenticated)
  kAuthenticated = 3,

  // inclusive-language: ignore
  // Encrypted with MITM protection, Secure Connections, and a 128-bit
  // encryption key.
  kSecureAuthenticated = 4,
};

// Returns a string representation of |level| for debug messages.
const char* LevelToString(SecurityLevel level);

// Represents the security properties of a key. The security properties of a
// connection's LTK defines the security properties of the connection.
class SecurityProperties final {
 public:
  SecurityProperties();
  SecurityProperties(SecurityLevel level,
                     size_t enc_key_size,
                     bool secure_connections);
  SecurityProperties(bool encrypted,
                     bool authenticated,
                     bool secure_connections,
                     size_t enc_key_size);
  // Build from a BR/EDR Link Key that resulted from pairing. |lk_type| should
  // not be kChangedCombination, because that means that the link key is the
  // same type as before it was changed, which this has no knowledge of.
  //
  // Legacy pairing keys will be considered to have security level kNoSecurity
  // because legacy pairing is superseded by Secure Simple Pairing in Core Spec
  // v2.1 + EDR in 2007. Backwards compatiblity is optional per v5.0, Vol 3,
  // Part C, Section 5. Furthermore, the last Core Spec with only legacy pairing
  // (v2.0 + EDR) was withdrawn by Bluetooth SIG on 2019-01-28.
  //
  // TODO(fxbug.dev/42111880): SecurityProperties will treat kDebugCombination
  // keys as "encrypted, unauthenticated, and no Secure Connections" to
  // potentially allow their use as valid link keys, but does not store the fact
  // that they originate from a controller in pairing debug mode, a potential
  // hazard. Care should be taken at the controller interface to enforce
  // particular policies regarding debug keys.
  explicit SecurityProperties(hci_spec::LinkKeyType lk_type,
                              std::optional<size_t> enc_key_size);

  ~SecurityProperties() = default;

  // Copy constructors that ignore InspectProperties
  SecurityProperties(const SecurityProperties& other);
  SecurityProperties& operator=(const SecurityProperties& other);

  SecurityLevel level() const;
  size_t enc_key_size() const { return enc_key_size_; }
  bool encrypted() const { return properties_ & Property::kEncrypted; }
  bool secure_connections() const {
    return properties_ & Property::kSecureConnections;
  }
  bool authenticated() const { return properties_ & Property::kAuthenticated; }

  // Returns the BR/EDR link key type that produces the current security
  // properties. Returns kCombination if the current security level is
  // kNoSecurity since it is the key type generated by legacy pairing.
  //
  // SecurityProperties does not encode the use of
  // LinkKeyType::kDebugCombination keys (see Core Spec v5.0 Vol 2, Part E
  // Section 7.6.4), produced when a controller is in debug mode, so
  // SecurityProperties constructed from LinkKeyType::kDebugCombination returns
  // LinkKeyType::kUnauthenticatedCombination192 from this method.
  hci_spec::LinkKeyType GetLinkKeyType() const;

  // Returns a string representation of these properties.
  std::string ToString() const;

  // Returns whether `this` SecurityProperties is at least as secure as |other|.
  // This checks the encryption/authentication level of `this` vs. other, that
  // `this` used secure connections if |other| did, and that `this` encryption
  // key size is at least as large as |others|.
  bool IsAsSecureAs(const SecurityProperties& other) const;

  // Attach pairing state inspect node named |name| as a child of |parent|.
  void AttachInspect(inspect::Node& parent, std::string name);

  // Compare two properties for equality.
  bool operator==(const SecurityProperties& other) const {
    return properties_ == other.properties_ &&
           enc_key_size_ == other.enc_key_size_;
  }

  bool operator!=(const SecurityProperties& other) const {
    return !(*this == other);
  }

 private:
  struct InspectProperties {
    inspect::StringProperty level;
    inspect::BoolProperty encrypted;
    inspect::BoolProperty secure_connections;
    inspect::BoolProperty authenticated;
    inspect::StringProperty key_type;
  };
  InspectProperties inspect_properties_;
  inspect::Node inspect_node_;

  // Possible security properties for a link.
  enum Property : uint8_t {
    kEncrypted = (1 << 0),
    kAuthenticated = (1 << 1),
    kSecureConnections = (1 << 2)
  };
  using PropertiesField = uint8_t;
  PropertiesField properties_;
  size_t enc_key_size_;
};

// Represents a reusable long term key for a specific transport.
class LTK final {
 public:
  LTK() = default;
  LTK(const SecurityProperties& security, const hci_spec::LinkKey& key);

  const SecurityProperties& security() const { return security_; }
  const hci_spec::LinkKey& key() const { return key_; }

  bool operator==(const LTK& other) const {
    return security() == other.security() && key() == other.key();
  }

  bool operator!=(const LTK& other) const { return !(*this == other); }

  // Attach |security_| as child node of |parent| with specified |name|.
  void AttachInspect(inspect::Node& parent, std::string name);

 private:
  SecurityProperties security_;
  hci_spec::LinkKey key_;
};

// Represents a 128-bit key.
class Key final {
 public:
  Key() = default;
  Key(const SecurityProperties& security, const UInt128& value);

  const SecurityProperties& security() const { return security_; }
  const UInt128& value() const { return value_; }

  bool operator==(const Key& other) const {
    return security() == other.security() && value() == other.value();
  }

 private:
  SecurityProperties security_;
  UInt128 value_;
};

// Container for LE pairing data.
struct PairingData final {
  // The identity address.
  std::optional<DeviceAddress> identity_address;

  // The long term link encryption key generated by the local device. For LTKs
  // generated by Secure Connections, this will be the same as peer_ltk.
  std::optional<sm::LTK> local_ltk;

  // The long term link encryption key generated by the peer device. For LTKs
  // generated by Secure Connections, this will be the same as local_ltk.
  std::optional<sm::LTK> peer_ltk;

  // The cross-transport key for pairing-free encryption on the other transport.
  std::optional<sm::LTK> cross_transport_key;

  // The identity resolving key used to resolve RPAs to |identity|.
  std::optional<sm::Key> irk;

  // The connection signature resolving key used in LE security mode 2.
  std::optional<sm::Key> csrk;

  bool operator==(const PairingData& other) const {
    return identity_address == other.identity_address &&
           local_ltk == other.local_ltk && peer_ltk == other.peer_ltk &&
           irk == other.irk && csrk == other.csrk;
  }
};

// Container for identity information for distribution.
struct IdentityInfo {
  UInt128 irk;
  DeviceAddress address;
};

// Enum for the possible values of the SM Bondable Mode as defined in spec V5.1
// Vol 3 Part C Section 9.4
enum class BondableMode {
  // Allows pairing which results in bonding, as well as pairing which does not
  Bondable,
  // Does not allow pairing which results in bonding
  NonBondable,
};

// Represents the local device's settings for easy mapping to
// Pairing(Request|Response)Parameters.
struct LocalPairingParams {
  // The local I/O capability.
  IOCapability io_capability;
  // Whether or not OOB authentication data is available locally. Defaults to no
  // OOB data.
  OOBDataFlag oob_data_flag = OOBDataFlag::kNotPresent;
  // The local requested security properties (Vol 3, Part H, 2.3.1). Defaults to
  // no Authentication Requirements.
  AuthReqField auth_req = 0u;
  // Maximum encryption key size supported by the local device. Valid values are
  // 7-16. Defaults to maximum allowed encryption key size.
  uint8_t max_encryption_key_size = kMaxEncryptionKeySize;
  // The keys that the local system is able to distribute. Defaults to
  // distributing no keys.
  KeyDistGenField local_keys = 0u;
  // The keys that are desired from the peer. Defaults to distributing no keys.
  KeyDistGenField remote_keys = 0u;
};

// These roles correspond to the device which starts pairing.
enum class Role {
  // The LMP Central device is always kInitiator (V5.0 Vol. 3 Part H Appendix
  // C.1).
  kInitiator,

  // The LMP Peripheral device is always kResponder (V5.0 Vol. 3 Part H Appendix
  // C.1).
  kResponder
};

using PairingProcedureId = uint64_t;

// Used by Phase 2 classes to notify their owner that a new encryption key is
// ready. For Legacy Pairing, this is the STK which may only be used for the
// current session. For Secure Connections, this is the LTK which may be
// persisted.
using OnPhase2KeyGeneratedCallback = fit::function<void(const UInt128&)>;

// Used to notify classes of peer Pairing Requests.
using PairingRequestCallback = fit::function<void(PairingRequestParams)>;

}  // namespace bt::sm
