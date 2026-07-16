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
#include <pw_bluetooth/hci_common.emb.h>
#include <pw_bluetooth/hci_events.emb.h>

#include <array>
#include <cstdint>

#include "pw_bluetooth_sapphire/internal/host/common/device_address.h"
#include "pw_bluetooth_sapphire/internal/host/common/device_class.h"
#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_bluetooth_sapphire/internal/host/common/uint128.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"

// This file contains general opcode/number and static packet definitions for
// the Bluetooth Host-Controller Interface. Each packet payload structure
// contains parameter descriptions based on their respective documentation in
// the Bluetooth Core Specification version 5.0
//
// NOTE: Avoid casting raw buffer pointers to the packet payload structure types
// below; use as template parameter to PacketView::payload(), or
// MutableBufferView::mutable_payload() instead. Take extra care when accessing
// flexible array members.

namespace bt::hci_spec {

using pw::bluetooth::emboss::ConnectionRole;
using pw::bluetooth::emboss::GenericEnableParam;
using pw::bluetooth::emboss::StatusCode;

// HCI opcode as used in command packets.
using OpCode = uint16_t;

// HCI event code as used in event packets.
using EventCode = uint8_t;

// Data Connection Handle used for ACL and SCO logical link connections.
using ConnectionHandle = uint16_t;

// Handle used to identify a periodic advertising sync.
using SyncHandle = uint16_t;

// Handle used to identify an advertising set used in the 5.0 Extended
// Advertising feature.
using AdvertisingHandle = uint8_t;

// Handle used to identify a periodic advertiser used in the 5.0 Periodic
// Advertising feature.
using PeriodicAdvertiserHandle = uint16_t;

// Uniquely identifies a CIG (Connected Isochronous Group) in the context of an
// LE connection.
using CigIdentifier = uint8_t;

// Uniquely identifies a CIS (Connected Isochronous Stream) in the context of a
// CIG and an LE connection.
using CisIdentifier = uint8_t;

// Returns the OGF (OpCode Group Field) which occupies the upper 6-bits of the
// opcode.
inline uint8_t GetOGF(const OpCode opcode) { return opcode >> 10; }

// Returns the OCF (OpCode Command Field) which occupies the lower 10-bits of
// the opcode.
inline uint16_t GetOCF(const OpCode opcode) { return opcode & 0x3FF; }

// Returns the opcode based on the given OGF and OCF fields.
constexpr OpCode DefineOpCode(const uint8_t ogf, const uint16_t ocf) {
  return static_cast<uint16_t>(((ogf & 0x3F) << 10) | (ocf & 0x03FF));
}

// ========================= HCI packet headers ==========================
// NOTE(armansito): The definitions below are incomplete since they get added as
// needed. This list will grow as we support more features.

struct CommandHeader {
  uint16_t opcode;
  uint8_t parameter_total_size;
} __attribute__((packed));

struct ACLDataHeader {
  // The first 16-bits contain the following fields, in order:
  //   - 12-bits: Connection Handle
  //   - 2-bits: Packet Boundary Flags
  //   - 2-bits: Broadcast Flags
  uint16_t handle_and_flags;

  // Length of data following the header.
  uint16_t data_total_length;
} __attribute__((packed));

struct SynchronousDataHeader {
  // The first 16-bits contain the following fields, in order:
  // - 12-bits: Connection Handle
  // - 2-bits: Packet Status Flag
  // - 2-bits: RFU
  uint16_t handle_and_flags;

  // Length of the data following the header.
  uint8_t data_total_length;
} __attribute__((packed));

// ============= HCI Command and Event (op)code and payloads =============

// No-Op
inline constexpr OpCode kNoOp = 0x0000;

// The following is a list of HCI command and event declarations sorted by OGF
// category. Within each category the commands are sorted by their OCF. Each
// declaration is preceded by the name of the command or event followed by the
// Bluetooth Core Specification version in which it was introduced. Commands
// that apply to a specific Bluetooth sub-technology
// (e.g. BR/EDR, LE, AMP) will also contain that definition.
//
// NOTE(armansito): This list is incomplete. Entries will be added as needed.

// ======= Link Control Commands =======
// Core Spec v5.0, Vol 2, Part E, Section 7.1
inline constexpr uint8_t kLinkControlOGF = 0x01;
constexpr OpCode LinkControlOpCode(const uint16_t ocf) {
  return DefineOpCode(kLinkControlOGF, ocf);
}

// ===============================
// Inquiry Command (v1.1) (BR/EDR)
inline constexpr OpCode kInquiry = LinkControlOpCode(0x0001);

// ======================================
// Inquiry Cancel Command (v1.1) (BR/EDR)
inline constexpr OpCode kInquiryCancel = LinkControlOpCode(0x0002);

// Inquiry Cancel Command has no command parameters.

// =================================
// Create Connection (v1.1) (BR/EDR)
inline constexpr OpCode kCreateConnection = LinkControlOpCode(0x0005);

// =======================================
// Disconnect Command (v1.1) (BR/EDR & LE)
inline constexpr OpCode kDisconnect = LinkControlOpCode(0x0006);

// ========================================
// Create Connection Cancel (v1.1) (BR/EDR)
inline constexpr OpCode kCreateConnectionCancel = LinkControlOpCode(0x0008);

// =========================================
// Accept Connection Request (v1.1) (BR/EDR)
inline constexpr OpCode kAcceptConnectionRequest = LinkControlOpCode(0x0009);

// =========================================
// Reject Connection Request (v1.1) (BR/EDR)
inline constexpr OpCode kRejectConnectionRequest = LinkControlOpCode(0x000A);

// ==============================================
// Link Key Request Reply Command (v1.1) (BR/EDR)
inline constexpr OpCode kLinkKeyRequestReply = LinkControlOpCode(0x000B);

inline constexpr size_t kBrEdrLinkKeySize = 16;

// =======================================================
// Link Key Request Negative Reply Command (v1.1) (BR/EDR)
inline constexpr OpCode kLinkKeyRequestNegativeReply =
    LinkControlOpCode(0x000C);

// =======================================================
// PIN Code Request Reply Command (v1.1) (BR/EDR)
inline constexpr OpCode kPinCodeRequestReply = LinkControlOpCode(0x000D);

// =======================================================
// PIN Code Request Negative Reply Command (v1.1) (BR/EDR)
inline constexpr OpCode kPinCodeRequestNegativeReply =
    LinkControlOpCode(0x000E);

// ================================================
// Authentication Requested Command (v1.1) (BR/EDR)
inline constexpr OpCode kAuthenticationRequested = LinkControlOpCode(0x0011);

// =================================================
// Set Connection Encryption Command (v1.1) (BR/EDR)
inline constexpr OpCode kSetConnectionEncryption = LinkControlOpCode(0x0013);

// ============================================================
// Remote Name Request Command (v1.1) (BR/EDR)
inline constexpr OpCode kRemoteNameRequest = LinkControlOpCode(0x0019);

// ======================================================
// Read Remote Supported Features Command (v1.1) (BR/EDR)
inline constexpr OpCode kReadRemoteSupportedFeatures =
    LinkControlOpCode(0x001B);

// =====================================================
// Read Remote Extended Features Command (v1.2) (BR/EDR)
inline constexpr OpCode kReadRemoteExtendedFeatures = LinkControlOpCode(0x001C);

// ============================================================
// Read Remote Version Information Command (v1.1) (BR/EDR & LE)
inline constexpr OpCode kReadRemoteVersionInfo = LinkControlOpCode(0x001D);

// =============================================
// Reject Synchronous Connection Command (BR/EDR)
inline constexpr OpCode kRejectSynchronousConnectionRequest =
    LinkControlOpCode(0x002A);

// =========================================================
// IO Capability Request Reply Command (v2.1 + EDR) (BR/EDR)
inline constexpr OpCode kIOCapabilityRequestReply = LinkControlOpCode(0x002B);

// =============================================================
// User Confirmation Request Reply Command (v2.1 + EDR) (BR/EDR)
inline constexpr OpCode kUserConfirmationRequestReply =
    LinkControlOpCode(0x002C);

// ======================================================================
// User Confirmation Request Negative Reply Command (v2.1 + EDR) (BR/EDR)
inline constexpr OpCode kUserConfirmationRequestNegativeReply =
    LinkControlOpCode(0x002D);

// ========================================================
// User Passkey Request Reply Command (v2.1 + EDR) (BR/EDR)
inline constexpr OpCode kUserPasskeyRequestReply = LinkControlOpCode(0x002E);

// =================================================================
// User Passkey Request Negative Reply Command (v2.1 + EDR) (BR/EDR)
inline constexpr OpCode kUserPasskeyRequestNegativeReply =
    LinkControlOpCode(0x002F);

// ==================================================================
// IO Capability Request Negative Reply Command (v2.1 + EDR) (BR/EDR)
inline constexpr OpCode kIOCapabilityRequestNegativeReply =
    LinkControlOpCode(0x0034);

// ======================================================
// Enhanced Setup Synchronous Connection Command (BR/EDR)
inline constexpr OpCode kEnhancedSetupSynchronousConnection =
    LinkControlOpCode(0x003D);

// ===============================================================
// Enhanced Accept Synchronous Connection Request Command (BR/EDR)
inline constexpr OpCode kEnhancedAcceptSynchronousConnectionRequest =
    LinkControlOpCode(0x003E);

// ======= Link Policy Commands ======
// Core Spec v6.0 Vol 4, Part E, Section 7.2
inline constexpr uint8_t kLinkPolicyOGF = 0x02;
constexpr OpCode LinkPolicyOpCode(const uint16_t ocf) {
  return DefineOpCode(kLinkPolicyOGF, ocf);
}

// =========================
// Sniff Mode Command (v1.1)
inline constexpr OpCode kSniffMode = LinkPolicyOpCode(0x0003);

// ==============================
// Exit Sniff Mode Command (v1.1)
inline constexpr OpCode kExitSniffMode = LinkPolicyOpCode(0x0004);

// =========================================
// Write Default Link Policy Settings (v1.1)
inline constexpr OpCode kWriteDefaultLinkPolicySettings =
    LinkPolicyOpCode(0x000F);

// ======= Controller & Baseband Commands =======
// Core Spec v5.0 Vol 2, Part E, Section 7.3
inline constexpr uint8_t kControllerAndBasebandOGF = 0x03;
constexpr OpCode ControllerAndBasebandOpCode(const uint16_t ocf) {
  return DefineOpCode(kControllerAndBasebandOGF, ocf);
}

// =============================
// Set Event Mask Command (v1.1)
inline constexpr OpCode kSetEventMask = ControllerAndBasebandOpCode(0x0001);

// ====================
// Reset Command (v1.1)
inline constexpr OpCode kReset = ControllerAndBasebandOpCode(0x0003);

// ========================================
// Read PIN Type Command (v1.1) (BR/EDR)
inline constexpr OpCode kReadPinType = ControllerAndBasebandOpCode(0x0009);

// ========================================
// Write PIN Type Command (v1.1) (BR/EDR)
inline constexpr OpCode kWritePinType = ControllerAndBasebandOpCode(0x000A);

// ========================================
// Write Local Name Command (v1.1) (BR/EDR)
inline constexpr OpCode kWriteLocalName = ControllerAndBasebandOpCode(0x0013);

// =======================================
// Read Local Name Command (v1.1) (BR/EDR)
inline constexpr OpCode kReadLocalName = ControllerAndBasebandOpCode(0x0014);

// ==========================================
// Write Page Timeout Command (v1.1) (BR/EDR)
inline constexpr OpCode kWritePageTimeout = ControllerAndBasebandOpCode(0x0018);

// ========================================
// Read Scan Enable Command (v1.1) (BR/EDR)
inline constexpr OpCode kReadScanEnable = ControllerAndBasebandOpCode(0x0019);

// =========================================
// Write Scan Enable Command (v1.1) (BR/EDR)
inline constexpr OpCode kWriteScanEnable = ControllerAndBasebandOpCode(0x001A);

// ===============================================
// Read Page Scan Activity Command (v1.1) (BR/EDR)
inline constexpr OpCode kReadPageScanActivity =
    ControllerAndBasebandOpCode(0x001B);

// ================================================
// Write Page Scan Activity Command (v1.1) (BR/EDR)
inline constexpr OpCode kWritePageScanActivity =
    ControllerAndBasebandOpCode(0x001C);

// ===============================================
// Read Inquiry Scan Activity Command (v1.1) (BR/EDR)
inline constexpr OpCode kReadInquiryScanActivity =
    ControllerAndBasebandOpCode(0x001D);

// ================================================
// Write Inquiry Scan Activity Command (v1.1) (BR/EDR)
inline constexpr OpCode kWriteInquiryScanActivity =
    ControllerAndBasebandOpCode(0x001E);

// ============================================
// Read Class of Device Command (v1.1) (BR/EDR)
inline constexpr OpCode kReadClassOfDevice =
    ControllerAndBasebandOpCode(0x0023);

// =============================================
// Write Class Of Device Command (v1.1) (BR/EDR)
inline constexpr OpCode kWriteClassOfDevice =
    ControllerAndBasebandOpCode(0x0024);

// =============================================
// Write Automatic Flush Timeout Command (v1.1) (BR/EDR)
inline constexpr OpCode kWriteAutomaticFlushTimeout =
    ControllerAndBasebandOpCode(0x0028);

// ===============================================================
// Read Transmit Transmit Power Level Command (v1.1) (BR/EDR & LE)
inline constexpr OpCode kReadTransmitPowerLevel =
    ControllerAndBasebandOpCode(0x002D);

// ===============================================================
// Write Synchonous Flow Control Enable Command (BR/EDR)
inline constexpr OpCode kWriteSynchronousFlowControlEnable =
    ControllerAndBasebandOpCode(0x002F);

// ===============================================================
// Host Buffer Size Command (v1.1)
inline constexpr OpCode kHostBufferSize = ControllerAndBasebandOpCode(0x0033);

// ===================================
// Read Inquiry Scan Type (v1.2) (BR/EDR)
inline constexpr OpCode kReadInquiryScanType =
    ControllerAndBasebandOpCode(0x0042);

// ====================================
// Write Inquiry Scan Type (v1.2) (BR/EDR)
inline constexpr OpCode kWriteInquiryScanType =
    ControllerAndBasebandOpCode(0x0043);

// =================================
// Read Inquiry Mode (v1.2) (BR/EDR)
inline constexpr OpCode kReadInquiryMode = ControllerAndBasebandOpCode(0x0044);

// ==================================
// Write Inquiry Mode (v1.2) (BR/EDR)
inline constexpr OpCode kWriteInquiryMode = ControllerAndBasebandOpCode(0x0045);

// ===================================
// Read Page Scan Type (v1.2) (BR/EDR)
inline constexpr OpCode kReadPageScanType = ControllerAndBasebandOpCode(0x0046);

// ====================================
// Write Page Scan Type (v1.2) (BR/EDR)
inline constexpr OpCode kWritePageScanType =
    ControllerAndBasebandOpCode(0x0047);

// =================================
// Write Extended Inquiry Response (v1.2) (BR/EDR)
inline constexpr OpCode kWriteExtendedInquiryResponse =
    ControllerAndBasebandOpCode(0x0052);

// ==============================================
// Read Simple Pairing Mode (v2.1 + EDR) (BR/EDR)
inline constexpr OpCode kReadSimplePairingMode =
    ControllerAndBasebandOpCode(0x0055);

// ===============================================
// Write Simple Pairing Mode (v2.1 + EDR) (BR/EDR)
inline constexpr OpCode kWriteSimplePairingMode =
    ControllerAndBasebandOpCode(0x0056);

// =========================================
// Set Event Mask Page 2 Command (v3.0 + HS)
inline constexpr OpCode kSetEventMaskPage2 =
    ControllerAndBasebandOpCode(0x0063);

// =========================================================
// Read Flow Control Mode Command (v3.0 + HS) (BR/EDR & AMP)
inline constexpr OpCode kReadFlowControlMode =
    ControllerAndBasebandOpCode(0x0066);

// ==========================================================
// Write Flow Control Mode Command (v3.0 + HS) (BR/EDR & AMP)
inline constexpr OpCode kWriteFlowControlMode =
    ControllerAndBasebandOpCode(0x0067);

// ============================================
// Read LE Host Support Command (v4.0) (BR/EDR)
inline constexpr OpCode kReadLEHostSupport =
    ControllerAndBasebandOpCode(0x006C);

// =============================================
// Write LE Host Support Command (v4.0) (BR/EDR)
inline constexpr OpCode kWriteLEHostSupport =
    ControllerAndBasebandOpCode(0x006D);

// =============================================
// Write Secure Connections Host Support Command (v4.1) (BR/EDR)
inline constexpr OpCode kWriteSecureConnectionsHostSupport =
    ControllerAndBasebandOpCode(0x007A);

// ===============================================================
// Read Authenticated Payload Timeout Command (v4.1) (BR/EDR & LE)
inline constexpr OpCode kReadAuthenticatedPayloadTimeout =
    ControllerAndBasebandOpCode(0x007B);

// ================================================================
// Write Authenticated Payload Timeout Command (v4.1) (BR/EDR & LE)
inline constexpr OpCode kWriteAuthenticatedPayloadTimeout =
    ControllerAndBasebandOpCode(0x007C);

// ======= Informational Parameters =======
// Core Spec v5.0 Vol 2, Part E, Section 7.4
inline constexpr uint8_t kInformationalParamsOGF = 0x04;
constexpr OpCode InformationalParamsOpCode(const uint16_t ocf) {
  return DefineOpCode(kInformationalParamsOGF, ocf);
}

// =============================================
// Read Local Version Information Command (v1.1)
inline constexpr OpCode kReadLocalVersionInfo =
    InformationalParamsOpCode(0x0001);

// ============================================
// Read Local Supported Commands Command (v1.2)
inline constexpr OpCode kReadLocalSupportedCommands =
    InformationalParamsOpCode(0x0002);

// ============================================
// Read Local Supported Features Command (v1.1)
inline constexpr OpCode kReadLocalSupportedFeatures =
    InformationalParamsOpCode(0x0003);

// ====================================================
// Read Local Extended Features Command (v1.2) (BR/EDR)
inline constexpr OpCode kReadLocalExtendedFeatures =
    InformationalParamsOpCode(0x0004);

// ===============================
// Read Buffer Size Command (v1.1)
inline constexpr OpCode kReadBufferSize = InformationalParamsOpCode(0x0005);

// ========================================
// Read BD_ADDR Command (v1.1) (BR/EDR, LE)
inline constexpr OpCode kReadBDADDR = InformationalParamsOpCode(0x0009);

// =======================================================
// Read Data Block Size Command (v3.0 + HS) (BR/EDR & AMP)
inline constexpr OpCode kReadDataBlockSize = InformationalParamsOpCode(0x000A);

// ====================================================
// Read Local Supported Controller Delay Command (v5.2)
inline constexpr OpCode kReadLocalSupportedControllerDelay =
    InformationalParamsOpCode(0x000F);

// ======= Testing Commands =======
// Core Spec v6.1 Vol 4, Part E, Section 7.6
inline constexpr uint8_t kTestingCommandsOGF = 0x06;
constexpr OpCode TestingCommandsOpCode(const uint16_t ocf) {
  return DefineOpCode(kTestingCommandsOGF, ocf);
}

// ====================================================
// Write Simple Pairing Debug Mode Command (v2.1 + EDR)
inline constexpr OpCode kWriteSimplePairingDebugMode =
    TestingCommandsOpCode(0x0004);

// ======= Events =======
// Core Spec v5.0 Vol 2, Part E, Section 7.7

// Reserved for vendor-specific debug events
// (Vol 2, Part E, Section 5.4.4)
inline constexpr EventCode kVendorDebugEventCode = 0xFF;

// ======================================
// Inquiry Complete Event (v1.1) (BR/EDR)
inline constexpr EventCode kInquiryCompleteEventCode = 0x01;

// ====================================
// Inquiry Result Event (v1.1) (BR/EDR)
inline constexpr EventCode kInquiryResultEventCode = 0x02;

// =========================================
// Connection Complete Event (v1.1) (BR/EDR)
inline constexpr EventCode kConnectionCompleteEventCode = 0x03;

// ========================================
// Connection Request Event (v1.1) (BR/EDR)
inline constexpr EventCode kConnectionRequestEventCode = 0x04;

// =================================================
// Disconnection Complete Event (v1.1) (BR/EDR & LE)
inline constexpr EventCode kDisconnectionCompleteEventCode = 0x05;

// =============================================
// Authentication Complete Event (v1.1) (BR/EDR)
inline constexpr EventCode kAuthenticationCompleteEventCode = 0x06;

// ==================================================
// Remote Name Request Complete Event (v1.1) (BR/EDR)
inline constexpr EventCode kRemoteNameRequestCompleteEventCode = 0x07;

// ============================================
// Encryption Change Event (v1.1) (BR/EDR & LE)
inline constexpr EventCode kEncryptionChangeEventCode = 0x08;

// =========================================================
// Change Connection Link Key Complete Event (v1.1) (BR/EDR)
inline constexpr EventCode kChangeConnectionLinkKeyCompleteEventCode = 0x09;

// =============================================================
// Read Remote Supported Features Complete Event (v1.1) (BR/EDR)
inline constexpr EventCode kReadRemoteSupportedFeaturesCompleteEventCode = 0x0B;

// ===================================================================
// Read Remote Version Information Complete Event (v1.1) (BR/EDR & LE)
inline constexpr EventCode kReadRemoteVersionInfoCompleteEventCode = 0x0C;

// =============================
// Command Complete Event (v1.1)
inline constexpr EventCode kCommandCompleteEventCode = 0x0E;

// ===========================
// Command Status Event (v1.1)
inline constexpr EventCode kCommandStatusEventCode = 0x0F;
inline constexpr uint8_t kCommandStatusPending = 0x00;

// ===========================
// Hardware Error Event (v1.1)
inline constexpr EventCode kHardwareErrorEventCode = 0x10;

// ========================================
// Role Change Event (BR/EDR) (v1.1)
inline constexpr EventCode kRoleChangeEventCode = 0x12;

// ========================================
// Number Of Completed Packets Event (v1.1)
inline constexpr EventCode kNumberOfCompletedPacketsEventCode = 0x13;

// ========================
// Mode Change Event (v1.1)
inline constexpr EventCode kModeChangeEventCode = 0x14;

// ======================================
// PIN Code Request Event (v1.1) (BR/EDR)
inline constexpr EventCode kPinCodeRequestEventCode = 0x16;

// ======================================
// Link Key Request Event (v1.1) (BR/EDR)
inline constexpr EventCode kLinkKeyRequestEventCode = 0x17;

// ===========================================
// Link Key Notification Event (v1.1) (BR/EDR)
inline constexpr EventCode kLinkKeyNotificationEventCode = 0x18;

// ===========================================
// Data Buffer Overflow Event (v1.1) (BR/EDR & LE)
inline constexpr EventCode kDataBufferOverflowEventCode = 0x1A;

// ==============================================
// Inquiry Result with RSSI Event (v1.2) (BR/EDR)
inline constexpr EventCode kInquiryResultWithRSSIEventCode = 0x22;

// ============================================================
// Read Remote Extended Features Complete Event (v1.1) (BR/EDR)
inline constexpr EventCode kReadRemoteExtendedFeaturesCompleteEventCode = 0x23;

// ============================================================
// Synchronous Connection Complete Event (BR/EDR)
inline constexpr EventCode kSynchronousConnectionCompleteEventCode = 0x2C;

// =============================================
// Extended Inquiry Result Event (v1.2) (BR/EDR)
inline constexpr EventCode kExtendedInquiryResultEventCode = 0x2F;

// ================================================================
// Encryption Key Refresh Complete Event (v2.1 + EDR) (BR/EDR & LE)
inline constexpr EventCode kEncryptionKeyRefreshCompleteEventCode = 0x30;

// =================================================
// IO Capability Request Event (v2.1 + EDR) (BR/EDR)
inline constexpr EventCode kIOCapabilityRequestEventCode = 0x31;

// ==================================================
// IO Capability Response Event (v2.1 + EDR) (BR/EDR)
inline constexpr EventCode kIOCapabilityResponseEventCode = 0x32;

// =====================================================
// User Confirmation Request Event (v2.1 + EDR) (BR/EDR)
inline constexpr EventCode kUserConfirmationRequestEventCode = 0x33;

// ================================================
// User Passkey Request Event (v2.1 + EDR) (BR/EDR)
inline constexpr EventCode kUserPasskeyRequestEventCode = 0x34;

// ===================================================
// Simple Pairing Complete Event (v2.1 + EDR) (BR/EDR)
inline constexpr EventCode kSimplePairingCompleteEventCode = 0x36;

// =====================================================
// User Passkey Notification Event (v2.1 + EDR) (BR/EDR)
inline constexpr EventCode kUserPasskeyNotificationEventCode = 0x3B;

// =========================
// LE Meta Event (v4.0) (LE)
inline constexpr EventCode kLEMetaEventCode = 0x3E;

// LE Connection Complete Event (v4.0) (LE)
inline constexpr EventCode kLEConnectionCompleteSubeventCode = 0x01;

// LE Advertising Report Event (v4.0) (LE)
inline constexpr EventCode kLEAdvertisingReportSubeventCode = 0x02;

// LE Connection Update Complete Event (v4.0) (LE)
inline constexpr EventCode kLEConnectionUpdateCompleteSubeventCode = 0x03;

// LE Read Remote Features Complete Event (v4.0) (LE)
inline constexpr EventCode kLEReadRemoteFeaturesCompleteSubeventCode = 0x04;

// LE Long Term Key Request Event (v4.0) (LE)
inline constexpr EventCode kLELongTermKeyRequestSubeventCode = 0x05;

// LE Remote Connection Parameter Request Event (v4.1) (LE)
inline constexpr EventCode kLERemoteConnectionParameterRequestSubeventCode =
    0x06;

// LE Data Length Change Event (v4.2) (LE)
inline constexpr EventCode kLEDataLengthChangeSubeventCode = 0x07;

// LE Read Local P-256 Public Key Complete Event (v4.2) (LE)
inline constexpr EventCode kLEReadLocalP256PublicKeyCompleteSubeventCode = 0x08;

// LE Generate DHKey Complete Event (v4.2) (LE)
inline constexpr EventCode kLEGenerateDHKeyCompleteSubeventCode = 0x09;

// LE Enhanced Connection Complete Event (v4.2) (LE)
inline constexpr EventCode kLEEnhancedConnectionCompleteSubeventCode = 0x0A;

// LE Directed Advertising Report Event (v4.2) (LE)
inline constexpr EventCode kLEDirectedAdvertisingReportSubeventCode = 0x0B;

// LE PHY Update Complete Event (v5.0) (LE)
inline constexpr EventCode kLEPHYUpdateCompleteSubeventCode = 0x0C;

// LE Extended Advertising Report Event (v5.0) (LE)
inline constexpr EventCode kLEExtendedAdvertisingReportSubeventCode = 0x0D;

// LE Periodic Advertising Sync Established Event (v5.0) (LE)
inline constexpr EventCode kLEPeriodicAdvertisingSyncEstablishedSubeventCode =
    0x0E;

// LE Periodic Advertising Report Event (v5.0) (LE)
inline constexpr EventCode kLEPeriodicAdvertisingReportSubeventCode = 0x0F;

// LE Periodic Advertising Sync Lost Event (v5.0) (LE)
inline constexpr EventCode kLEPeriodicAdvertisingSyncLostSubeventCode = 0x10;

// LE Scan Timeout Event (v5.0) (LE)
inline constexpr EventCode kLEScanTimeoutSubeventCode = 0x11;

// LE Advertising Set Terminated Event (v5.0) (LE)
inline constexpr EventCode kLEAdvertisingSetTerminatedSubeventCode = 0x012;

// LE Scan Request Received Event (v5.0) (LE)
inline constexpr EventCode kLEScanRequestReceivedSubeventCode = 0x13;

// LE Channel Selection Algorithm Event (v5.0) (LE)
inline constexpr EventCode kLEChannelSelectionAlgorithmSubeventCode = 0x014;

// LE Request Peer SCA Complete Event (v5.2) (LE)
inline constexpr EventCode kLERequestPeerSCACompleteSubeventCode = 0x1F;

// LE CIS Established Event (v5.2) (LE)
inline constexpr EventCode kLECISEstablishedSubeventCode = 0x019;

// LE CIS Request Event (v5.2) (LE)
inline constexpr EventCode kLECISRequestSubeventCode = 0x01A;

// ================================================================
// Number Of Completed Data Blocks Event (v3.0 + HS) (BR/EDR & AMP)
inline constexpr EventCode kNumberOfCompletedDataBlocksEventCode = 0x48;

// ================================================================
// Authenticated Payload Timeout Expired Event (v4.1) (BR/EDR & LE)
inline constexpr EventCode kAuthenticatedPayloadTimeoutExpiredEventCode = 0x57;

// ======= Status Parameters =======
// Core Spec v5.0, Vol 2, Part E, Section 7.5
inline constexpr uint8_t kStatusParamsOGF = 0x05;
constexpr OpCode StatusParamsOpCode(const uint16_t ocf) {
  return DefineOpCode(kStatusParamsOGF, ocf);
}

// ========================
// Read RSSI Command (v1.1)
inline constexpr OpCode kReadRSSI = StatusParamsOpCode(0x0005);

// ========================================
// Read Encryption Key Size (v1.1) (BR/EDR)
inline constexpr OpCode kReadEncryptionKeySize = StatusParamsOpCode(0x0008);

// ======= LE Controller Commands =======
// Core Spec v5.0 Vol 2, Part E, Section 7.8
inline constexpr uint8_t kLEControllerCommandsOGF = 0x08;
constexpr OpCode LEControllerCommandOpCode(const uint16_t ocf) {
  return DefineOpCode(kLEControllerCommandsOGF, ocf);
}

// Returns true if the given |opcode| corresponds to a LE controller command.
inline bool IsLECommand(OpCode opcode) {
  return GetOGF(opcode) == kLEControllerCommandsOGF;
}

// =====================================
// LE Set Event Mask Command (v4.0) (LE)
inline constexpr OpCode kLESetEventMask = LEControllerCommandOpCode(0x0001);

// =======================================
// LE Read Buffer Size [v1] Command (v4.0) (LE)
inline constexpr OpCode kLEReadBufferSizeV1 = LEControllerCommandOpCode(0x0002);

// ====================================================
// LE Read Local Supported Features Command (v4.0) (LE)
inline constexpr OpCode kLEReadLocalSupportedFeatures =
    LEControllerCommandOpCode(0x0003);

// =========================================
// LE Set Random Address Command (v4.0) (LE)
inline constexpr OpCode kLESetRandomAddress = LEControllerCommandOpCode(0x0005);

// =================================================
// LE Set Advertising Parameters Command (v4.0) (LE)
inline constexpr OpCode kLESetAdvertisingParameters =
    LEControllerCommandOpCode(0x0006);

// ========================================================
// LE Read Advertising Channel Tx Power Command (v4.0) (LE)
inline constexpr OpCode kLEReadAdvertisingChannelTxPower =
    LEControllerCommandOpCode(0x0007);

// ===========================================
// LE Set Advertising Data Command (v4.0) (LE)
inline constexpr OpCode kLESetAdvertisingData =
    LEControllerCommandOpCode(0x0008);

// =============================================
// LE Set Scan Response Data Command (v4.0) (LE)
inline constexpr OpCode kLESetScanResponseData =
    LEControllerCommandOpCode(0x0009);

// =============================================
// LE Set Advertising Enable Command (v4.0) (LE)
inline constexpr OpCode kLESetAdvertisingEnable =
    LEControllerCommandOpCode(0x000A);

// ==========================================
// LE Set Scan Parameters Command (v4.0) (LE)
inline constexpr OpCode kLESetScanParameters =
    LEControllerCommandOpCode(0x000B);

// ======================================
// LE Set Scan Enable Command (v4.0) (LE)
inline constexpr OpCode kLESetScanEnable = LEControllerCommandOpCode(0x000C);

// ========================================
// LE Create Connection Command (v4.0) (LE)
inline constexpr OpCode kLECreateConnection = LEControllerCommandOpCode(0x000D);

// ===============================================
// LE Create Connection Cancel Command (v4.0) (LE)
inline constexpr OpCode kLECreateConnectionCancel =
    LEControllerCommandOpCode(0x000E);

// ===========================================
// LE Read Filter Accept List Size Command (v4.0) (LE)
inline constexpr OpCode kLEReadFilterAcceptListSize =
    LEControllerCommandOpCode(0x000F);

// =======================================
// LE Clear Filter Accept List Command (v4.0) (LE)
inline constexpr OpCode kLEClearFilterAcceptList =
    LEControllerCommandOpCode(0x0010);

// ===============================================
// LE Add Device To Filter Accept List Command (v4.0) (LE)
inline constexpr OpCode kLEAddDeviceToFilterAcceptList =
    LEControllerCommandOpCode(0x0011);

// ====================================================
// LE Remove Device From Filter Accept List Command (v4.0) (LE)
inline constexpr OpCode kLERemoveDeviceFromFilterAcceptList =
    LEControllerCommandOpCode(0x0012);

// ========================================
// LE Connection Update Command (v4.0) (LE)
inline constexpr OpCode kLEConnectionUpdate = LEControllerCommandOpCode(0x0013);

// ======================================================
// LE Set Host Channel Classification Command (v4.0) (LE)
inline constexpr OpCode kLESetHostChannelClassification =
    LEControllerCommandOpCode(0x0014);

// =======================================
// LE Read Channel Map Command (v4.0) (LE)
inline constexpr OpCode kLEReadChannelMap = LEControllerCommandOpCode(0x0015);

// ===========================================
// LE Read Remote Features Command (v4.0) (LE)
inline constexpr OpCode kLEReadRemoteFeatures =
    LEControllerCommandOpCode(0x0016);

// ==============================
// LE Encrypt Command (v4.0) (LE)
inline constexpr OpCode kLEEncrypt = LEControllerCommandOpCode(0x0017);

// ===========================
// LE Rand Command (v4.0) (LE)
inline constexpr OpCode kLERand = LEControllerCommandOpCode(0x0018);

// =======================================
// LE Start Encryption Command (v4.0) (LE)
inline constexpr OpCode kLEStartEncryption = LEControllerCommandOpCode(0x0019);

// ==================================================
// LE Long Term Key Request Reply Command (v4.0) (LE)
inline constexpr OpCode kLELongTermKeyRequestReply =
    LEControllerCommandOpCode(0x001A);

// ===========================================================
// LE Long Term Key Request Negative Reply Command (v4.0) (LE)
inline constexpr OpCode kLELongTermKeyRequestNegativeReply =
    LEControllerCommandOpCode(0x001B);

// ============================================
// LE Read Supported States Command (v4.0) (LE)
inline constexpr OpCode kLEReadSupportedStates =
    LEControllerCommandOpCode(0x001C);

// ====================================
// LE Receiver Test Command (v4.0) (LE)
inline constexpr OpCode kLEReceiverTest = LEControllerCommandOpCode(0x001D);

// ======================================
// LE Transmitter Test Command (v4.0) (LE)
inline constexpr OpCode kLETransmitterTest = LEControllerCommandOpCode(0x001E);

// ===============================
// LE Test End Command (v4.0) (LE)
inline constexpr OpCode kLETestEnd = LEControllerCommandOpCode(0x001F);

// ================================================================
// LE Remote Connection Parameter Request Reply Command (v4.1) (LE)
inline constexpr OpCode kLERemoteConnectionParameterRequestReply =
    LEControllerCommandOpCode(0x0020);

// =========================================================================
// LE Remote Connection Parameter Request Negative Reply Command (v4.1) (LE)
inline constexpr OpCode kLERemoteConnectionParameterRequestNegativeReply =
    LEControllerCommandOpCode(0x0021);

// ======================================
// LE Set Data Length Command (v4.2) (LE)
inline constexpr OpCode kLESetDataLength = LEControllerCommandOpCode(0x0022);

// =========================================================
// LE Read Suggested Default Data Length Command (v4.2) (LE)
inline constexpr OpCode kLEReadSuggestedDefaultDataLength =
    LEControllerCommandOpCode(0x0023);

// ==========================================================
// LE Write Suggested Default Data Length Command (v4.2) (LE)
inline constexpr OpCode kLEWriteSuggestedDefaultDataLength =
    LEControllerCommandOpCode(0x0024);

// ==================================================
// LE Read Local P-256 Public Key Command (v4.2) (LE)
inline constexpr OpCode kLEReadLocalP256PublicKey =
    LEControllerCommandOpCode(0x0025);

// ======================================
// LE Generate DH Key Command (v4.2) (LE)
inline constexpr OpCode kLEGenerateDHKey = LEControllerCommandOpCode(0x0026);

// ===================================================
// LE Add Device To Resolving List Command (v4.2) (LE)
inline constexpr OpCode kLEAddDeviceToResolvingList =
    LEControllerCommandOpCode(0x0027);

// ========================================================
// LE Remove Device From Resolving List Command (v4.2) (LE)
inline constexpr OpCode kLERemoveDeviceFromResolvingList =
    LEControllerCommandOpCode(0x0028);

// ===========================================
// LE Clear Resolving List Command (v4.2) (LE)
inline constexpr OpCode kLEClearResolvingList =
    LEControllerCommandOpCode(0x0029);

// ===============================================
// LE Read Resolving List Size Command (v4.2) (LE)
inline constexpr OpCode kLEReadResolvingListSize =
    LEControllerCommandOpCode(0x002A);

// ===================================================
// LE Read Peer Resolvable Address Command (v4.2) (LE)
inline constexpr OpCode kLEReadPeerResolvableAddress =
    LEControllerCommandOpCode(0x002B);

// ====================================================
// LE Read Local Resolvable Address Command (v4.2) (LE)
inline constexpr OpCode kLEReadLocalResolvableAddress =
    LEControllerCommandOpCode(0x002C);

// ====================================================
// LE Set Address Resolution Enable Command (v4.2) (LE)
inline constexpr OpCode kLESetAddressResolutionEnable =
    LEControllerCommandOpCode(0x002D);

// =============================================================
// LE Set Resolvable Private Address Timeout Command (v4.2) (LE)
inline constexpr OpCode kLESetResolvablePrivateAddressTimeout =
    LEControllerCommandOpCode(0x002E);

// ===============================================
// LE Read Maximum Data Length Command (v4.2) (LE)
inline constexpr OpCode kLEReadMaximumDataLength =
    LEControllerCommandOpCode(0x002F);

// ===============================
// LE Read PHY Command (v5.0) (LE)
inline constexpr OpCode kLEReadPHY = LEControllerCommandOpCode(0x0030);

// ======================================
// LE Set Default PHY Command (v5.0) (LE)
inline constexpr OpCode kLESetDefaultPHY = LEControllerCommandOpCode(0x0031);

// ==============================
// LE Set PHY Command (v5.0) (LE)
inline constexpr OpCode kLESetPHY = LEControllerCommandOpCode(0x0032);

// =============================================
// LE Enhanced Receiver Test Command (v5.0) (LE)
inline constexpr OpCode kLEEnhancedReceiverText =
    LEControllerCommandOpCode(0x0033);

// ================================================
// LE Enhanced Transmitter Test Command (v5.0) (LE)
inline constexpr OpCode kLEEnhancedTransmitterTest =
    LEControllerCommandOpCode(0x0034);

// =========================================================
// LE Set Advertising Set Random Address Command (v5.0) (LE)
inline constexpr OpCode kLESetAdvertisingSetRandomAddress =
    LEControllerCommandOpCode(0x0035);

// ==========================================================
// LE Set Extended Advertising Parameters Command (v5.0) (LE)
inline constexpr OpCode kLESetExtendedAdvertisingParameters =
    LEControllerCommandOpCode(0x0036);

// ====================================================
// LE Set Extended Advertising Data Command (v5.0) (LE)
inline constexpr OpCode kLESetExtendedAdvertisingData =
    LEControllerCommandOpCode(0x0037);

// ======================================================
// LE Set Extended Scan Response Data Command (v5.0) (LE)
inline constexpr OpCode kLESetExtendedScanResponseData =
    LEControllerCommandOpCode(0x0038);

// ======================================================
// LE Set Extended Advertising Enable Command (v5.0) (LE)
inline constexpr OpCode kLESetExtendedAdvertisingEnable =
    LEControllerCommandOpCode(0x0039);

// ===========================================================
// LE Read Maximum Advertising Data Length Command (v5.0) (LE)
inline constexpr OpCode kLEReadMaximumAdvertisingDataLength =
    LEControllerCommandOpCode(0x003A);

// ================================================================
// LE Read Number of Supported Advertising Sets Command (v5.0) (LE)
inline constexpr OpCode kLEReadNumSupportedAdvertisingSets =
    LEControllerCommandOpCode(0x003B);

// =============================================
// LE Remove Advertising Set Command (v5.0) (LE)
inline constexpr OpCode kLERemoveAdvertisingSet =
    LEControllerCommandOpCode(0x003C);

// =============================================
// LE Clear Advertising Sets Command (v5.0) (LE)
inline constexpr OpCode kLEClearAdvertisingSets =
    LEControllerCommandOpCode(0x003D);

// ==========================================================
// LE Set Periodic Advertising Parameters Command (v5.0) (LE)
inline constexpr OpCode kLESetPeriodicAdvertisingParameters =
    LEControllerCommandOpCode(0x003E);

// ====================================================
// LE Set Periodic Advertising Data Command (v5.0) (LE)
inline constexpr OpCode kLESetPeriodicAdvertisingData =
    LEControllerCommandOpCode(0x003F);

// ======================================================
// LE Set Periodic Advertising Enable Command (v5.0) (LE)
inline constexpr OpCode kLESetPeriodicAdvertisingEnable =
    LEControllerCommandOpCode(0x0040);

// ===================================================
// LE Set Extended Scan Parameters Command (v5.0) (LE)
inline constexpr OpCode kLESetExtendedScanParameters =
    LEControllerCommandOpCode(0x0041);

// ===============================================
// LE Set Extended Scan Enable Command (v5.0) (LE)
inline constexpr OpCode kLESetExtendedScanEnable =
    LEControllerCommandOpCode(0x0042);

// =================================================
// LE Extended Create Connection Command (v5.0) (LE)
inline constexpr OpCode kLEExtendedCreateConnection =
    LEControllerCommandOpCode(0x0043);

// =======================================================
// LE Periodic Advertising Create Sync Command (v5.0) (LE)
inline constexpr OpCode kLEPeriodicAdvertisingCreateSync =
    LEControllerCommandOpCode(0x0044);

// ==============================================================
// LE Periodic Advertising Create Sync Cancel Command (v5.0) (LE)
inline constexpr OpCode kLEPeriodicAdvertisingCreateSyncCancel =
    LEControllerCommandOpCode(0x0045);

// ==========================================================
// LE Periodic Advertising Terminate Sync Command (v5.0) (LE)
inline constexpr OpCode kLEPeriodicAdvertisingTerminateSync =
    LEControllerCommandOpCode(0x0046);

// =============================================================
// LE Add Device To Periodic Advertiser List Command (v5.0) (LE)
inline constexpr OpCode kLEAddDeviceToPeriodicAdvertiserList =
    LEControllerCommandOpCode(0x0047);

// ==================================================================
// LE Remove Device From Periodic Advertiser List Command (v5.0) (LE)
inline constexpr OpCode kLERemoveDeviceFromPeriodicAdvertiserList =
    LEControllerCommandOpCode(0x0048);

// =====================================================
// LE Clear Periodic Advertiser List Command (v5.0) (LE)
inline constexpr OpCode kLEClearPeriodicAdvertiserList =
    LEControllerCommandOpCode(0x0049);

// =========================================================
// LE Read Periodic Advertiser List Size Command (v5.0) (LE)
inline constexpr OpCode kLEReadPeriodicAdvertiserListSize =
    LEControllerCommandOpCode(0x004A);

// ==========================================
// LE Read Transmit Power Command (v5.0) (LE)
inline constexpr OpCode kLEReadTransmitPower =
    LEControllerCommandOpCode(0x004B);

// ================================================
// LE Read RF Path Compensation Command (v5.0) (LE)
inline constexpr OpCode kLEReadRFPathCompensation =
    LEControllerCommandOpCode(0x004C);

// =================================================
// LE Write RF Path Compensation Command (v5.0) (LE)
inline constexpr OpCode kLEWriteRFPathCompensation =
    LEControllerCommandOpCode(0x004D);

// =======================================
// LE Set Privacy Mode Command (v5.0) (LE)
inline constexpr OpCode kLESetPrivacyMode = LEControllerCommandOpCode(0x004E);

// ============================================
// LE Read Buffer Size [v2] Command (v5.2) (LE)
inline constexpr OpCode kLEReadBufferSizeV2 = LEControllerCommandOpCode(0x0060);

// =======================================
// LE Request Peer SCA Command (v5.2) (LE)
inline constexpr OpCode kLERequestPeerSCA = LEControllerCommandOpCode(0x006D);

// ==========================================
// LE Setup ISO Data Path Command (v5.2) (LE)
inline constexpr OpCode kLESetupISODataPath = LEControllerCommandOpCode(0x006E);

// =======================================
// LE Set Host Feature Command (v5.2) (LE)
inline constexpr OpCode kLESetHostFeature = LEControllerCommandOpCode(0x0074);

// =========================================
// LE Set CIG Parameters Command (v5.2) (LE)
inline constexpr OpCode kLESetCIGParameters = LEControllerCommandOpCode(0x0062);

// =================================
// LE Create CIS Command (v5.2) (LE)
inline constexpr OpCode kLECreateCIS = LEControllerCommandOpCode(0x0064);

// =========================================
// LE Accept CIS Request Command (v5.2) (LE)
inline constexpr OpCode kLEAcceptCISRequest = LEControllerCommandOpCode(0x0066);

// =========================================
// LE Reject CIS Request Command (v5.2) (LE)
inline constexpr OpCode kLERejectCISRequest = LEControllerCommandOpCode(0x0067);

// ======= Vendor Command =======
// Core Spec v5.0, Vol 2, Part E, Section 5.4.1
inline constexpr uint8_t kVendorOGF = 0x3F;
constexpr OpCode VendorOpCode(const uint16_t ocf) {
  return DefineOpCode(kVendorOGF, ocf);
}

}  // namespace bt::hci_spec
