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
#include <pw_chrono/system_clock.h>

#include <cstdint>

#include "pw_bluetooth_sapphire/internal/host/common/device_address.h"
#include "pw_bluetooth_sapphire/internal/host/common/uint128.h"

// This file defines constants that are used by the Security Manager Protocol
// (SMP) that operates over the L2CAP SMP channel.

namespace bt::sm {

// Core Spec v5.3, Vol 3, Part H, 3.2
inline constexpr uint16_t kNoSecureConnectionsMtu = 23;
inline constexpr uint16_t kLeSecureConnectionsMtu = 65;

// SMP Timeout in seconds (Core Spec v5.3, Vol 3, Part H, 3.4)
inline constexpr pw::chrono::SystemClock::duration kPairingTimeout =
    std::chrono::seconds(30);

// The supported encryption key sizes (Core Spec v5.3, Vol 3, Part H, 2.3.4).
inline constexpr uint8_t kMinEncryptionKeySize = 7;
inline constexpr uint8_t kMaxEncryptionKeySize = 16;

// These are the sample ltk and random from (Core Spec v5.3, Vol 6, Part C, 1),
// they are declared so that SecurityManager can reject any peers using them and
// inclusive-language: disable
// prevent a mitm.
constexpr UInt128 kSpecSampleLtk = {0xBF,
                                    0x01,
                                    0xFB,
                                    0x9D,
                                    0x4E,
                                    0xF3,
                                    0xBC,
                                    0x36,
                                    0xD8,
                                    0x74,
                                    0xF5,
                                    0x39,
                                    0x41,
                                    0x38,
                                    0x68,
                                    0x4C};
inline constexpr uint64_t kSpecSampleRandom = 0xABCDEF1234567890;

// The field that identifies the type of a command.
using Code = uint8_t;

struct Header {
  Code code;
} __attribute__((packed));

// Supported pairing methods.
enum class PairingMethod {
  // Unauthenticated
  kJustWorks,

  // Local host inputs passkey. Authenticated.
  kPasskeyEntryInput,

  // Local host displays passkey. Authenticated.
  kPasskeyEntryDisplay,

  // Authenticated, LE Secure Connections only.
  kNumericComparison,

  // Authenticated depending on OOB mechanism
  kOutOfBand,
};

enum class IOCapability : uint8_t {
  kDisplayOnly = 0x00,
  kDisplayYesNo = 0x01,
  kKeyboardOnly = 0x02,
  kNoInputNoOutput = 0x03,
  kKeyboardDisplay = 0x04,
};

enum class OOBDataFlag : uint8_t {
  kNotPresent = 0x00,
  kPresent = 0x01,
};

// Possible values that can be assigned to the "AuthReq" bit field (Core Spec
// v5.3, Vol 3, Part H, Figure 3.3).
enum AuthReq : uint8_t {
  // Indicates that bonding is requested.
  kBondingFlag = (1 << 0),

  // Indicates whether Man-in-the-middle protection is required.
  kMITM = (1 << 2),

  // Indicates whether Secure Connections is supported.
  kSC = (1 << 3),

  // Indicates whether Keypress notifications should be generated for the
  // Passkey Entry protocol.
  kKeypress = (1 << 4),

  // Indicates whether cross-transport key generation is supported for Secure
  // Connections.
  kCT2 = (1 << 5),
};
using AuthReqField = uint8_t;

// Possible values for the Key Distribution/Generation fields (Core Spec v5.3,
// Vol 3, Part H, Figure 3.11)
enum KeyDistGen : uint8_t {
  // LE: Indicates that the LTK will be distributed using the "Encryption
  // Information" command in LE legacy pairing. Ignored in LE Secure
  // Connections.
  //
  // BR/EDR: Indicates that the LTK will be derived from the BR/EDR Link Key.
  kEncKey = (1 << 0),

  // Indicates that the IRK will be distributed using the "Identity Information"
  // command and the Identity Address using the "Identity Address Information"
  // command.
  kIdKey = (1 << 1),

  // Indicates that the CSRK will be distributed using the "Signing Information"
  // command.
  kSignKey = (1 << 2),

  // LE: Indicates that the BR/EDR Link Key will be derived from the LTK.
  // Ignored if LE Secure Connections isn't supported.
  //
  // BR/EDR: Reserved for future use.
  kLinkKey = (1 << 3),
};
using KeyDistGenField = uint8_t;

// Possible failure reason codes used in the "Pairing Failed" command.
// (Core Spec v5.3, Vol 3, Part H, 3.5.5, Table 3.7).
enum class ErrorCode : uint8_t {
  // User input of passkey failed, e.g. due to cancelation.
  kPasskeyEntryFailed = 0x01,

  // OOB data is not available.
  kOOBNotAvailable = 0x02,

  // Authentication requirements cannot be met due to IO capabilities.
  kAuthenticationRequirements = 0x03,

  // The confirm value does not match what was calculated.
  kConfirmValueFailed = 0x04,

  // Pairing is not supported.
  kPairingNotSupported = 0x05,

  // The resultant encryption key size is insufficient given local security
  // requirements.
  kEncryptionKeySize = 0x06,

  // An SMP command is not supported.
  kCommandNotSupported = 0x07,

  // Pairing failed due to an unspecified reason.
  kUnspecifiedReason = 0x08,

  // Pairing/authentication procedure is disallowed because too little time has
  // elapsed since the last pairing/security request.
  kRepeatedAttempts = 0x09,

  // SMP command parameters were invalid.
  kInvalidParameters = 0x0A,

  // Indicates to the peer that the DHKey Check value received doesn't match the
  // one calculated locally.
  kDHKeyCheckFailed = 0x0B,

  // Indicates that the confirm values in the numeric comparison protocol do not
  // match.
  kNumericComparisonFailed = 0x0C,

  // Indicates that pairing over the LE transport failed due to a concurrent
  // pairing request over the BR/EDR transport.
  kBREDRPairingInProgress = 0x0D,

  // Indicates that the BR/EDR Link Key generated on the BR/EDR transport cannot
  // be used to derive keys for the LE transport.
  kCrossTransportKeyDerivationNotAllowed = 0x0E,
};

// Possible keypress notification types used in the "Keypress Notification"
// command (Core Spec v5.3, Vol 3, Part H, 3.5.8).
enum class KeypressNotificationType : uint8_t {
  kStarted = 0,
  kDigitEntered = 1,
  kDigitErased = 2,
  kCleared = 3,
  kCompleted = 4,
};

// Possible address types used in the "Identity Address Information" command
// (Core Spec v5.3, Vol 3, Part H, 3.6.5).
enum class AddressType : uint8_t {
  kPublic = 0x00,
  kStaticRandom = 0x01,
};

// ========== SMP PDUs ========
inline constexpr Code kInvalidCode = 0x00;

// ======================================
// Pairing Request (Core Spec v5.3, Vol 3, Part H, 3.5.1)
inline constexpr Code kPairingRequest = 0x01;
struct PairingRequestParams {
  // The local I/O capability.
  IOCapability io_capability;

  // Whether or not OOB authentication data is available.
  OOBDataFlag oob_data_flag;

  // The requested security properties (Core Spec v5.3, Vol 3, Part H, 2.3.1).
  AuthReqField auth_req;

  // Maximum encryption key size supported. Valid values are 7-16.
  uint8_t max_encryption_key_size;

  // The keys that the initiator requests to distribute/generate.
  KeyDistGenField initiator_key_dist_gen;

  // The keys that the responder requests to distribute/generate.
  KeyDistGenField responder_key_dist_gen;
} __attribute__((packed));

// =======================================
// Pairing Response (Core Spec v5.3, Vol 3, Part H, 3.5.2)
inline constexpr Code kPairingResponse = 0x02;
using PairingResponseParams = PairingRequestParams;

// ======================================
// Pairing Confirm (Core Spec v5.3, Vol 3, Part H, 3.5.3)
inline constexpr Code kPairingConfirm = 0x03;
using PairingConfirmValue = UInt128;

// =====================================
// Pairing Random (Core Spec v5.3, Vol 3, Part H, 3.5.4)
inline constexpr Code kPairingRandom = 0x04;
using PairingRandomValue = UInt128;

// =====================================
// Pairing Failed (Core Spec v5.3, Vol 3, Part H, 3.5.5)
inline constexpr Code kPairingFailed = 0x05;
using PairingFailedParams = ErrorCode;

// =============================================
// Encryption Information (LE Legacy Pairing only; Core Spec v5.3, Vol 3,
// Part H, 3.6.2)
inline constexpr Code kEncryptionInformation = 0x06;
using EncryptionInformationParams = UInt128;

// ====================================================================
// Central Identification (LE Legacy Pairing only; Core Spec v5.3, Vol 3,
// Part H, 3.6.3)
inline constexpr Code kCentralIdentification = 0x07;
struct CentralIdentificationParams {
  uint16_t ediv;
  uint64_t rand;
} __attribute__((packed));

// ===========================================
// Identity Information (Core Spec v5.3, Vol 3, Part H, 3.6.4)
inline constexpr Code kIdentityInformation = 0x08;
using IRK = UInt128;

// ===================================================
// Identity Address Information (Core Spec v5.3, Vol 3, Part H, 3.6.5)
inline constexpr Code kIdentityAddressInformation = 0x09;
struct IdentityAddressInformationParams {
  AddressType type;
  DeviceAddressBytes bd_addr;
} __attribute__((packed));

// ==========================================
// Signing Information (Core Spec v5.3, Vol 3, Part H, 3.6.6)
inline constexpr Code kSigningInformation = 0x0A;
using CSRK = UInt128;

// =======================================
// Security Request (Core Spec v5.3, Vol 3, Part H, 3.6.7)
inline constexpr Code kSecurityRequest = 0x0B;

// See enum AuthReq for parameters.

// ==================================================================
// Pairing Public Key (Secure Connections only; Core Spec v5.3, Vol 3, Part H,
// 3.5.6)
inline constexpr Code kPairingPublicKey = 0x0C;
struct PairingPublicKeyParams {
  uint8_t x[32];
  uint8_t y[32];
} __attribute__((packed));

// ======================================================================
// Pairing DHKey Check (LE Secure Connections only; Core Spec v5.3, Vol 3,
// Part H, 3.5.7)
inline constexpr Code kPairingDHKeyCheck = 0x0D;
using PairingDHKeyCheckValueE = UInt128;

// ============================================
// Keypress Notification (Core Spec v5.3, Vol 3, Part H, 3.5.8)
inline constexpr Code kKeypressNotification = 0x0E;

// See enum KeypressNotificationType above for parameters.

}  // namespace bt::sm
