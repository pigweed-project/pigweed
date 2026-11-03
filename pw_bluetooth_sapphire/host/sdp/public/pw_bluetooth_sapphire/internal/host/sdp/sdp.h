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
#include <list>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "pw_bluetooth_sapphire/internal/host/common/uuid.h"
#include "pw_bluetooth_sapphire/internal/host/sdp/data_element.h"

namespace bt::sdp {

// Each service has a service record handle that uniquely identifies that
// service within an SDP server.
using ServiceHandle = uint32_t;

// Handle to service record representing SDP itself. (Vol 3, Part B, 2.2)
inline constexpr ServiceHandle kSDPHandle = 0x00000000;
// Handles 0x0000001 - 0x0000FFFF are reserved (Vol 3, Part B, 5.1.1)
inline constexpr ServiceHandle kFirstUnreservedHandle = 0x00010000;
inline constexpr ServiceHandle kLastHandle = 0xFFFFFFFF;

// Vol 3, Part B, 2.3.1
using AttributeId = uint16_t;

// Referred to as "PDU ID" in the spec.
using OpCode = uint8_t;

using TransactionId = uint16_t;

// Header for each SDP PDU.
// v5.0, Vol 3, Part B, 4.2
struct Header {
  OpCode pdu_id;
  TransactionId tid;
  uint16_t param_length;
} __attribute__((packed));

// v5.0, Vol 3, Part B, 4.4.1
enum class ErrorCode : uint16_t {
  kUnsupportedVersion = 0x0001,
  kInvalidRecordHandle = 0x0002,
  kInvalidRequestSyntax = 0x0003,
  kInvalidSize = 0x0004,
  kInvalidContinuationState = 0x0005,
  kInsufficientResources = 0x0006,
};

// ===== SDP PDUs =====
inline constexpr OpCode kReserved = 0x00;

// Error Handling
inline constexpr OpCode kErrorResponse = 0x01;

// Service Search Transaction
inline constexpr OpCode kServiceSearchRequest = 0x02;
inline constexpr OpCode kServiceSearchResponse = 0x03;

// Service Attribute Transaction
inline constexpr OpCode kServiceAttributeRequest = 0x04;
inline constexpr OpCode kServiceAttributeResponse = 0x05;

// Service Search Attribute Transaction
inline constexpr OpCode kServiceSearchAttributeRequest = 0x06;
inline constexpr OpCode kServiceSearchAttributeResponse = 0x07;

// ====== SDP Protocol UUIDs ======
// Defined in the Bluetooth Assigned Numbers:
// https://www.bluetooth.com/specifications/assigned-numbers/service-discovery

namespace protocol {

inline constexpr UUID kSDP(uint16_t{0x0001});
inline constexpr UUID kRFCOMM(uint16_t{0x0003});
inline constexpr UUID kATT(uint16_t{0x0007});
inline constexpr UUID kOBEX(uint16_t{0x0008});  // IrDA Interop
inline constexpr UUID kBNEP(uint16_t{0x000F});
inline constexpr UUID kHIDP(uint16_t{0x0011});
inline constexpr UUID kAVCTP(uint16_t{0x0017});
inline constexpr UUID kAVDTP(uint16_t{0x0019});
inline constexpr UUID kL2CAP(uint16_t{0x0100});

}  // namespace protocol

// ====== SDP Profile / Class UUIDs =====
// Defined in the Bluetooth Assigned Numbers:
// https://www.bluetooth.com/specifications/assigned-numbers/service-discovery

namespace profile {

// Service Discovery Profile (SDP)
inline constexpr UUID kServiceDiscoveryClass(uint16_t{0x1000});
inline constexpr UUID kBrowseGroupClass(uint16_t{0x1001});
// Serial Port Profile (SPP)
inline constexpr UUID kSerialPort(uint16_t{0x1101});
// Dial-up Networking Profile (DUN)
inline constexpr UUID kDialupNetworking(uint16_t{0x1103});
// Object Push Profile (OPP)
inline constexpr UUID kObexObjectPush(uint16_t{0x1105});
// File Transfer Profile (FTP)
inline constexpr UUID kObexFileTransfer(uint16_t{0x1106});
// Headset Profile (HSP)
inline constexpr UUID kHeadset(uint16_t{0x1108});
inline constexpr UUID kHeadsetAudioGateway(uint16_t{0x1112});
inline constexpr UUID kHeadsetHS(uint16_t{0x1131});
// Advanced Audio Distribution Profile (A2DP)
inline constexpr UUID kAudioSource(uint16_t{0x110A});
inline constexpr UUID kAudioSink(uint16_t{0x110B});
inline constexpr UUID kAdvancedAudioDistribution(uint16_t{0x110D});
// Audio/Video Remote Control Profile (AVRCP)
inline constexpr UUID kAVRemoteControlTarget(uint16_t{0x110C});
inline constexpr UUID kAVRemoteControl(uint16_t{0x110E});
inline constexpr UUID kAVRemoteControlController(uint16_t{0x110F});
// Personal Area Networking (PAN)
inline constexpr UUID kPANU(uint16_t{0x1115});
inline constexpr UUID kNAP(uint16_t{0x1116});
inline constexpr UUID kGN(uint16_t{0x1117});
// Basic Printing and Basic Imaging Profiles omitted (unsupported)
// Hands-Free Profile (HFP)
inline constexpr UUID kHandsfree(uint16_t{0x111E});
inline constexpr UUID kHandsfreeAudioGateway(uint16_t{0x111F});
// Human Interface Device omitted (unsupported)
// Hardcopy Cable Replacement Profile omitted (unsupported)
// Sim Access Profile (SAP)
inline constexpr UUID kSIM_Access(uint16_t{0x112D});
// Phonebook Access Profile (PBAP)
inline constexpr UUID kPhonebookPCE(uint16_t{0x112E});
inline constexpr UUID kPhonebookPSE(uint16_t{0x112F});
inline constexpr UUID kPhonebook(uint16_t{0x1130});
// Message Access Profile (MAP)
inline constexpr UUID kMessageAccessServer(uint16_t{0x1132});
inline constexpr UUID kMessageNotificationServer(uint16_t{0x1133});
inline constexpr UUID kMessageAccessProfile(uint16_t{0x1134});
// GNSS and 3DSP omitted (unsupported)
// Multi-Profile Specification (MPS)
inline constexpr UUID kMPSProfile(uint16_t{0x113A});
inline constexpr UUID kMPSClass(uint16_t{0x113B});
// Calendar, Task, and Notes Profile omitted (unsupported)
// Device ID
inline constexpr UUID kPeerIdentification(uint16_t{0x1200});
// Video Distribution Profile (VDP)
inline constexpr UUID kVideoSource(uint16_t{0x1303});
inline constexpr UUID kVideoSink(uint16_t{0x1304});
inline constexpr UUID kVideoDistribution(uint16_t{0x1305});
// Health Device Profile (HDP)
inline constexpr UUID kHDP(uint16_t{0x1400});
inline constexpr UUID kHDPSource(uint16_t{0x1401});
inline constexpr UUID kHDPSink(uint16_t{0x1402});

}  // namespace profile

// ====== SDP Attribute IDs ======

// ====== Universal Attribute Definitions =====
// v5.0, Vol 3, Part B, Sec 5.1

// Service Record Handle
inline constexpr AttributeId kServiceRecordHandle = 0x0000;

using ServiceRecordHandleValueType = uint32_t;

// Service Class ID List
inline constexpr AttributeId kServiceClassIdList = 0x0001;

// A sequence of UUIDs.
// Must contain at least one UUID.
using ServiceClassIdListValueType = std::vector<DataElement>;

// Service Record State
// Used to facilitate caching. If any part of the service record changes,
// this value must change.
inline constexpr AttributeId kServiceRecordState = 0x0002;

using ServiceRecordStateValueType = uint32_t;

// Service ID
inline constexpr AttributeId kServiceId = 0x0003;

using ServiceIdValueType = UUID;

// Protocol Descriptor List
inline constexpr AttributeId kProtocolDescriptorList = 0x0004;

// This is a list of DataElementSequences, of which each has as its first
// element a Protocol UUID, followed by protocol-specific parameters.
// See v5.0, Vol 3, Part B, Sec 5.1.5
using ProtocolDescriptorListValueType = std::vector<DataElement>;

// AdditionalProtocolDescriptorList
inline constexpr AttributeId kAdditionalProtocolDescriptorList = 0x000D;

// This is a sequence of Protocol Descriptor Lists
using AdditionalProtocolDescriptorListValueType = std::vector<DataElement>;

// Browse Group List
// Browse Group lists are described in v5.0, Vol 3, Part B, Sec 2.6
inline constexpr AttributeId kBrowseGroupList = 0x0005;

// This is a sequence which is composed of UUIDs of the groups that this
// service belongs to.
using BrowseGroupListValueType = std::vector<DataElement>;

// The UUID used for the root of the browsing hierarchy
inline constexpr UUID kPublicBrowseRootUuid(uint16_t{0x1002});

// Language Base Attribute Id List
inline constexpr AttributeId kLanguageBaseAttributeIdList = 0x0006;

// A sequence of uint16_t triplets containing:
//  - An identifier for a natural language from ISO 639:1988 (E/F)
//  - A character encoding to use for the language (UTF-8 is 106)
//  - An attribute ID as the base attribute for offset for human-readable
//    information about the service.
using LanguageBaseAttributeIdListValueType = std::vector<DataElement>;

// Service Info TTL
// Number of seconds that the service record is expected to be valid and
// unchanged.
inline constexpr AttributeId kServiceInfoTimeToLive = 0x0007;

using ServiceInfoTimeToLiveValueType = uint32_t;

// Service Availability
// Represents the relative ability of the service to accept additional clients.
// 0x00 means no clients can connect, 0xFF means no one is using it.
// See Vol 3, Part B, 5.1.10
inline constexpr AttributeId kServiceAvailability = 0x0008;

using ServiceAvailabilityValueType = uint8_t;

// Bluetooth Profile Descriptor List
inline constexpr AttributeId kBluetoothProfileDescriptorList = 0x0009;

// A Sequence of Sequences with:
//  - a UUID for a profile
//  - A 16-bit unsigned version number with:
//     - 8 MSbits major version
//     - 8 LSbits minor version
using BluetoothProfileDescriptorListValueType = std::vector<DataElement>;

// TODO(fxbug.dev/42078669): Documentation URL, ClientExecutableURL, IconURL
// When we support the URL type.

// ##### Language Attribute Offsets #####
// These must be added to the attribute ID retrieved from the
// LanguageBaseAttributeIdList

// Service Name
inline constexpr AttributeId kServiceNameOffset = 0x0000;

using ServiceNameValueType = std::string;

// Service Description
inline constexpr AttributeId kServiceDescriptionOffset = 0x0001;

using ServiceDescriptionValueType = std::string;

// Provider Name
inline constexpr AttributeId kProviderNameOffset = 0x0002;

using ProviderNameValueType = std::string;

// ===== ServiceDiscoveryServer Service Class Attribute Definitions ======
// These attributes are defined as valid for the ServiceDiscoveryServer.
// See Spec v5.0, Vol 3, Part B, Section 5.2

// VersionNumberList is a list of the versions supported by the SDP server.
// See v5.0, Vol 3, Part B, Section 5.2.3
inline constexpr AttributeId kSDP_VersionNumberList = 0x0200;

using SDP_VersionNumberListType = std::vector<DataElement>;

// ServiceDatabaseState is a 32-bit integer that is changed whenever any other
// service records are added or deleted from the database.
inline constexpr AttributeId kSDP_ServiceDatabaseState = 0x0201;

// ===== Advanced Audio Distribution Profile Attribute Definitions ======
// These attributes are defined as valid for the AudioSource and AudioSink
// Service Class UUIDs in the Assigned Numbers for SDP
// https://www.bluetooth.com/specifications/assigned-numbers/service-discovery
inline constexpr AttributeId kA2DP_SupportedFeatures = 0x0311;

// ===== OBEX Protocol Attribute Definitions =====
// This attribute is defined on a per-profile basis, and is the same for all
// relevant profiles that require OBEX. See
// https://www.bluetooth.com/specifications/assigned-numbers/service-discovery
inline constexpr AttributeId kGoepL2capPsm = 0x0200;

}  // namespace bt::sdp
