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
// clang-format off

#include <array>
#include <cstddef>
#include <cstdint>

#include <pw_bluetooth/hci_commands.emb.h>
#include <pw_chrono/system_clock.h>
#include "pw_bluetooth_sapphire/internal/host/common/advertising_data.h"

// This file contains constants and numbers used in HCI packet payloads.

namespace bt::hci_spec {

// Bitmask values for the 8-octet Local Supported LMP Features bit-field. See
// Core Specv5.0, Volume 2, Part C, Section 3.3 "Feature Mask Definition".
enum class LMPFeature : uint64_t {
  // Extended features (Page 0): Octet 0
  k3SlotPackets   = (1 << 0),
  k5SlotPackets   = (1 << 1),
  kEncryption     = (1 << 2),
  kSlotOffset     = (1 << 3),
  kTimingAccuracy = (1 << 4),
  kRoleSwitch     = (1 << 5),
  kHoldMode       = (1 << 6),
  kSniffMode      = (1 << 7),

  // Extended features (Page 0): Octet 1
  // kPreviouslyUsed               = (1ull << 8),
  kPowerControlRequests         = (1ull << 9),
  kChannelQualityDrivenDataRate = (1ull << 10),
  kSCOLink                      = (1ull << 11),
  kHV2Packets                   = (1ull << 12),
  kHV3Packets                   = (1ull << 13),
  kmLawLogSynchronousData       = (1ull << 14),
  kALawLogSynchronousData       = (1ull << 15),

  // Extended features (Page 0): Octet 2
  kCVSDSynchronousData        = (1ull << 16),
  kPagingParameterNegotiation = (1ull << 17),
  kPowerControl               = (1ull << 18),
  kTransparentSynchronousData = (1ull << 19),
  kFCLLeastSignificantBit     = (1ull << 20),
  kFCLMiddleBit               = (1ull << 21),
  kFCLMostSignificantBit      = (1ull << 22),
  kBroadcastEncryption        = (1ull << 23),

  // Extended features (Page 0): Octet 3
  // Reserved (1ull << 24)
  kEDRACL2MbitMode        = (1ull << 25),
  kEDRACL3MbitMode        = (1ull << 26),
  kEnhancedInquiryScan    = (1ull << 27),
  kInterlacedInquiryScan  = (1ull << 28),
  kInterlacedPageScan     = (1ull << 29),
  kRSSIwithInquiryResults = (1ull << 30),
  kEV3Packets             = (1ull << 31),

  // Extended features (Page 0): Octet 4
  kEV4Packets                  = (1ull << 32),
  kEV5Packets                  = (1ull << 33),
  // Reserved
  kAFHCapablePeripheral        = (1ull << 35),
  kAFHClassificationPeripheral = (1ull << 36),
  kBREDRNotSupported           = (1ull << 37),
  kLESupportedController       = (1ull << 38),
  k3SlotEDRACLPackets          = (1ull << 39),

  // Extended features (Page 0): Octet 5
  k5SlotEDRACLPackets       = (1ull << 40),
  kSniffSubrating           = (1ull << 41),
  kPauseEncryption          = (1ull << 42),
  kAFHCapableCentral        = (1ull << 43),
  kAFHClassificationCentral = (1ull << 44),
  kEDReSCO2MbitMode         = (1ull << 45),
  kEDReSCO3MbitMode         = (1ull << 46),
  k3SlotEDReSCOPackets      = (1ull << 47),

  // Extended features (Page 0): Octet 6
  kExtendedInquiryResponse              = (1ull << 48),
  kSimultaneousLEAndBREDR               = (1ull << 49),
  // Reserved
  kSecureSimplePairingControllerSupport = (1ull << 51),
  kEncapsulatedPDU                      = (1ull << 52),
  kErroneousDataReporting               = (1ull << 53),
  kNonflushablePacketBoundaryFlag       = (1ull << 54),
  // Reserved

  // Extended features (Page 0): Octet 7
  kLinkSupervisionTimeoutChangedEvent = (1ull << 56),
  kVariableInquiryTxPowerLevel        = (1ull << 57),
  kEnhancedPowerControl               = (1ull << 58),
  // Reserved
  // Reserved
  // Reserved
  // Reserved
  kExtendedFeatures                   = (1ull << 63),

  // Extended features (Page 1): Octet 0
  kSecureSimplePairingHostSupport = (1ull << 0),
  kLESupportedHost                = (1ull << 1),
  kSimultaneousLEAndBREDRHost     = (1ull << 2),
  kSecureConnectionsHostSupport   = (1ull << 3),

  // Extended features (Page 2): Octet 0
  kCPBTransmitterOperation          = (1ull << 0),
  kCPBReceiverOperation             = (1ull << 1),
  kSynchronizationTrain             = (1ull << 2),
  kSynchronizationScan              = (1ull << 3),
  kInquiryResponseNotificationEvent = (1ull << 4),
  kGeneralizedInterlacedScan        = (1ull << 5),
  kCoarseClockAdjustment            = (1ull << 6),
  // Reserved

  // Extended features (Page 2): Octet 1
  kSecureConnectionsControllerSupport = (1ull << 8),
  kPing                               = (1ull << 9),
  kSlotAvailabilityMask               = (1ull << 10),
  kTrainNudging                       = (1ull << 11)
};

// Bitmask of 8-octet LE supported features field. See Core Spec
// v5.0, Volume 6, Part B, Section 4.6 "Feature Support".
using LESupportedFeatures = uint64_t;

// Bitmask values for the 8-octet LE Supported Features bit-field. See Core Spec
// v5.0, Volume 6, Part B, Section 4.6 "Feature Support".
enum class LESupportedFeature : uint64_t {
  kLEEncryption                         = (1 << 0),
  kConnectionParametersRequestProcedure = (1 << 1),
  kExtendedRejectIndication             = (1 << 2),
  kPeripheralInitiatedFeaturesExchange       = (1 << 3),
  kLEPing                               = (1 << 4),
  kLEDataPacketLengthExtension          = (1 << 5),
  kLLPrivacy                            = (1 << 6),
  kExtendedScannerFilterPolicies        = (1 << 7),

  // Added in 5.0
  kLE2MPHY                                  = (1 << 8),
  kStableModulationIndexTransmitter         = (1 << 9),
  kStableModulationIndexReceiver            = (1 << 10),
  kLECodedPHY                               = (1 << 11),
  kLEExtendedAdvertising                    = (1 << 12),
  kLEPeriodicAdvertising                    = (1 << 13),
  kChannelSelectionAlgorithm2               = (1 << 14),
  kLEPowerClass1                            = (1 << 15),
  kMinimumNumberOfUsedChannelsProcedure     = (1 << 16),

  // Added in 5.1
  kConnectionCTERequest                     = (1 << 17),
  kConnectionCTEResponse                    = (1 << 18),
  kConnectionlessCTETransmitter             = (1 << 19),
  kConnectionlessCTEReceiver                = (1 << 20),
  kAntennaSwitchingDuringCTETransmission    = (1 << 21),
  kAntennaSwitchingDuringCTEReception       = (1 << 22),
  kReceivingConstantToneExtensions          = (1 << 23),
  kPeriodicAdvertisingSyncTransferSender    = (1 << 24),
  kPeriodicAdvertisingSyncTransferRecipient = (1 << 25),
  kSleepClockAccuracyUpdates                = (1 << 26),
  kRemotePublicKeyValidation                = (1 << 27),

  // Added in 5.2
  kConnectedIsochronousStreamCentral     = (1 << 28),
  kConnectedIsochronousStreamPeripheral  = (1 << 29),
  kIsochronousBroadcaster                = (1 << 30),
  kSynchronizedReceiver                  = (1ull << 31),
  kConnectedIsochronousStreamHostSupport = (1ull << 32),
  kLEPowerControlRequest                 = (1ull << 33),
  kLEPowerChangeIndication               = (1ull << 34),
  kLEPathLossMonitoring                  = (1ull << 35),

  // The rest is reserved for future use.
};

// Bit positions for constants LE Supported Features that are controlled by the host
// for use in the LE Set Host Feature command
enum class LESupportedFeatureBitPos : uint8_t {
  kConnectedIsochronousStreamHostSupport = 32,
};

// Bitmask values for the 8-octet HCI_Set_Event_Mask command parameter.
enum class EventMask : uint64_t {
  kInquiryCompleteEvent                         = (1 << 0),
  kInquiryResultEvent                           = (1 << 1),
  kConnectionCompleteEvent                      = (1 << 2),
  kConnectionRequestEvent                       = (1 << 3),
  kDisconnectionCompleteEvent                   = (1 << 4),
  kAuthenticationCompleteEvent                  = (1 << 5),
  kRemoteNameRequestCompleteEvent               = (1 << 6),
  kEncryptionChangeEvent                        = (1 << 7),
  kChangeConnectionLinkKeyCompleteEvent         = (1 << 8),
  kLinkKeyTypeChangedEvent                      = (1 << 9),
  kReadRemoteSupportedFeaturesCompleteEvent     = (1 << 10),
  kReadRemoteVersionInformationCompleteEvent    = (1 << 11),
  kQoSSetupCompleteEvent                        = (1 << 12),
  // Reserved For Future Use: (1 << 13)
  // Reserved For Future Use: (1 << 14)
  kHardwareErrorEvent                           = (1 << 15),
  kFlushOccurredEvent                           = (1 << 16),
  kRoleChangeEvent                              = (1 << 17),
  // Reserved For Future Use: (1 << 18)
  kModeChangeEvent                              = (1 << 19),
  kReturnLinkKeysEvent                          = (1 << 20),
  kPINCodeRequestEvent                          = (1 << 21),
  kLinkKeyRequestEvent                          = (1 << 22),
  kLinkKeyNotificationEvent                     = (1 << 23),
  kLoopbackCommandEvent                         = (1 << 24),
  kDataBufferOverflowEvent                      = (1 << 25),
  kMaxSlotsChangeEvent                          = (1 << 26),
  kReadClockOffsetCompleteEvent                 = (1 << 27),
  kConnectionPacketTypeChangedEvent             = (1 << 28),
  kQoSViolationEvent                            = (1 << 29),
  kPageScanModeChangeEvent                      = (1 << 30),  // deprecated
  kPageScanRepetitionModeChangeEvent            = (1ull << 31),
  kFlowSpecificationCompleteEvent               = (1ull << 32),
  kInquiryResultWithRSSIEvent                   = (1ull << 33),
  kReadRemoteExtendedFeaturesCompleteEvent      = (1ull << 34),
  // Reserved For Future Use: (1ull << 35)
  // Reserved For Future Use: (1ull << 36)
  // Reserved For Future Use: (1ull << 37)
  // Reserved For Future Use: (1ull << 38)
  // Reserved For Future Use: (1ull << 39)
  // Reserved For Future Use: (1ull << 40)
  // Reserved For Future Use: (1ull << 41)
  // Reserved For Future Use: (1ull << 42)
  kSynchronousConnectionCompleteEvent           = (1ull << 43),
  kSynchronousConnectionChangedEvent            = (1ull << 44),
  kSniffSubratingEvent                          = (1ull << 45),
  kExtendedInquiryResultEvent                   = (1ull << 46),
  kEncryptionKeyRefreshCompleteEvent            = (1ull << 47),
  kIOCapabilityRequestEvent                     = (1ull << 48),
  kIOCapabilityResponseEvent                    = (1ull << 49),
  kUserConfirmationRequestEvent                 = (1ull << 50),
  kUserPasskeyRequestEvent                      = (1ull << 51),
  kRemoteOOBDataRequestEvent                    = (1ull << 52),
  kSimplePairingCompleteEvent                   = (1ull << 53),
  // Reserved For Future Use: (1ull << 54)
  kLinkSupervisionTimeoutChangedEvent           = (1ull << 55),
  kEnhancedFlushCompleteEvent                   = (1ull << 56),
  // Reserved For Future Use: (1ull << 57)
  kUserPasskeyNotificationEvent                 = (1ull << 58),
  kKeypressNotificationEvent                    = (1ull << 59),
  kRemoteHostSupportedFeaturesNotificationEvent = (1ull << 60),
  kLEMetaEvent                                  = (1ull << 61),
  // Reserved For Future Use: (1ull << 62)
  // Reserved For Future Use: (1ull << 63)
};

// Bitmask values for the 8-octet HCI_Set_Event_Mask_Page_2 command parameter.
enum class EventMaskPage2 : uint64_t {
  kPhysicalLinkCompleteEvent                              = (1 << 0),
  kChannelSelectedEvent                                   = (1 << 1),
  kDisconnectionPhysicalLinkCompleteEvent                 = (1 << 2),
  kPhysicalLinkLossEarlyWarningEvent                      = (1 << 3),
  kPhysicalLinkRecoveryEvent                              = (1 << 4),
  kLogicalLinkCompleteEvent                               = (1 << 5),
  kDisconnectionLogicalLinkCompleteEvent                  = (1 << 6),
  kFlowSpecModifyCompleteEvent                            = (1 << 7),
  kNumberOfCompletedDataBlocksEvent                       = (1 << 8),
  kAMPStartTestEvent                                      = (1 << 9),
  kAMPTestEndEvent                                        = (1 << 10),
  kAMPReceiverReportEvent                                 = (1 << 11),
  kShortRangeModeChangeCompleteEvent                      = (1 << 12),
  kAMPStatusChangeEvent                                   = (1 << 13),
  kTriggeredClockCaptureEvent                             = (1 << 14),
  kSynchronizationTrainCompleteEvent                      = (1 << 15),
  kSynchronizationTrainReceivedEvent                      = (1 << 16),
  kConnectionlessPeripheralBroadcastReceiveEvent          = (1 << 17),
  kConnectionlessPeripheralBroadcastTimeoutEvent          = (1 << 18),
  kTruncatedPageCompleteEvent                             = (1 << 19),
  kPeripheralPageResponseTimeoutEvent                     = (1 << 20),
  kConnectionlessPeripheralBroadcastChannelMapChangeEvent = (1 << 21),
  kInquiryResponseNotificationEvent                       = (1 << 22),
  kAuthenticatedPayloadTimeoutExpiredEvent                = (1 << 23),
  kSAMStatusChangeEvent                                   = (1 << 24),
};

// Bitmask values for the 8-octet HCI_LE_Set_Event_Mask command parameter.
enum class LEEventMask : uint64_t {
  kLEConnectionComplete                        = (1ull << 0),
  kLEAdvertisingReport                         = (1ull << 1),
  kLEConnectionUpdateComplete                  = (1ull << 2),
  kLEReadRemoteFeaturesComplete                = (1ull << 3),
  kLELongTermKeyRequest                        = (1ull << 4),
  kLERemoteConnectionParameterRequest          = (1ull << 5),
  kLEDataLengthChange                          = (1ull << 6),
  kLEReadLocalP256PublicKeyComplete            = (1ull << 7),
  kLEGenerateDHKeyComplete                     = (1ull << 8),
  kLEEnhancedConnectionComplete                = (1ull << 9),
  kLEDirectedAdvertisingReport                 = (1ull << 10),
  kLEPHYUpdateComplete                         = (1ull << 11),
  kLEExtendedAdvertisingReport                 = (1ull << 12),
  kLEPeriodicAdvertisingSyncEstablished        = (1ull << 13),
  kLEPeriodicAdvertisingReport                 = (1ull << 14),
  kLEPeriodicAdvertisingSyncLost               = (1ull << 15),
  kLEExtendedScanTimeout                       = (1ull << 16),
  kLEExtendedAdvertisingSetTerminated          = (1ull << 17),
  kLEScanRequestReceived                       = (1ull << 18),
  kLEChannelSelectionAlgorithm                 = (1ull << 19),
  kLEConnectionlessIQReport                    = (1ull << 20),
  kLEConnectionIQReport                        = (1ull << 21),
  kLECTERequestFailed                          = (1ull << 22),
  kLEPeriodicAdvertisingSyncTransferReceived   = (1ull << 23),
  kLECISEstablished                            = (1ull << 24),
  kLECISRequest                                = (1ull << 25),
  kLECreateBIGComplete                         = (1ull << 26),
  kLETerminateBIGComplete                      = (1ull << 27),
  kLEBIGSyncEstablished                        = (1ull << 28),
  kLEBIGSyncLost                               = (1ull << 29),
  kLERequestPeerSCAComplete                    = (1ull << 30),
  kLEPathLossThreshold                         = (1ull << 31),
  kLETransmitPowerReporting                    = (1ull << 32),
  kLEBIGInfoAdvertisingReport                  = (1ull << 33),
  kLESubrateChange                             = (1ull << 34),
  kLEPeriodicAdvertisingSyncEstablishedV2      = (1ull << 35),
  kLEPeriodicAdvertisingReportV2               = (1ull << 36),
  kLEPeriodicAdvertisingSyncTransferReceivedV2 = (1ull << 37),
  kLEPeriodicAdvertisingSubeventDataRequest    = (1ull << 38),
  kLEPeriodicAdvertisingResponseReport         = (1ull << 39),
  kLEEnhancedConnectionCompleteV2              = (1ull << 40),
};

// Values that can be passed to the Type parameter in a
// HCI_Read_Transmit_Power_Level command.
enum class ReadTransmitPowerType : uint8_t {
  // Read Current Transmit Power Level.
  kCurrent = 0x00,

  // Read Maximum Transmit Power Level.
  kMax = 0x01,
};

// Possible values for the Encryption_Enabled parameter in a HCI_Encryption_Change event
// (see Vol 2, Part E, 7.7.8).
enum class EncryptionStatus : uint8_t {
  // Link Level Encryption is OFF.
  kOff = 0x00,

  // Link Level Encryption is ON with E0 for BR/EDR and AES-CCM for LE.
  kOn = 0x01,

  // Link Level Encryption is ON with AES-CCM for BR/EDR.
  kBredrSecureConnections = 0x02,
};

// The minimum and maximum range values for the LE advertising interval
// parameters.
// (see Core Spec v5.0, Vol 2, Part E, Section 7.8.5)
inline constexpr uint16_t kLEAdvertisingIntervalMin = 0x0020;
inline constexpr uint16_t kLEAdvertisingIntervalMax = 0x4000;

// The minimum and maximum range values for the LE periodic advertising interval
// parameters.
// (see Core Spec v5.0, Vol 2, Part E, Section 7.8.61)
inline constexpr uint16_t kLEPeriodicAdvertisingIntervalMin = 0x0006;
inline constexpr uint16_t kLEPeriodicAdvertisingIntervalMax = 0xFFFF;

// The minimum and maximum range values for the LE extended advertising interval
// parameters.
inline constexpr uint32_t kLEExtendedAdvertisingIntervalMin = 0x000020;
inline constexpr uint32_t kLEExtendedAdvertisingIntervalMax = 0xFFFFFF;

// The default LE advertising interval parameter value, corresponding to 1.28
// seconds (see Core Spec v5.0, Vol 2, Part E, Section 7.8.5).
inline constexpr uint16_t kLEAdvertisingIntervalDefault = 0x0800;

// The minimum and maximum range values for the LE scan interval parameters.
// (see Core Spec v5.0, Vol 2, Part E, Section 7.8.10)
inline constexpr uint16_t kLEScanIntervalMin = 0x0004;
inline constexpr uint16_t kLEScanIntervalMax = 0x4000;

// The minimum and maximum range values for the LE scan interval parameters.
// (see Core Spec v6.0, Vol 4, Part E, Section 7.8.10)
inline constexpr uint16_t kLEScanWindowMin = 0x0004;
inline constexpr uint16_t kLEScanWindowMax = 0x4000;

// The minimum and maximum range values for the LE extended scan interval
// parameters.
// (see Core Spec v5.0, Vol 2, Part E, Section 7.8.64)
inline constexpr uint16_t kLEExtendedScanIntervalMin = 0x0004;
inline constexpr uint16_t kLEExtendedScanIntervalMax = 0xFFFF;

// The minimum and maximum range values for the LE extended scan interval
// parameters.
// (see Core Spec v6.0, Vol 4, Part E, Section 7.8.64)
inline constexpr uint16_t kLEExtendedScanWindowMin = 0x0004;
inline constexpr uint16_t kLEExtendedScanWindowMax = 0xFFFF;

// The default LE scan interval parameter value, corresponding to 10
// milliseconds (see Core Spec v5.0, Vol 2, Part E, Section 7.8.10).
inline constexpr uint16_t kLEScanIntervalDefault = 0x0010;

// The minimum and maximum range values for the LE connection interval parameters.
// (see Core Spec v5.0, Vol 2, Part E, Section 7.8.12)
inline constexpr uint16_t kLEConnectionIntervalMin = 0x0006;
inline constexpr uint16_t kLEConnectionIntervalMax = 0x0C80;

// The maximum value that can be used for the |conn_latency| parameter in a
// HCI_LE_Create_Connection
// command (see Core Spec v5.0, Vol 2, Part E, Section 7.8.12).
inline constexpr uint16_t kLEConnectionLatencyMax = 0x01F3;

// The minimum and maximum range values for LE connection supervision timeout
// parameters.
inline constexpr uint16_t kLEConnectionSupervisionTimeoutMin = 0x000A;
inline constexpr uint16_t kLEConnectionSupervisionTimeoutMax = 0x0C80;

// The minimum and maximum range values for LE link layer tx PDU used on
// connections.
inline constexpr uint16_t kLEMaxTxOctetsMin = 0x001B;
inline constexpr uint16_t kLEMaxTxOctetsMax = 0x00FB;

// The minimum and maximum range values for LE link layer tx maximum packet
// transmission time used on connections.
inline constexpr uint16_t kLEMaxTxTimeMin = 0x0148;
inline constexpr uint16_t kLEMaxTxTimeMax = 0x4290;

// Minimum, maximum, default values for the Resolvable Private Address timeout
// parameter.
inline constexpr uint16_t kLERPATimeoutMin = 0x0001;      // 1 second
inline constexpr uint16_t kLERPATimeoutMax = 0xA1B8;      // Approx. 11.5 hours
inline constexpr uint16_t kLERPATimeoutDefault = 0x0384;  // 900 seconds or 15 minutes.

// The maximum length of advertising data that can get passed to the
// HCI_LE_Set_Advertising_Data command.
//
// This constant should be used on pre-5.0 controllers. On controllers that
// support 5.0+ the host should use the
// HCI_LE_Read_Maximum_Advertising_Data_Length command to obtain this
// information.
inline constexpr size_t kMaxLEAdvertisingDataLength = 0x1F;  // (31)

// Core Spec Version 5.4, Volume 6, Part B, Section 2.3.4.9: the total
// amount of Host Advertising Data before fragmentation shall not exceed 1650
// octets.
inline constexpr size_t kMaxLEExtendedAdvertisingDataLength = 1650;

// Tx Power values, See Core Spec v5.0 Vol 4, Part E, 7.8.6.
inline constexpr int8_t kLEAdvertisingTxPowerMin = -127;
inline constexpr int8_t kLEAdvertisingTxPowerMax = 20;
inline constexpr int8_t kLEExtendedAdvertisingTxPowerNoPreference = 0x7F; // Vol 4, Part E, 7.8.53

// The valid range of RSSI values in event packets. Core Spec v6.0, Vol 4, Part E, 7.7.38.
inline constexpr int8_t kMinRssi = -127;
inline constexpr int8_t kMaxRssi = 20;

// Core Spec v5.0 Volume 4, Part E, Section 7.8.56: The range of valid
// advertising handles is 0x00 to 0xEF. However, some Controllers internally
// reserve advertising handle 0x00 for legacy HCI LE command use. To avoid
// issues with those Controllers, we avoid using 0x00 entirely. These values are
// used in enabling extended advertising.
inline constexpr uint8_t kMinAdvertisingHandle = 0x01;
inline constexpr uint8_t kMaxAdvertisingHandle = 0xEF;
inline constexpr uint8_t kNoMaxExtendedAdvertisingEvents = 0;
inline constexpr uint8_t kNoAdvertisingDuration = 0;

// Values used in enabling extended scanning. See Core Spec v5.4 Vol 4, Part E, 7.8.64.
inline constexpr uint16_t kNoScanningDuration = 0;
inline constexpr uint16_t kNoScanningPeriod = 0;

// LE Advertising event types that can be reported in a LE Advertising Report
// event.
enum class LEAdvertisingEventType : uint8_t {
  // Connectable and scannable undirected advertising (ADV_IND)
  kAdvInd = 0x00,

  // Connectable directed advertising (ADV_DIRECT_IND)
  kAdvDirectInd = 0x01,

  // Scannable undirected advertising (ADV_SCAN_IND)
  kAdvScanInd = 0x02,

  // Non connectable undirected advertising (ADV_NONCONN_IND)
  kAdvNonConnInd = 0x03,

  // Scan Response (SCAN_RSP)
  kScanRsp = 0x04,

  // The rest is reserved for future use
};

// Possible values that can be reported for the |address_type| parameter in a LE
// Advertising Report event.
enum class LEAddressType : uint8_t {
  // Public Device Address
  kPublic = 0x00,

  // Random Device Address
  kRandom = 0x01,

  // Public Identity Address (Corresponds to Resolved Private Address)
  kPublicIdentity = 0x02,

  // Random (static) Identity Address (Corresponds to Resolved Private Address)
  kRandomIdentity = 0x03,

  // This is a special value used in LE Extended Advertising Report events to
  // indicate a random address that the controller was unable to resolve.
  kRandomUnresolved = 0xFE,

  // This is a special value that is only used in LE Directed Advertising Report
  // events.
  // Meaning: No address provided (anonymous advertisement)
  kAnonymous = 0xFF,
};

// Possible values that can be used for the |peer_address_type| parameter in a
// HCI_LE_Set_Advertising_Parameters command.
// (see Core Spec v5.0, Vol 2, Part E, Section 7.8.5)
enum class LEPeerAddressType : uint8_t {
  // Public Device Address (default) or Public Identity Address
  kPublic = 0x00,

  // Random Device Address or Random (static) Identity Address
  kRandom = 0x01,

  // This is a special value that should only be used with the
  // HCI_LE_Add_Device_To_Filter_Accept_List and HCI_LE_Remove_Device_From_Filter_Accept_List
  // commands for peer devices sending anonymous advertisements.
  kAnonymous = 0xFF,
};

// Possible values that can be used for the |adv_channel_map| bitfield in a
// HCI_LE_Set_Advertising_Parameters command.
// (see Core Spec v5.0, Vol 2, Part E, Section 7.8.5)
inline constexpr uint8_t kLEAdvertisingChannel37 = 0x01;
inline constexpr uint8_t kLEAdvertisingChannel38 = 0x02;
inline constexpr uint8_t kLEAdvertisingChannel39 = 0x04;
inline constexpr uint8_t kLEAdvertisingChannelAll = 0x07;

// Possible values that can be used for the Filter_Policy parameter in a
// HCI_LE_Periodic_Advertising_Create_Sync command.
// (see Core Spec v5.0, Vol 2, Part E, Section 7.8.67)
enum class LEPeriodicAdvFilterPolicy : uint8_t {
  // Use the Advertising_SID, Advertising_Address_Type, and Advertising_Address
  // parameters to determine which advertiser to listen to.
  kListNotUsed = 0x00,

  // Use the Periodic Advertiser List to determine which advertiser to listen to.
  kUsePeriodicAdvList = 0x01,
};

// The PHY bitfield values that can be used in HCI_LE_Set_PHY and
// HCI_LE_Set_Default_PHY commands that can be used for the TX_PHYS and RX_PHYS
// parameters.
inline constexpr uint8_t kLEPHYBit1M = (1 << 0);
inline constexpr uint8_t kLEPHYBit2M = (1 << 1);
inline constexpr uint8_t kLEPHYBitCoded = (1 << 2);

// The PHY bitfield values that can be used in HCI_LE_Set_PHY and
// HCI_LE_Set_Default_PHY commands that can be used for the ALL_PHYS parameter.
inline constexpr uint8_t kLEAllPHYSBitTxNoPreference = (1 << 0);
inline constexpr uint8_t kLEAllPHYSBitRxNoPreference = (1 << 1);

// Potential values that can be used for the LE PHY parameters in HCI commands
// and events.
enum class LEPHY : uint8_t {
  kLE1M = 0x01,
  kLE2M = 0x02,

  // Only for the HCI_LE_Enhanced_Transmitter_Test command this value implies
  // S=8 data coding. Otherwise this refers to general LE Coded PHY.
  kLECoded = 0x03,

  // This should ony be used with the HCI_LE_Enhanced_Transmitter_Test command.
  kLECodedS2 = 0x04,

  // Some HCI events may use this value to indicate that no packets were sent
  // on a particular PHY, specifically the LE Extended Advertising Report event.
  kNone = 0x00,
};

// Potential values that can be used for the PHY_options parameter in a
// HCI_LE_Set_PHY command.
enum class LEPHYOptions : uint16_t {
  kNoPreferredEncoding = 0x00,
  kPreferS2Coding = 0x01,
  kPreferS8Coding = 0x02,
};

// Potential values that can be passed for the Modulation_Index parameter in a
// HCI_LE_Enhanced_Receiver_Test command.
enum class LETestModulationIndex : uint8_t {
  kAssumeStandard = 0x00,
  kAssumeStable = 0x01,
};

// Potential values for the Operation parameter in a
// HCI_LE_Set_Extended_Advertising_Data command.
enum class LESetExtendedAdvDataOp : uint8_t {
  // Intermediate fragment of fragmented extended advertising data.
  kIntermediateFragment = 0x00,

  // First fragment of fragmented extended advertising data.
  kFirstFragment = 0x01,

  // Last fragment of fragmented extended advertising data.
  kLastFragment = 0x02,

  // Complete extended advertising data.
  kComplete = 0x03,

  // Unchanged data (just update the Advertising DID)
  kUnchangedData = 0x04,
};

// Potential values for the Fragment_Preference parameter in a
// HCI_LE_Set_Extended_Advertising_Data command.
enum class LEExtendedAdvFragmentPreference : uint8_t {
  // The Controller may fragment all Host advertising data
  kMayFragment = 0x00,

  // The Controller should not fragment or should minimize fragmentation of Host
  // advertising data
  kShouldNotFragment = 0x01,
};

// The Advertising_Event_Properties bitfield values used in a
// HCI_LE_Set_Extended_Advertising_Parameters command.
inline constexpr uint16_t kLEAdvEventPropBitConnectable                      = (1 << 0);
inline constexpr uint16_t kLEAdvEventPropBitScannable                        = (1 << 1);
inline constexpr uint16_t kLEAdvEventPropBitDirected                         = (1 << 2);
inline constexpr uint16_t kLEAdvEventPropBitHighDutyCycleDirectedConnectable = (1 << 3);
inline constexpr uint16_t kLEAdvEventPropBitUseLegacyPDUs                    = (1 << 4);
inline constexpr uint16_t kLEAdvEventPropBitAnonymousAdvertising             = (1 << 5);
inline constexpr uint16_t kLEAdvEventPropBitIncludeTxPower                   = (1 << 6);

// The Event_Type bitfield values reported in a LE Extended Advertising Report Event.
inline constexpr uint16_t kLEExtendedAdvEventTypeConnectable  = (1 << 0);
inline constexpr uint16_t kLEExtendedAdvEventTypeScannable    = (1 << 1);
inline constexpr uint16_t kLEExtendedAdvEventTypeDirected     = (1 << 2);
inline constexpr uint16_t kLEExtendedAdvEventTypeScanResponse = (1 << 3);
inline constexpr uint16_t kLEExtendedAdvEventTypeLegacy       = (1 << 4);

// LE Advertising data status properties stored in bits 5 and 6 of the
// Event_Type bitfield of a LE Extended Advertising Report event and in a LE
// Periodic Advertising Report event.
enum class LEAdvertisingDataStatus : uint16_t {
  // Data is complete.
  kComplete = 0x00,

  // Data is incomplete, more data to come in future events.
  kIncomplete = 0x01,

  // Data is incomplete and truncated, no more data to come.
  kIncompleteTruncated = 0x02,
};

// The Periodic_Advertising_Properties bitfield used in a
// HCI_LE_Set_Periodic_Advertising_Parameters command.
inline constexpr uint16_t kLEPeriodicAdvPropBitIncludeTxPower = (1 << 6);

// The maximum length of LE data packets when the LE Data Packet Length Extension
// feature is supported. See v5.0, Vol 6, Part B, 4.5.10, Table 4.3.
inline constexpr size_t kMaxLEExtendedDataLength = 251;

// Maximum value of the Advertising SID subfield in the ADI field of the PDU
inline constexpr uint8_t kLEAdvertsingSIDMax = 0xEF;

// Invalid RSSI value.
inline constexpr int8_t kRSSIInvalid = 127;

// Invalid advertising sid value
inline constexpr uint8_t kAdvertisingSidInvalid = 0xFF;

// Invalid periodic advertising interval
inline constexpr uint16_t kPeriodicAdvertisingIntervalInvalid = 0x0000;

// The maximum length of a friendly name that can be assigned to a BR/EDR
// controller, in octets.
inline constexpr size_t kMaxNameLength = bt::kMaxNameLength;

// The maximum number of bytes in a HCI Command Packet payload, excluding the
// header. See Core Spec v5.0 Vol 2, Part E, 5.4.1, paragraph 2.
inline constexpr size_t kMaxCommandPacketPayloadSize = 255;

// The maximum number of bytes in a HCI event Packet payload, excluding the
// header. See Core Spec v5.0 Vol 2, Part E, 5.4.4, paragraph 1.
inline constexpr size_t kMaxEventPacketPayloadSize = 255;

// The maximum number of bytes in a HCI ACL data packet payload supported by our
// stack.
inline constexpr size_t kMaxACLPayloadSize = 1024;

// The maximum number of bytes in a HCI Synchronous Data packet payload.
// This is based on the maximum value of the 1-byte Data_Total_Length field of a Synchronous Data packet.
inline constexpr size_t kMaxSynchronousDataPacketPayloadSize = 255;

// The maximum number of bytes in an Isochronous data packet payload, based on
// the maximum size (12 bits) of the data_total_length field of an Isochronous
// data packet.
// See Core Spec v5.4, Vol 4, Part E, Section 5.4.5
inline constexpr size_t kMaxIsochronousDataPacketPayloadSize = 16384;

// The minimum number of bytes in an Isochronous data packet payload, to
// accommodate the mandatory SDU header size.
// See Core Spec v6.3, Vol 4, Part E, Section 5.4.5
inline constexpr size_t kMinIsoDataPacketLength = 4;

// Values that can be used in HCI Read|WriteFlowControlMode commands.
enum class FlowControlMode : uint8_t {
  // Packet based data flow control mode (default for a Primary Controller)
  kPacketBased = 0x00,

  // Data block based flow control mode (default for an AMP Controller)
  kDataBlockBased = 0x01
};

// The Packet Boundary Flag is contained in bits 4 and 5 in the second octet of
// a HCI ACL Data packet.
enum class ACLPacketBoundaryFlag : uint8_t {
  kFirstNonFlushable  = 0x00,
  kContinuingFragment = 0x01,
  kFirstFlushable     = 0x02,
  kCompletePDU        = 0x03,
};

// The Broadcast Flag is contained in bits 6 and 7 in the second octet of a HCI
// ACL Data packet.
enum class ACLBroadcastFlag : uint8_t {
  kPointToPoint              = 0x00,
  kActivePeripheralBroadcast = 0x01,
};

// The Packet Status Flag is contained in bits 4 to 5 of the second octet of
// a HCI Synchronous Data packet (Core Spec v5.2, Vol 4, Part E, Sec 5.4.3).
enum class SynchronousDataPacketStatusFlag : uint8_t {
  kCorrectlyReceived  = 0x00,
  kPossiblyInvalid  = 0x01,
  kNoDataReceived  = 0x02,
  kDataPartiallyLost = 0x03,
};

// Possible values that can be reported in a LE Channel Selection Algorithm event.
enum class LEChannelSelectionAlgorithm : uint8_t {
  kAlgorithm1 = 0x00,
  kAlgorithm2 = 0x01,
};

// "Hosts and Controllers shall be able to accept HCI ACL Data Packets with up
// to 27 bytes of data excluding the HCI ACL Data Packet header on Connection
// Handles associated with an LE-U logical link." (See Core Spec v5.0, Volume 2,
// Part E, Section 5.4.2)
inline constexpr size_t kMinLEACLDataBufferLength = 27;

// The maximum value that can be used for a 12-bit connection handle.
inline constexpr uint16_t kConnectionHandleMax = 0x0EFF;

// The maximum value that can ve used for a 8-bit advertising set handle.
inline constexpr uint8_t kAdvertisingHandleMax = 0xEF;

// The maximum value that can be used for a CIG ID.
inline constexpr uint8_t kMaxCigId = 0xEF;


// The maximum value that can be set for the length of an Inquiry
inline constexpr uint8_t kInquiryLengthMax = 0x30;

// Bit 15, or "Clock_Offset_Valid_Flag" of the 16-bit clock offset field.
// Some HCI commands that require a clock offset expect this bit to be set (e.g.
// see HCI_Remote_Name_Request command, Vol 2, Part E, 7.1.19).
inline constexpr uint16_t kClockOffsetValidFlagBit = 0x8000;

// Masks the lower 15 bits of a Clock_Offset, excluding the bit 15 - the reserved/validity bit.
inline constexpr uint16_t kClockOffsetMask = 0x7FFF;

// Bitmask Values for the Scan_Enable parameter in a
// HCI_(Read,Write)_Scan_Enable command.
enum class ScanEnableBit : uint8_t {
  kInquiry  = (1 << 0), // Inquiry scan enabled
  kPage     = (1 << 1), // Page scan enabled.
};

using ScanEnableType = uint8_t;

// Constant values for common scanning modes
// See Spec 5.0, Vol 3, Part C, Section 4.2.2.1, Table 4.2
inline constexpr uint16_t kPageScanR0Interval = 0x0800; // 1.28s
inline constexpr uint16_t kPageScanR0Window = 0x0800; // 1.28s
inline constexpr uint16_t kPageScanR1Interval = 0x0800; // 1.28s
inline constexpr uint16_t kPageScanR1Window = 0x0011; // 10.625ms
inline constexpr uint16_t kPageScanR2Interval = 0x1000; // 2.56s
inline constexpr uint16_t kPageScanR2Window = 0x0011; // 10.625ms

enum class InquiryScanType : uint8_t {
  kStandardScan = 0x00, // Standard scan (default) (mandatory)
  kInterlacedScan = 0x01, // Interlaced scan
};

// Link Types for BR/EDR connections.
enum class LinkType : uint8_t {
  kSCO = 0x00,         // SCO
  kACL = 0x01,         // ACL (data channel)
  kExtendedSCO = 0x02, // eSCO
};

// Length of the Extended Inquiry Response data. (Vol 3, Part C, Section 8)
inline constexpr size_t kExtendedInquiryResponseBytes = 240;

// Maximum length of a local name in the Extended Inquiry Response data.
// Length: 1 byte, DataType: 1 byte, Remaining buffer: 238 bytes.
// (Vol 3, Part C, Section 8)
inline constexpr size_t kExtendedInquiryResponseMaxNameBytes = kExtendedInquiryResponseBytes - 2;

// Minimum supported encryption key size for ACL-U links, as queried by Read
// Encryption Key Size. This isn't specified so the value is taken from the LE
// limit for SM Long Term Keys (v5.0 Vol 3, Part H, 2.3.4). This limit applies
// to the per-session encryption key, not the semi-permanent Link Key (v5.0
// Vol 2, Part H, 1).
inline constexpr uint8_t kMinEncryptionKeySize = 7;

// inclusive-language: ignore
// Ignore inclusive language check to match the language used in the spec
//
// Key types for BR/EDR link encryption as reported to the host using the Link
// Key Notification event upon pairing or key changes (v5.0 Vol 2, Part E,
// Section 7.7.24).
//
// "Combination" refers to keys created from contributions of two devices
// according to v5.0 Vol 2, Part H, Section 3.2.4 and as opposed to "unit" keys
// that are generated on a single device but used by both parties (Section 3.2.3
// and deprecated in Section 3.1).
//
// Authenticated keys were generated using a challenge-response scheme described
// inclusive-language: ignore
// in v5.0 Vol 2, Part H, Section 5 to protect against man-in-the-middle (MITM)
// attacks.
//
// When Secure Connections is used, the key exchange uses keys generated from
// points on a 256-bit elliptic curve (v5.0 Vol 2, Part H, Section 7.1) and
// authentication uses Secure Authentication procedures described in Section 5.
enum class LinkKeyType : uint8_t {
  // Legacy pairing (pre-v2.1) key types
  kCombination = 0x00,
  kLocalUnit = 0x01,
  kRemoteUnit = 0x02,

  // Secure Simple Pairing key types
  kDebugCombination = 0x03,
  kUnauthenticatedCombination192 = 0x04,
  kAuthenticatedCombination192 = 0x05,

  // Special value indicating key generated due to a Change Connection Link Key
  // command. The actual key type is the same as before the change.
  kChangedCombination = 0x06,

  // Secure Simple Pairing with Secure Connections key types
  kUnauthenticatedCombination256 = 0x07,
  kAuthenticatedCombination256 = 0x08,
};

// Bitmask values for supported Packet Types
// Used for HCI_Create_Connection and HCI_Change_Connection_Packet_Type
enum class PacketTypeBits : uint16_t {
  // Reserved (1 << 0)
  kDisable2DH1 = (1 << 1),
  kDisable3DH1 = (1 << 2),
  kEnableDM1 = (1 << 3), // Note: always on in >= v1.2
  kEnableDH1 = (1 << 4),
  // Reserved (1 << 5)
  // Reserved (1 << 6)
  // Reserved (1 << 7)
  kDisable2DH3 = (1 << 8),
  kDisable3DH3 = (1 << 9),
  kEnableDM3 = (1 << 10),
  kEnableDH3 = (1 << 11),
  kDisable2DH5 = (1 << 12),
  kDisable3DH5 = (1 << 13),
  kEnableDM5 = (1 << 14),
  kEnableDH5 = (1 << 15),
};

using PacketTypeType = uint16_t;

enum class RoleSwitchBits : uint8_t {
  kDisallowRoleSwitch = 0x0,
  kAllowRoleSwitch = 0x1
};

enum class ScoRetransmissionEffort : uint8_t {
  // SCO or eSCO
  kNone = 0x00,

  // eSCO only
  kPowerOptimized  = 0x01,

  // eSCO only
  kQualityOptimized  = 0x02,

  // SCO or eSCO
  kDontCare = 0xFF,
};

// Flush Timeout = N * 0.625ms (Core Spec v5.2, Vol 4, Part E, Sec 7.3.30).
inline constexpr float kFlushTimeoutCommandParameterToMillisecondsConversionFactor = 0.625f;
inline constexpr float kFlushTimeoutMsToCommandParameterConversionFactor = 1.0f / kFlushTimeoutCommandParameterToMillisecondsConversionFactor;

// See Core Spec v5.2, Vol 4, Part E, Sec 7.3.30
inline constexpr uint16_t kMaxAutomaticFlushTimeoutCommandParameterValue = 0x07FF;
inline constexpr pw::chrono::SystemClock::duration kMaxAutomaticFlushTimeoutDuration = std::chrono::milliseconds(static_cast<int64_t>(kMaxAutomaticFlushTimeoutCommandParameterValue * kFlushTimeoutCommandParameterToMillisecondsConversionFactor));

// Page Timeout = N * 0.625 ms (Core Spec v5.2, Vol 4, Part E, Sec 7.3.16).
// The default is 5.12 sec.
inline constexpr pw::chrono::SystemClock::duration kDurationPerPageTimeoutUnit = std::chrono::duration_cast<pw::chrono::SystemClock::duration>(std::chrono::microseconds(625));
inline constexpr pw::chrono::SystemClock::duration kMinPageTimeoutDuration = kDurationPerPageTimeoutUnit * static_cast<uint16_t>(pw::bluetooth::emboss::PageTimeout::MIN);
inline constexpr pw::chrono::SystemClock::duration kDefaultPageTimeoutDuration = kDurationPerPageTimeoutUnit * static_cast<uint16_t>(pw::bluetooth::emboss::PageTimeout::DEFAULT);
inline constexpr pw::chrono::SystemClock::duration kMaxPageTimeoutDuration = kDurationPerPageTimeoutUnit * static_cast<uint16_t>(pw::bluetooth::emboss::PageTimeout::MAX);

}  // namespace bt::hci_spec
