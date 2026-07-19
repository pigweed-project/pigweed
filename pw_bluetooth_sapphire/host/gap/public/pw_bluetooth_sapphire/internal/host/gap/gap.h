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
#include <pw_bluetooth/hci_events.emb.h>
#include <pw_chrono/system_clock.h>

#include <cstddef>
#include <cstdint>

#include "pw_bluetooth_sapphire/internal/host/common/identifier.h"
#include "pw_bluetooth_sapphire/internal/host/common/uuid.h"

// This file contains constants and numbers that are part of the Generic Access
// Profile specification.

namespace bt::gap {

// Bluetooth technologies that a device can support.
enum class TechnologyType {
  kLowEnergy,
  kClassic,
  kDualMode,
};
const char* TechnologyTypeToString(TechnologyType type);

enum class Mode {
  // Use the legacy HCI command set.
  kLegacy,

  // Use the extended HCI command set introduced in version 5.0
  kExtended,
};

// Enum for the supported values of the BR/EDR Security Mode as defined in
// Core Spec v5.4, Vol 3, Part C, 4.2.2.
enum class BrEdrSecurityMode {
  // Mode 4 entails possibly encrypted, possibly authenticated communication.
  Mode4,
  // Secure Connections Only mode enforces that all encrypted transmissions use
  // 128-bit,
  // SC-generated and authenticated encryption keys.
  SecureConnectionsOnly,
};
const char* BrEdrSecurityModeToString(BrEdrSecurityMode mode);

// Enum for the supported values of the LE Security Mode as defined in spec v5.2
// Vol 3 Part C 10.2.
enum class LESecurityMode {
  // Mode 1 entails possibly encrypted, possibly authenticated communication.
  Mode1,
  // Secure Connections Only mode enforces that all encrypted transmissions use
  // 128-bit,
  // SC-generated and authenticated encryption keys.
  SecureConnectionsOnly,
};
const char* LeSecurityModeToString(LESecurityMode mode);

const char* EncryptionStatusToString(
    pw::bluetooth::emboss::EncryptionStatus status);

enum class PairingStateType : uint8_t {
  kSecureSimplePairing,
  kLegacyPairing,
  kUnknown,
};
const char* PairingStateTypeToString(PairingStateType type);

// Placeholder assigned as the local name when gap::Adapter is initialized.
inline constexpr char kDefaultLocalName[] = "fuchsia";

// Constants used in BR/EDR Inquiry (Core Spec v5.0, Vol 2, Part C, Appendix A)
// Default cycles value for length of Inquiry. See T_gap(100).
// This is in 1.28s time slice units, and is 10.24 seconds.
inline constexpr uint8_t kInquiryLengthDefault = 0x08;

// The inquiry scan interval and window used by our stack. The unit for these
// values is controller timeslices (N) where Time in ms = N * 0.625ms
inline constexpr uint16_t kInquiryScanInterval = 0x01E0;  // 300 ms
inline constexpr uint16_t kInquiryScanWindow = 0x0012;    // 11.25 ms

// Upper bound on the number of BR/EDR services a single remote device
// legitimately advertises. Used to cap memory usage when processing EIR data.
inline constexpr size_t kMaxPeerEirServices = 64;

// Constants used in Low Energy (see Core Spec v5.0, Vol 3, Part C, Appendix A).

inline constexpr pw::chrono::SystemClock::duration kLEGeneralDiscoveryScanMin =
    std::chrono::milliseconds(10240);
inline constexpr pw::chrono::SystemClock::duration
    kLEGeneralDiscoveryScanMinCoded = std::chrono::milliseconds(30720);
inline constexpr pw::chrono::SystemClock::duration kLEScanFastPeriod =
    std::chrono::milliseconds(30720);

// The HCI spec defines the time conversion as follows: Time =  N * 0.625 ms,
// where N is the value of the constant.
inline constexpr float kHciScanIntervalToMs = 0.625f;
constexpr float HciScanIntervalToMs(uint16_t i) {
  return static_cast<float>(i) * kHciScanIntervalToMs;
}
constexpr float HciScanWindowToMs(uint16_t w) { return HciScanIntervalToMs(w); }

// Recommended scan and advertising parameters that can be passed directly to
// the HCI commands. A constant that contains the word "Coded" is recommended
// when using the LE Coded PHY. Otherwise the constant is recommended when using
// the LE 1M PHY. See Core Spec v5.2, Vol. 3, Part C, Table A for ranges and
// descriptions.

// For user-initiated scanning
inline constexpr uint16_t kLEScanFastInterval = 0x0060;       // 60 ms
inline constexpr uint16_t kLEScanFastIntervalCoded = 0x0120;  // 180 ms
inline constexpr uint16_t kLEScanFastWindow = 0x0030;         // 30 ms
inline constexpr uint16_t kLEScanFastWindowCoded = 0x90;      // 90 ms

// For background scanning
inline constexpr uint16_t kLEScanSlowInterval1 = 0x0800;       // 1.28 s
inline constexpr uint16_t kLEScanSlowInterval1Coded = 0x1800;  // 3.84 s
inline constexpr uint16_t kLEScanSlowWindow1 = 0x0012;         // 11.25 ms
inline constexpr uint16_t kLEScanSlowWindow1Coded = 0x0036;    // 33.75 ms
inline constexpr uint16_t kLEScanSlowInterval2 = 0x1000;       // 2.56 s
inline constexpr uint16_t kLEScanSlowInterval2Coded = 0x3000;  // 7.68 s
inline constexpr uint16_t kLEScanSlowWindow2 = 0x0024;         // 22.5 ms
inline constexpr uint16_t kLEScanSlowWindow2Coded = 0x006C;    // 67.5 ms

// Advertising parameters
inline constexpr uint16_t kLEAdvertisingFastIntervalMin1 = 0x0030;  // 30 ms
inline constexpr uint16_t kLEAdvertisingFastIntervalMax1 = 0x0060;  // 60 ms
inline constexpr uint16_t kLEAdvertisingFastIntervalMin2 = 0x00A0;  // 100 ms
inline constexpr uint16_t kLEAdvertisingFastIntervalMax2 = 0x00F0;  // 150 ms
inline constexpr uint16_t kLEAdvertisingFastIntervalCodedMin1 =
    0x0090;  // 90 ms
inline constexpr uint16_t kLEAdvertisingFastIntervalCodedMax1 =
    0x0120;  // 180 ms
inline constexpr uint16_t kLEAdvertisingFastIntervalCodedMin2 =
    0x01E0;  // 300 ms
inline constexpr uint16_t kLEAdvertisingFastIntervalCodedMax2 =
    0x02D0;  // 450 ms

inline constexpr uint16_t kLEAdvertisingSlowIntervalMin = 0x0640;       // 1 s
inline constexpr uint16_t kLEAdvertisingSlowIntervalMax = 0x0780;       // 1.2 s
inline constexpr uint16_t kLEAdvertisingSlowIntervalCodedMin = 0x12C0;  // 3 s
inline constexpr uint16_t kLEAdvertisingSlowIntervalCodedMax = 0x1680;  // 3.6 s

// Timeout used for the LE Create Connection command.
inline constexpr pw::chrono::SystemClock::duration kLECreateConnectionTimeout =
    std::chrono::seconds(20);
// Timeout used for the Br/Edr Create Connection command.
inline constexpr pw::chrono::SystemClock::duration
    kBrEdrCreateConnectionTimeout = std::chrono::seconds(20);

// Timeout used for scanning during LE General CEP. Selected to be longer than
// the scan period.
inline constexpr pw::chrono::SystemClock::duration kLEGeneralCepScanTimeout =
    std::chrono::seconds(20);

// Connection Interval Timing Parameters (see v5.0, Vol 3, Part C,
// Section 9.3.12 and Appendix A)
inline constexpr pw::chrono::SystemClock::duration
    kLEConnectionParameterTimeout = std::chrono::seconds(30);
// Recommended minimum time upon connection establishment before the central
// starts a connection update procedure.
inline constexpr pw::chrono::SystemClock::duration kLEConnectionPauseCentral =
    std::chrono::seconds(1);
// Recommended minimum time upon connection establishment before the peripheral
// starts a connection update procedure.
inline constexpr pw::chrono::SystemClock::duration
    kLEConnectionPausePeripheral = std::chrono::seconds(5);

inline constexpr uint16_t kLEInitialConnIntervalMin = 0x0018;       // 30 ms
inline constexpr uint16_t kLEInitialConnIntervalMax = 0x0028;       // 50 ms
inline constexpr uint16_t kLEInitialConnIntervalCodedMin = 0x0048;  // 90 ms
inline constexpr uint16_t kLEInitialConnIntervalCodedMax = 0x0078;  // 150 ms

// Time interval that must expire before a temporary device is removed from the
// cache.
inline constexpr pw::chrono::SystemClock::duration kCacheTimeout =
    std::chrono::seconds(60);

// Time interval between random address changes when privacy is enabled (see
// T_GAP(private_addr_int) in 5.0 Vol 3, Part C, Appendix A)
inline constexpr pw::chrono::SystemClock::duration kPrivateAddressTimeout =
    std::chrono::minutes(15);

// Maximum duration for which a scannable advertisement will be stored and not
// reported to clients until a corresponding scan response is received.
//
// This number has been determined empirically but over a limited number of
// devices. According to Core Spec. v5.2 Vol 6, Part B, Section 4.4 and in
// practice, the typical gap between the two events from the same peer is
// <=10ms. However in practice it's possible to see gaps as high as 1.5 seconds
// or more.
inline constexpr pw::chrono::SystemClock::duration kLEScanResponseTimeout =
    std::chrono::seconds(2);

// GATT types used in the GAP service.
inline constexpr UUID kGenericAccessService(uint16_t{0x1800});
inline constexpr UUID kDeviceNameCharacteristic(uint16_t{0x2A00});
inline constexpr UUID kAppearanceCharacteristic(uint16_t{0x2A01});
constexpr UUID kPeripheralPreferredConnectionParametersCharacteristic(uint16_t{
    0x2A04});

// The Peripheral Preferred Connection Parameters Characteristic is optionally
// included in the GAP service of a peripheral (Core Spec v5.2, Vol 3, Part C,
// Sec 9.12.3). See hci_spec::LEConnectionParameters for a description of these
// fields.
struct PeripheralPreferredConnectionParametersCharacteristicValue {
  uint16_t min_interval;
  uint16_t max_interval;
  uint16_t max_latency;
  uint16_t supervision_timeout;
} __attribute__((packed));

}  // namespace bt::gap
