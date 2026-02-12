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
#include "pw_bluetooth/hci_data.emb.h"
#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/common/device_address.h"
#include "pw_bluetooth_sapphire/internal/host/gap/gap.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"
#include "pw_bluetooth_sapphire/internal/host/iso/iso_common.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/a2dp_offload_manager.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/l2cap_defs.h"
#include "pw_bluetooth_sapphire/internal/host/transport/emboss_packet.h"

namespace bt::testing {

// This module contains functionality to create arbitrary HCI packets defining
// common behaviors with respect to expected devices and connections.
// This allows easily defining expected packets to be sent or received for
// given transactions such as connection establishment or discovery

// Generates a blob of data that is unique to the size and starting value
std::vector<uint8_t> GenDataBlob(size_t size, uint8_t starting_value);

DynamicByteBuffer AcceptConnectionRequestPacket(DeviceAddress address);

DynamicByteBuffer AuthenticationRequestedPacket(
    hci_spec::ConnectionHandle conn);

DynamicByteBuffer CommandCompletePacket(
    hci_spec::OpCode opcode,
    pw::bluetooth::emboss::StatusCode =
        pw::bluetooth::emboss::StatusCode::SUCCESS);

DynamicByteBuffer CommandCompletePacket(
    pw::bluetooth::emboss::OpCode opcode,
    pw::bluetooth::emboss::StatusCode =
        pw::bluetooth::emboss::StatusCode::SUCCESS);

DynamicByteBuffer CommandStatusPacket(
    hci_spec::OpCode op_code,
    pw::bluetooth::emboss::StatusCode status_code =
        pw::bluetooth::emboss::StatusCode::SUCCESS,
    uint8_t num_packets = 0xF0);

DynamicByteBuffer CommandStatusPacket(
    pw::bluetooth::emboss::OpCode op_code,
    pw::bluetooth::emboss::StatusCode status_code =
        pw::bluetooth::emboss::StatusCode::SUCCESS,
    uint8_t num_packets = 0xF0);

DynamicByteBuffer ConnectionCompletePacket(
    DeviceAddress address,
    hci_spec::ConnectionHandle conn,
    pw::bluetooth::emboss::StatusCode status =
        pw::bluetooth::emboss::StatusCode::SUCCESS);

DynamicByteBuffer ConnectionRequestPacket(
    DeviceAddress address,
    hci_spec::LinkType link_type = hci_spec::LinkType::kACL);

DynamicByteBuffer CreateConnectionPacket(DeviceAddress address);

DynamicByteBuffer CreateConnectionCancelPacket(DeviceAddress address);

DynamicByteBuffer DisconnectionCompletePacket(
    hci_spec::ConnectionHandle conn,
    pw::bluetooth::emboss::StatusCode reason =
        pw::bluetooth::emboss::StatusCode::REMOTE_USER_TERMINATED_CONNECTION);
DynamicByteBuffer DisconnectPacket(
    hci_spec::ConnectionHandle conn,
    pw::bluetooth::emboss::StatusCode reason =
        pw::bluetooth::emboss::StatusCode::REMOTE_USER_TERMINATED_CONNECTION);
DynamicByteBuffer DisconnectStatusResponsePacket();

DynamicByteBuffer EmptyCommandPacket(hci_spec::OpCode opcode);

DynamicByteBuffer EncryptionChangeEventPacket(
    pw::bluetooth::emboss::StatusCode status_code,
    hci_spec::ConnectionHandle conn,
    hci_spec::EncryptionStatus encryption_enabled);

DynamicByteBuffer EnhancedAcceptSynchronousConnectionRequestPacket(
    DeviceAddress peer_address,
    bt::StaticPacket<
        pw::bluetooth::emboss::SynchronousConnectionParametersWriter> params);

DynamicByteBuffer EnhancedSetupSynchronousConnectionPacket(
    hci_spec::ConnectionHandle conn,
    bt::StaticPacket<
        pw::bluetooth::emboss::SynchronousConnectionParametersWriter> params);

DynamicByteBuffer InquiryCommandPacket(
    uint16_t inquiry_length = gap::kInquiryLengthDefault);

DynamicByteBuffer IoCapabilityRequestNegativeReplyPacket(
    DeviceAddress address, pw::bluetooth::emboss::StatusCode status_code);
DynamicByteBuffer IoCapabilityRequestNegativeReplyResponse(
    DeviceAddress address);
DynamicByteBuffer IoCapabilityRequestPacket(DeviceAddress address);
DynamicByteBuffer IoCapabilityRequestReplyPacket(
    DeviceAddress address,
    pw::bluetooth::emboss::IoCapability io_cap,
    pw::bluetooth::emboss::AuthenticationRequirements auth_req);
DynamicByteBuffer IoCapabilityRequestReplyResponse(DeviceAddress address);
DynamicByteBuffer IoCapabilityResponsePacket(
    DeviceAddress address,
    pw::bluetooth::emboss::IoCapability io_cap,
    pw::bluetooth::emboss::AuthenticationRequirements auth_req);

// Generate a set of fragments from SDU data and a vector of sizes.
std::vector<DynamicByteBuffer> IsoDataFragments(
    hci_spec::ConnectionHandle connection_handle,
    std::optional<uint32_t> time_stamp,
    uint16_t packet_sequence_number,
    pw::bluetooth::emboss::IsoDataPacketStatus packet_status_flag,
    const std::vector<uint8_t>& sdu_data,
    const std::vector<size_t>& fragment_sizes);
DynamicByteBuffer IsoDataPacket(
    hci_spec::ConnectionHandle connection_handle,
    pw::bluetooth::emboss::IsoDataPbFlag pb_flag,
    std::optional<uint32_t> time_stamp,
    std::optional<uint16_t> packet_sequence_number,
    std::optional<uint16_t> iso_sdu_length,
    std::optional<pw::bluetooth::emboss::IsoDataPacketStatus> status_flag,
    pw::span<const uint8_t> sdu_data);

DynamicByteBuffer LEReadRemoteFeaturesCompletePacket(
    hci_spec::ConnectionHandle conn, hci_spec::LESupportedFeatures le_features);
DynamicByteBuffer LEReadRemoteFeaturesPacket(hci_spec::ConnectionHandle conn);

DynamicByteBuffer LECisRequestEventPacket(
    hci_spec::ConnectionHandle acl_connection_handle,
    hci_spec::ConnectionHandle cis_connection_handle,
    uint8_t cig_id,
    uint8_t cis_id);

DynamicByteBuffer LEAcceptCisRequestCommandPacket(
    hci_spec::ConnectionHandle cis_handle);

DynamicByteBuffer LERejectCisRequestCommandPacket(
    hci_spec::ConnectionHandle cis_handle,
    pw::bluetooth::emboss::StatusCode reason);

DynamicByteBuffer LESetCIGParametersCommandPacket(
    uint8_t cig_id,
    uint32_t sdu_interval_c_to_p,
    uint32_t sdu_interval_p_to_c,
    pw::bluetooth::emboss::LESleepClockAccuracyRange worst_case_sca,
    pw::bluetooth::emboss::LECISPacking packing,
    pw::bluetooth::emboss::LECISFraming framing,
    uint16_t max_transport_latency_c_to_p,
    uint16_t max_transport_latency_p_to_c,
    pw::span<const bt::iso::CisConfigParams> cis_params);

DynamicByteBuffer LESetCIGParametersCompletePacket(
    uint8_t cig_id,
    const std::vector<hci_spec::ConnectionHandle>& cis_handles,
    pw::bluetooth::emboss::StatusCode status =
        pw::bluetooth::emboss::StatusCode::SUCCESS);

struct CreateCisHandles {
  hci_spec::ConnectionHandle cis_handle;
  hci_spec::ConnectionHandle acl_handle;
};
DynamicByteBuffer LECreateCISCommandPacket(
    pw::span<const CreateCisHandles> cis_handles);

DynamicByteBuffer LECisEstablishedEventPacket(
    pw::bluetooth::emboss::StatusCode status,
    hci_spec::ConnectionHandle connection_handle,
    uint32_t cig_sync_delay_us,
    uint32_t cis_sync_delay_us,
    uint32_t transport_latency_c_to_p_us,
    uint32_t transport_latency_p_to_c_us,
    pw::bluetooth::emboss::IsoPhyType phy_c_to_p,
    pw::bluetooth::emboss::IsoPhyType phy_p_to_c,
    uint8_t nse,        // maximum number of subevents
    uint8_t bn_c_to_p,  // burst number C => P
    uint8_t bn_p_to_c,  // burst number P => C
    uint8_t
        ft_c_to_p,  // flush timeout (in multiples of the ISO_Interval) C => P
    uint8_t
        ft_p_to_c,  // flush timeout (in multiples of the ISO_Interval) P => C
    uint16_t max_pdu_c_to_p,
    uint16_t max_pdu_p_to_c,
    uint16_t iso_interval);

DynamicByteBuffer LESetupIsoDataPathPacket(
    hci_spec::ConnectionHandle connection_handle,
    pw::bluetooth::emboss::DataPathDirection direction,
    uint8_t data_path_id,
    bt::StaticPacket<pw::bluetooth::emboss::CodecIdWriter> codec_id,
    uint32_t controller_delay,
    const std::optional<std::vector<uint8_t>>& codec_configuration);
DynamicByteBuffer LESetupIsoDataPathResponse(
    pw::bluetooth::emboss::StatusCode status,
    hci_spec::ConnectionHandle connection_handle);

DynamicByteBuffer LERequestPeerScaCompletePacket(
    hci_spec::ConnectionHandle conn,
    pw::bluetooth::emboss::LESleepClockAccuracyRange sca);
DynamicByteBuffer LERequestPeerScaPacket(hci_spec::ConnectionHandle conn);

DynamicByteBuffer LEPeriodicAdvertisingCreateSyncPacket(
    DeviceAddress address,
    uint8_t sid,
    uint16_t sync_timeout,
    bool filter_duplicates = false,
    bool use_periodic_advertiser_list = false);

DynamicByteBuffer LEPeriodicAdvertisingCreateSyncCancelPacket();

DynamicByteBuffer LEAddDeviceToPeriodicAdvertiserListPacket(
    DeviceAddress address, uint8_t sid);

DynamicByteBuffer LERemoveDeviceFromPeriodicAdvertiserListPacket(
    DeviceAddress address, uint8_t sid);

DynamicByteBuffer LEPeriodicAdvertisingSyncEstablishedEventPacketV1(
    pw::bluetooth::emboss::StatusCode status,
    hci_spec::SyncHandle sync_handle,
    uint8_t advertising_sid,
    DeviceAddress address,
    pw::bluetooth::emboss::LEPhy phy,
    uint16_t interval,
    pw::bluetooth::emboss::LEClockAccuracy clock_accuracy);

DynamicByteBuffer LEPeriodicAdvertisingSyncEstablishedEventPacketV2(
    pw::bluetooth::emboss::StatusCode status,
    hci_spec::SyncHandle sync_handle,
    uint8_t advertising_sid,
    DeviceAddress address,
    pw::bluetooth::emboss::LEPhy phy,
    uint16_t interval,
    pw::bluetooth::emboss::LEClockAccuracy clock_accuracy,
    uint8_t num_subevents);

DynamicByteBuffer LEPeriodicAdvertisingReportEventPacketV1(
    hci_spec::SyncHandle sync_handle,
    pw::bluetooth::emboss::LEPeriodicAdvertisingDataStatus data_status,
    const DynamicByteBuffer& data);

DynamicByteBuffer LEPeriodicAdvertisingReportEventPacketV2(
    hci_spec::SyncHandle sync_handle,
    uint16_t event_counter,
    uint8_t subevent,
    pw::bluetooth::emboss::LEPeriodicAdvertisingDataStatus data_status,
    const DynamicByteBuffer& data);

DynamicByteBuffer LESyncLostEventPacket(hci_spec::SyncHandle sync_handle);

DynamicByteBuffer LEBigInfoAdvertisingReportEventPacket(
    hci_spec::SyncHandle sync_handle,
    uint8_t num_bis,
    uint8_t nse,
    uint16_t iso_interval,
    uint8_t bn,
    uint8_t pto,
    uint8_t irc,
    uint16_t max_pdu,
    uint32_t sdu_interval,
    uint16_t max_sdu,
    pw::bluetooth::emboss::IsoPhyType phy,
    pw::bluetooth::emboss::BigFraming framing,
    bool encryption);

DynamicByteBuffer LEPeriodicAdvertisingSyncTransferReceivedEventPacket(
    pw::bluetooth::emboss::StatusCode status,
    hci_spec::ConnectionHandle connection_handle,
    uint16_t service_data,
    hci_spec::SyncHandle sync_handle,
    uint8_t advertising_sid,
    DeviceAddress address,
    pw::bluetooth::emboss::LEPhy phy,
    uint16_t pa_interval,
    pw::bluetooth::emboss::LEClockAccuracy advertiser_clock_accuracy);

DynamicByteBuffer LEPeriodicAdvertisingTerminateSyncPacket(
    hci_spec::SyncHandle sync_handle);

DynamicByteBuffer LESetPeriodicAdvertisingSyncTransferParamsPacket(
    hci_spec::ConnectionHandle connection_handle,
    pw::bluetooth::emboss::PeriodicAdvertisingSyncTransferMode mode,
    uint16_t sync_timeout);

DynamicByteBuffer LEPeriodicAdvertisingSyncTransferPacket(
    hci_spec::ConnectionHandle connection_handle,
    uint16_t service_data,
    hci_spec::SyncHandle sync_handle);

DynamicByteBuffer LEStartEncryptionPacket(hci_spec::ConnectionHandle,
                                          uint64_t random_number,
                                          uint16_t encrypted_diversifier,
                                          UInt128 ltk);

DynamicByteBuffer LinkKeyNotificationPacket(DeviceAddress address,
                                            UInt128 link_key,
                                            hci_spec::LinkKeyType key_type);

DynamicByteBuffer LinkKeyRequestPacket(DeviceAddress address);
DynamicByteBuffer LinkKeyRequestNegativeReplyPacket(DeviceAddress address);
DynamicByteBuffer LinkKeyRequestNegativeReplyResponse(DeviceAddress address);
DynamicByteBuffer LinkKeyRequestReplyPacket(DeviceAddress address,
                                            UInt128 link_key);
DynamicByteBuffer LinkKeyRequestReplyResponse(DeviceAddress address);

DynamicByteBuffer NumberOfCompletedPacketsPacket(
    hci_spec::ConnectionHandle conn, uint16_t num_packets);
DynamicByteBuffer NumberOfCompletedPacketsPacketWithInvalidSize(
    hci_spec::ConnectionHandle conn, uint16_t num_packets);

DynamicByteBuffer PinCodeRequestPacket(DeviceAddress address);
DynamicByteBuffer PinCodeRequestNegativeReplyPacket(DeviceAddress address);
DynamicByteBuffer PinCodeRequestNegativeReplyResponse(DeviceAddress address);
DynamicByteBuffer PinCodeRequestReplyPacket(DeviceAddress address,
                                            uint8_t pin_length,
                                            std::string pin_code);
DynamicByteBuffer PinCodeRequestReplyResponse(DeviceAddress address);

// The ReadRemoteExtended*CompletePacket packets report a max page number of 3,
// even though there are only 2 pages, in order to test this behavior seen in
// real devices.
DynamicByteBuffer ReadRemoteExtended1CompletePacket(
    hci_spec::ConnectionHandle conn);
DynamicByteBuffer ReadRemoteExtended1CompletePacketNoSsp(
    hci_spec::ConnectionHandle conn);
DynamicByteBuffer ReadRemoteExtended1Packet(hci_spec::ConnectionHandle conn);
DynamicByteBuffer ReadRemoteExtended2CompletePacket(
    hci_spec::ConnectionHandle conn);
DynamicByteBuffer ReadRemoteExtended2Packet(hci_spec::ConnectionHandle conn);

DynamicByteBuffer ReadRemoteVersionInfoCompletePacket(
    hci_spec::ConnectionHandle conn);
DynamicByteBuffer ReadRemoteVersionInfoPacket(hci_spec::ConnectionHandle conn);

DynamicByteBuffer ReadRemoteSupportedFeaturesCompletePacket(
    hci_spec::ConnectionHandle conn, bool extended_features);
DynamicByteBuffer ReadRemoteSupportedFeaturesPacket(
    hci_spec::ConnectionHandle conn);

DynamicByteBuffer ReadScanEnable();
DynamicByteBuffer ReadScanEnableResponse(uint8_t scan_enable);

DynamicByteBuffer RejectConnectionRequestPacket(
    DeviceAddress address, pw::bluetooth::emboss::StatusCode reason);

DynamicByteBuffer RejectSynchronousConnectionRequest(
    DeviceAddress address, pw::bluetooth::emboss::StatusCode status_code);

DynamicByteBuffer RemoteNameRequestCompletePacket(
    DeviceAddress address, const std::string& name = "Fuchsia💖");
DynamicByteBuffer RemoteNameRequestPacket(DeviceAddress address);

DynamicByteBuffer RoleChangePacket(
    DeviceAddress address,
    pw::bluetooth::emboss::ConnectionRole role,
    pw::bluetooth::emboss::StatusCode status =
        pw::bluetooth::emboss::StatusCode::SUCCESS);

DynamicByteBuffer ScoDataPacket(
    hci_spec::ConnectionHandle conn,
    hci_spec::SynchronousDataPacketStatusFlag flag,
    const BufferView& payload,
    std::optional<uint8_t> payload_length_override = std::nullopt);

DynamicByteBuffer SetConnectionEncryption(hci_spec::ConnectionHandle conn,
                                          bool enable);

DynamicByteBuffer SimplePairingCompletePacket(
    DeviceAddress address, pw::bluetooth::emboss::StatusCode status_code);

DynamicByteBuffer StartA2dpOffloadRequest(
    const l2cap::A2dpOffloadManager::Configuration& config,
    hci_spec::ConnectionHandle connection_handle,
    l2cap::ChannelId l2cap_channel_id,
    uint16_t l2cap_mtu_size);

DynamicByteBuffer StopA2dpOffloadRequest();

DynamicByteBuffer SynchronousConnectionCompletePacket(
    hci_spec::ConnectionHandle conn,
    DeviceAddress address,
    hci_spec::LinkType link_type,
    pw::bluetooth::emboss::StatusCode status);

DynamicByteBuffer UserConfirmationRequestNegativeReplyPacket(
    DeviceAddress address);
DynamicByteBuffer UserConfirmationRequestPacket(DeviceAddress address,
                                                uint32_t passkey);
DynamicByteBuffer UserConfirmationRequestReplyPacket(DeviceAddress address);

DynamicByteBuffer UserPasskeyNotificationPacket(DeviceAddress address,
                                                uint32_t passkey);

DynamicByteBuffer UserPasskeyRequestNegativeReply(DeviceAddress address);
DynamicByteBuffer UserPasskeyRequestNegativeReplyResponse(
    DeviceAddress address);
DynamicByteBuffer UserPasskeyRequestPacket(DeviceAddress address);
DynamicByteBuffer UserPasskeyRequestReplyPacket(DeviceAddress address,
                                                uint32_t passkey);
DynamicByteBuffer UserPasskeyRequestReplyResponse(DeviceAddress address);

DynamicByteBuffer WriteAutomaticFlushTimeoutPacket(
    hci_spec::ConnectionHandle conn, uint16_t flush_timeout);

DynamicByteBuffer WriteInquiryScanActivity(uint16_t scan_interval,
                                           uint16_t scan_window);

DynamicByteBuffer WritePageScanActivityPacket(uint16_t scan_interval,
                                              uint16_t scan_window);

DynamicByteBuffer WritePageScanTypePacket(uint8_t scan_type);
DynamicByteBuffer WritePageScanTypeResponse();

DynamicByteBuffer WritePageTimeoutPacket(uint16_t page_timeout);

DynamicByteBuffer WritePinTypePacket(uint8_t pin_type);

DynamicByteBuffer WriteScanEnable(uint8_t scan_enable);

}  // namespace bt::testing
