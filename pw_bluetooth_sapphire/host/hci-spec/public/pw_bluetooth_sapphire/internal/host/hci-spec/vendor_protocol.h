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
// This file contains general opcode/number and static packet definitions for
// extensions to the Bluetooth Host-Controller interface. These extensions
// aren't standardized through the Bluetooth SIG and their documentation is
// available separately (linked below). Each packet payload structure contains
// parameter descriptions based on their respective documentation.
//
// Documentation links:
//
//    - Android: https://source.android.com/devices/bluetooth/hci_requirements
//
// NOTE: The definitions below are incomplete. They get added as needed. This
// list will grow as we support more vendor features.

#include <pw_bluetooth/hci_android.emb.h>

#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"

namespace bt::hci_spec::vendor::android {

inline constexpr uint32_t kLdacVendorId = 0x0000012D;
inline constexpr uint16_t kLdacCodecId = 0x00AA;

// ============================================================================
// Multiple Advertising
//
// NOTE: Multiple advertiser support is deprecated in the Google feature spec
// v0.98 and above. Users of the following vendor extension HCI commands should
// first ensure that the controller is using a compatible Google feature spec.

// The kLEMultiAdvt opcode is shared across all multiple advertising HCI
// commands. To differentiate between the multiple commands, a subopcode field
// is included in the command payload.
inline constexpr OpCode kLEMultiAdvt = VendorOpCode(0x154);
inline constexpr uint8_t kLEMultiAdvtSetAdvtParamSubopcode = 0x01;
inline constexpr uint8_t kLEMultiAdvtSetAdvtDataSubopcode = 0x02;
inline constexpr uint8_t kLEMultiAdvtSetScanRespSubopcode = 0x03;
inline constexpr uint8_t kLEMultiAdvtSetRandomAddrSubopcode = 0x4;
inline constexpr uint8_t kLEMultiAdvtEnableSubopcode = 0x05;
inline constexpr EventCode kLEMultiAdvtStateChangeSubeventCode = 0x55;

// ============================================================================
// Batch Scanning

// The kLEBatchScanning opcode is shared across all batch scanning HCI
// commands. To differentiate between the multiple commands, a subopcode field
// is included in the command payload.
inline constexpr OpCode kLEBatchScan = VendorOpCode(0x156);
inline constexpr uint8_t kLEBatchScanEnableSubopcode = 0x01;
inline constexpr uint8_t kLEBatchScanSetStorageParametersSubopcode = 0x02;
inline constexpr uint8_t kLEBatchScanSetScanParametersSubopcode = 0x03;
inline constexpr uint8_t kLEBatchScanReadResultParametersSubopcode = 0x04;
inline constexpr EventCode kStorageThresholdBreachSubeventCode = 0x54;

// ============================================================================
// Advertising Packet Content Filtering

// The kLEApcf opcode is shared across all advertising packet content filtering
// HCI commands. To differentiate between the multiple commands, a subopcode
// field is included in the command payload. These subopcode fields must be set
// to a specific value.
inline constexpr OpCode kLEApcf = VendorOpCode(0x157);
inline constexpr uint8_t kLEApcfEnableSubopcode = 0x00;
inline constexpr uint8_t kLEApcfSetFilteringParametersSubopcode = 0x01;
inline constexpr uint8_t kLEApcfBroadcastAddressSubopcode = 0x02;
inline constexpr uint8_t kLEApcfServiceUUIDSubopcode = 0x03;
inline constexpr uint8_t kLEApcfServiceSolicitationUUIDSubopcode = 0x04;
inline constexpr uint8_t kLEApcfLocalNameSubopcode = 0x05;
inline constexpr uint8_t kLEApcfManufacturerDataSubopcode = 0x06;
inline constexpr uint8_t kLEApcfServiceDataSubopcode = 0x07;
inline constexpr uint8_t kLEApcfTransportDiscoveryService = 0x08;
inline constexpr uint8_t kLEApcfAdTypeFilter = 0x09;
inline constexpr uint8_t kLEApcfReadExtendedFeatures = 0xFF;

// The maximum length of an advertising data field. The value 29 is selected
// because advertising data can be between 0 - 31 bytes wide.
//
//    - Byte 0: length of the advertising data itself.
//    - Byte 1: advertising data type (e.g. 0xFF for manufacturer data)
//
// With two bytes used, the rest of the payload size can only take up a maximum
// of 29 bytes.
inline constexpr uint8_t kLEApcfMaxPDUValueLength = 29;

}  // namespace bt::hci_spec::vendor::android
