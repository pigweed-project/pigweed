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

#include "pw_bluetooth_sapphire/internal/host/testing/test_packets.h"

#include <pw_assert/check.h>

#include <numeric>

#include "pw_bluetooth/hci_common.emb.h"
#include "pw_bluetooth/hci_data.emb.h"
#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/vendor_protocol.h"
#include "pw_bluetooth_sapphire/internal/host/hci/bredr_connection_request.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"
#include "pw_string/string_builder.h"

namespace bt::testing {

namespace android_hci = bt::hci_spec::vendor::android;
namespace android_emb = pw::bluetooth::vendor::android_hci;

// Generates a blob of data that is unique to the size and starting value
std::vector<uint8_t> GenDataBlob(size_t size, uint8_t starting_value) {
  std::vector<uint8_t> buffer(size);
  std::iota(buffer.begin(), buffer.end(), starting_value);
  return buffer;
}

DynamicByteBuffer AcceptConnectionRequestPacket(DeviceAddress address) {
  const auto addr = address.value().bytes();
  return DynamicByteBuffer(StaticByteBuffer(
      LowerBits(hci_spec::kAcceptConnectionRequest),
      UpperBits(hci_spec::kAcceptConnectionRequest),
      0x07,  // parameter_total_size (7 bytes)
      // peer BD_ADDR (6 bytes)
      addr[0],
      addr[1],
      addr[2],
      addr[3],
      addr[4],
      addr[5],
      0x00  // Role (Become central)
      ));
}

DynamicByteBuffer AuthenticationRequestedPacket(
    hci_spec::ConnectionHandle conn) {
  return DynamicByteBuffer(StaticByteBuffer(
      LowerBits(hci_spec::kAuthenticationRequested),
      UpperBits(hci_spec::kAuthenticationRequested),
      0x02,             // parameter_total_size (2 bytes)
      LowerBits(conn),  // Connection_Handle
      UpperBits(conn)   // Connection_Handle
      ));
}

DynamicByteBuffer CommandCompletePacket(
    hci_spec::OpCode opcode, pw::bluetooth::emboss::StatusCode status) {
  return DynamicByteBuffer(StaticByteBuffer(
      hci_spec::kCommandCompleteEventCode,
      0x04,  // parameter_total_size (4 bytes)
      0xF0,  // Num_HCI_Command_Packets allowed to be sent to controller (240)
      LowerBits(opcode),  // Command_Opcode
      UpperBits(opcode),  // Command_Opcode
      status              // Status
      ));
}

DynamicByteBuffer CommandCompletePacket(
    pw::bluetooth::emboss::OpCode opcode,
    pw::bluetooth::emboss::StatusCode status) {
  return CommandCompletePacket(static_cast<hci_spec::OpCode>(opcode), status);
}

DynamicByteBuffer CommandStatusPacket(
    hci_spec::OpCode op_code,
    pw::bluetooth::emboss::StatusCode status_code,
    uint8_t num_packets) {
  return DynamicByteBuffer(StaticByteBuffer(
      hci_spec::kCommandStatusEventCode,
      0x04,         // parameter_total_size (4 bytes)
      status_code,  // Status
      num_packets,  // Num_HCI_Command_Packets allowed to be sent to controller
      LowerBits(op_code),  // Command_Opcode
      UpperBits(op_code)   // Command_Opcode
      ));
}

DynamicByteBuffer CommandStatusPacket(
    pw::bluetooth::emboss::OpCode op_code,
    pw::bluetooth::emboss::StatusCode status_code,
    uint8_t num_packets) {
  return CommandStatusPacket(
      static_cast<hci_spec::OpCode>(op_code), status_code, num_packets);
}

DynamicByteBuffer ConnectionCompletePacket(
    DeviceAddress address,
    hci_spec::ConnectionHandle conn,
    pw::bluetooth::emboss::StatusCode status) {
  const auto addr = address.value().bytes();
  return DynamicByteBuffer(StaticByteBuffer(
      hci_spec::kConnectionCompleteEventCode,
      0x0B,             // parameter_total_size (11 bytes)
      status,           // Status
      LowerBits(conn),  // Connection_Handle
      UpperBits(conn),  // Connection_Handle
      // peer BD_ADDR (6 bytes)
      addr[0],
      addr[1],
      addr[2],
      addr[3],
      addr[4],
      addr[5],
      0x01,  // Link_Type (ACL)
      0x00   // Encryption_Enabled (Disabled)
      ));
}

DynamicByteBuffer ConnectionRequestPacket(DeviceAddress address,
                                          hci_spec::LinkType link_type) {
  const auto addr = address.value().bytes();
  return DynamicByteBuffer(StaticByteBuffer(
      hci_spec::kConnectionRequestEventCode,
      0x0A,  // parameter_total_size (10 bytes)
      // peer BD_ADDR (6 bytes)
      addr[0],
      addr[1],
      addr[2],
      addr[3],
      addr[4],
      addr[5],
      0x5A,      // Class_Of_Device
      0x42,      // (Smart Phone minor class)
      0x0C,      // Networking, Capturing, Object Transfer, Telephony, LE Audio
      link_type  // Link_Type
      ));
}

DynamicByteBuffer CreateConnectionPacket(DeviceAddress address) {
  const auto addr = address.value().bytes();
  return DynamicByteBuffer(StaticByteBuffer(
      LowerBits(hci_spec::kCreateConnection),
      UpperBits(hci_spec::kCreateConnection),
      0x0d,  // parameter_total_size (13 bytes)
      // peer BD_ADDR (6 bytes)
      addr[0],
      addr[1],
      addr[2],
      addr[3],
      addr[4],
      addr[5],
      LowerBits(hci::kEnableAllPacketTypes),  // Packet_Type
      UpperBits(hci::kEnableAllPacketTypes),  // Packet_Type
      0x02,                                   // Page_Scan_Repetition_Mode (R2)
      0x00,                                   // Reserved
      0x00,                                   // Clock_Offset
      0x00,                                   // Clock_Offset
      0x00                                    // Allow_Role_Switch (Not allowed)
      ));
}

DynamicByteBuffer CreateConnectionCancelPacket(DeviceAddress address) {
  const auto addr = address.value().bytes();
  return DynamicByteBuffer(
      StaticByteBuffer(LowerBits(hci_spec::kCreateConnectionCancel),
                       UpperBits(hci_spec::kCreateConnectionCancel),
                       0x06,  // parameter_total_size (6 bytes)
                       // peer BD_ADDR (6 bytes)
                       addr[0],
                       addr[1],
                       addr[2],
                       addr[3],
                       addr[4],
                       addr[5]));
}

DynamicByteBuffer DisconnectionCompletePacket(
    hci_spec::ConnectionHandle conn, pw::bluetooth::emboss::StatusCode reason) {
  return DynamicByteBuffer(StaticByteBuffer(
      hci_spec::kDisconnectionCompleteEventCode,
      0x04,  // parameter_total_size (4 bytes)
      pw::bluetooth::emboss::StatusCode::SUCCESS,  // Status
      LowerBits(conn),                             // Connection_Handle
      UpperBits(conn),                             // Connection_Handle
      reason                                       // Reason
      ));
}

DynamicByteBuffer DisconnectPacket(hci_spec::ConnectionHandle conn,
                                   pw::bluetooth::emboss::StatusCode reason) {
  return DynamicByteBuffer(StaticByteBuffer(
      LowerBits(hci_spec::kDisconnect),
      UpperBits(hci_spec::kDisconnect),
      0x03,             // parameter_total_size (3 bytes)
      LowerBits(conn),  // Connection_Handle
      UpperBits(conn),  // Connection_Handle
      reason            // Reason
      ));
}

DynamicByteBuffer DisconnectStatusResponsePacket() {
  return DynamicByteBuffer(StaticByteBuffer(
      hci_spec::kCommandStatusEventCode,
      0x04,  // parameter_total_size (3 bytes)
      pw::bluetooth::emboss::StatusCode::SUCCESS,  // Status
      0xF0,  // Num_HCI_Command_Packets allowed to be sent to controller (240)
      LowerBits(hci_spec::kDisconnect),  // Command_Opcode
      UpperBits(hci_spec::kDisconnect)   // Command_Opcode
      ));
}

DynamicByteBuffer EmptyCommandPacket(hci_spec::OpCode opcode) {
  return DynamicByteBuffer(
      StaticByteBuffer(LowerBits(opcode), UpperBits(opcode), /*length=*/0));
}

DynamicByteBuffer EncryptionChangeEventPacket(
    pw::bluetooth::emboss::StatusCode status_code,
    hci_spec::ConnectionHandle conn,
    hci_spec::EncryptionStatus encryption_enabled) {
  return DynamicByteBuffer(StaticByteBuffer(
      hci_spec::kEncryptionChangeEventCode,
      0x04,             // parameter_total_size (4 bytes)
      status_code,      // Status
      LowerBits(conn),  // Connection_Handle
      UpperBits(conn),  // Connection_Handle
      static_cast<uint8_t>(encryption_enabled)  // Encryption_Enabled
      ));
}

DynamicByteBuffer EnhancedAcceptSynchronousConnectionRequestPacket(
    DeviceAddress peer_address,
    bt::StaticPacket<
        pw::bluetooth::emboss::SynchronousConnectionParametersWriter> params) {
  auto packet = hci::CommandPacket::New<
      pw::bluetooth::emboss::
          EnhancedAcceptSynchronousConnectionRequestCommandWriter>(
      hci_spec::kEnhancedAcceptSynchronousConnectionRequest);
  auto view = packet.view_t();

  view.bd_addr().CopyFrom(peer_address.value().view());
  view.connection_parameters().CopyFrom(params.view());

  return DynamicByteBuffer(packet.data());
}

DynamicByteBuffer EnhancedSetupSynchronousConnectionPacket(
    hci_spec::ConnectionHandle conn,
    bt::StaticPacket<
        pw::bluetooth::emboss::SynchronousConnectionParametersWriter> params) {
  auto packet = hci::CommandPacket::New<
      pw::bluetooth::emboss::EnhancedSetupSynchronousConnectionCommandWriter>(
      hci_spec::kEnhancedSetupSynchronousConnection);

  auto view = packet.view_t();
  view.connection_handle().Write(conn);
  view.connection_parameters().CopyFrom(params.view());

  return DynamicByteBuffer(packet.data());
}

DynamicByteBuffer InquiryCommandPacket(uint16_t inquiry_length) {
  return DynamicByteBuffer(StaticByteBuffer(
      LowerBits(hci_spec::kInquiry),
      UpperBits(hci_spec::kInquiry),
      0x05,            // parameter_total_size (5 bytes)
      0x33,            // LAP (GIAC)
      0x8B,            // LAP (GIAC)
      0x9E,            // LAP (GIAC)
      inquiry_length,  // Inquiry_Length
      0x00             // Num_Responses (Unlimited)
      ));
}

DynamicByteBuffer IoCapabilityRequestNegativeReplyPacket(
    DeviceAddress address, pw::bluetooth::emboss::StatusCode status_code) {
  const auto addr = address.value().bytes();
  return DynamicByteBuffer(StaticByteBuffer(
      LowerBits(hci_spec::kIOCapabilityRequestNegativeReply),
      UpperBits(hci_spec::kIOCapabilityRequestNegativeReply),
      0x07,  // parameter_total_size (7 bytes)
      // peer BD_ADDR (6 bytes)
      addr[0],
      addr[1],
      addr[2],
      addr[3],
      addr[4],
      addr[5],
      status_code  // Reason
      ));
}

DynamicByteBuffer IoCapabilityRequestNegativeReplyResponse(
    DeviceAddress address) {
  const auto addr = address.value().bytes();
  return DynamicByteBuffer(StaticByteBuffer(
      hci_spec::kCommandCompleteEventCode,
      0x0A,  // parameter_total_size (10 bytes)
      0xF0,  // Num_HCI_Command_Packets allowed to be sent to controller (240)
      LowerBits(hci_spec::kIOCapabilityRequestNegativeReply),  // Command_Opcode
      UpperBits(hci_spec::kIOCapabilityRequestNegativeReply),  // Command_Opcode
      pw::bluetooth::emboss::StatusCode::SUCCESS,              // Status
      // peer BD_ADDR (6 bytes)
      addr[0],
      addr[1],
      addr[2],
      addr[3],
      addr[4],
      addr[5]));
}

DynamicByteBuffer IoCapabilityRequestPacket(DeviceAddress address) {
  const auto addr = address.value().bytes();
  return DynamicByteBuffer(
      StaticByteBuffer(hci_spec::kIOCapabilityRequestEventCode,
                       0x06,  // parameter_total_size (6 bytes)
                       // peer BD_ADDR (6 bytes)
                       addr[0],
                       addr[1],
                       addr[2],
                       addr[3],
                       addr[4],
                       addr[5]));
}

DynamicByteBuffer IoCapabilityRequestReplyPacket(
    DeviceAddress address,
    pw::bluetooth::emboss::IoCapability io_cap,
    pw::bluetooth::emboss::AuthenticationRequirements auth_req) {
  const auto addr = address.value().bytes();
  return DynamicByteBuffer(StaticByteBuffer(
      LowerBits(hci_spec::kIOCapabilityRequestReply),
      UpperBits(hci_spec::kIOCapabilityRequestReply),
      0x09,  // parameter_total_size (9 bytes)
      // peer BD_ADDR (6 bytes)
      addr[0],
      addr[1],
      addr[2],
      addr[3],
      addr[4],
      addr[5],
      io_cap,   // IO_Capability
      0x00,     // OOB_Data_Present (Not present)
      auth_req  // Authentication_Requirements
      ));
}

DynamicByteBuffer IoCapabilityRequestReplyResponse(DeviceAddress address) {
  const auto addr = address.value().bytes();
  return DynamicByteBuffer(StaticByteBuffer(
      hci_spec::kCommandCompleteEventCode,
      0x0A,  // parameter_total_size (10 bytes)
      0xF0,  // Num_HCI_Command_Packets allowed to be sent to controller (240)
      LowerBits(hci_spec::kIOCapabilityRequestReply),  // Command_Opcode
      UpperBits(hci_spec::kIOCapabilityRequestReply),  // Command_Opcode
      pw::bluetooth::emboss::StatusCode::SUCCESS,      // Status
      // peer BD_ADDR (6 bytes)
      addr[0],
      addr[1],
      addr[2],
      addr[3],
      addr[4],
      addr[5]));
}

DynamicByteBuffer IoCapabilityResponsePacket(
    DeviceAddress address,
    pw::bluetooth::emboss::IoCapability io_cap,
    pw::bluetooth::emboss::AuthenticationRequirements auth_req) {
  const auto addr = address.value().bytes();
  return DynamicByteBuffer(StaticByteBuffer(
      hci_spec::kIOCapabilityResponseEventCode,
      0x09,  // parameter_total_size (9 bytes)
      // peer BD_ADDR (6 bytes)
      addr[0],
      addr[1],
      addr[2],
      addr[3],
      addr[4],
      addr[5],
      io_cap,   // IO_Capability
      0x00,     // OOB_Data_Present (Not present)
      auth_req  // Authentication_Requirements
      ));
}

DynamicByteBuffer IsoDataPacket(
    hci_spec::ConnectionHandle connection_handle,
    pw::bluetooth::emboss::IsoDataPbFlag pb_flag,
    std::optional<uint32_t> time_stamp,
    std::optional<uint16_t> packet_sequence_number,
    std::optional<uint16_t> iso_sdu_length,
    std::optional<pw::bluetooth::emboss::IsoDataPacketStatus>
        packet_status_flag,
    pw::span<const uint8_t> sdu_data) {
  size_t time_stamp_size = time_stamp.has_value() ? 4 : 0;
  bool has_sdu_hdr =
      (pb_flag == pw::bluetooth::emboss::IsoDataPbFlag::FIRST_FRAGMENT) ||
      (pb_flag == pw::bluetooth::emboss::IsoDataPbFlag::COMPLETE_SDU);
  size_t sdu_hdr_size = has_sdu_hdr ? 4 : 0;
  size_t data_total_length = time_stamp_size + sdu_hdr_size + sdu_data.size();
  size_t frame_total_size =
      pw::bluetooth::emboss::IsoDataFrameHeaderView::SizeInBytes() +
      data_total_length;
  DynamicByteBuffer packet(frame_total_size);
  auto view = pw::bluetooth::emboss::MakeIsoDataFramePacketView(
      packet.mutable_data(), frame_total_size);

  view.header().connection_handle().Write(connection_handle);
  view.header().pb_flag().Write(pb_flag);
  view.header().ts_flag().Write(
      time_stamp.has_value()
          ? pw::bluetooth::emboss::TsFlag::TIMESTAMP_PRESENT
          : pw::bluetooth::emboss::TsFlag::TIMESTAMP_NOT_PRESENT);
  view.header().data_total_length().Write(data_total_length);

  if (time_stamp.has_value()) {
    view.time_stamp().Write(*time_stamp);
  }

  if (has_sdu_hdr) {
    PW_CHECK(packet_sequence_number.has_value());
    PW_CHECK(iso_sdu_length.has_value());
    PW_CHECK(packet_status_flag.has_value());
    view.packet_sequence_number().Write(*packet_sequence_number);
    view.iso_sdu_length().Write(*iso_sdu_length);
    view.packet_status_flag().Write(*packet_status_flag);
  }

  // Write payload
  memcpy(view.iso_sdu_fragment().BackingStorage().data(),
         sdu_data.data(),
         sdu_data.size());

  return packet;
}

std::vector<DynamicByteBuffer> IsoDataFragments(
    hci_spec::ConnectionHandle connection_handle,
    std::optional<uint32_t> time_stamp,
    uint16_t packet_sequence_number,
    pw::bluetooth::emboss::IsoDataPacketStatus packet_status_flag,
    const std::vector<uint8_t>& sdu_data,
    const std::vector<size_t>& fragment_sizes) {
  std::vector<DynamicByteBuffer> fragments;
  uint16_t sdu_total_size = sdu_data.size();

  size_t fragment_start = 0;
  size_t fragment_end;
  for (size_t fragment_size : fragment_sizes) {
    fragment_end = fragment_start + fragment_size;
    bool is_first = (fragment_start == 0);
    bool is_last = (fragment_end == sdu_total_size);
    pw::bluetooth::emboss::IsoDataPbFlag pb_flag;
    if (is_first) {
      pb_flag = is_last ? pw::bluetooth::emboss::IsoDataPbFlag::COMPLETE_SDU
                        : pw::bluetooth::emboss::IsoDataPbFlag::FIRST_FRAGMENT;
    } else {
      pb_flag =
          is_last ? pw::bluetooth::emboss::IsoDataPbFlag::LAST_FRAGMENT
                  : pw::bluetooth::emboss::IsoDataPbFlag::INTERMEDIATE_FRAGMENT;
    }
    DynamicByteBuffer next_fragment = IsoDataPacket(
        connection_handle,
        pb_flag,
        is_first ? time_stamp : std::nullopt,
        is_first ? std::optional<uint16_t>(packet_sequence_number)
                 : std::nullopt,
        is_first ? std::optional<uint16_t>(sdu_total_size) : std::nullopt,
        is_first ? std::optional<pw::bluetooth::emboss::IsoDataPacketStatus>(
                       packet_status_flag)
                 : std::nullopt,
        pw::span<const uint8_t>(sdu_data.data() + fragment_start,
                                fragment_size));
    fragments.push_back(next_fragment);
    fragment_start = fragment_end;
  }

  return fragments;
}

DynamicByteBuffer LEReadRemoteFeaturesCompletePacket(
    hci_spec::ConnectionHandle conn,
    hci_spec::LESupportedFeatures le_features) {
  const BufferView features(&le_features, sizeof(le_features));
  return DynamicByteBuffer(StaticByteBuffer(
      hci_spec::kLEMetaEventCode,
      0x0c,  // parameter_total_size (12 bytes)
      hci_spec::kLEReadRemoteFeaturesCompleteSubeventCode,  // Subevent_Code
      pw::bluetooth::emboss::StatusCode::SUCCESS,           // Status
      LowerBits(conn),                                      // Connection_Handle
      UpperBits(conn),                                      // Connection_Handle
      // LE_Features (8 bytes)
      features[0],
      features[1],
      features[2],
      features[3],
      features[4],
      features[5],
      features[6],
      features[7]));
}

DynamicByteBuffer LEReadRemoteFeaturesPacket(hci_spec::ConnectionHandle conn) {
  return DynamicByteBuffer(StaticByteBuffer(
      LowerBits(hci_spec::kLEReadRemoteFeatures),
      UpperBits(hci_spec::kLEReadRemoteFeatures),
      0x02,             // parameter_total_size (2 bytes)
      LowerBits(conn),  // Connection_Handle
      UpperBits(conn)   // Connection_Handle
      ));
}

DynamicByteBuffer LECisRequestEventPacket(
    hci_spec::ConnectionHandle acl_connection_handle,
    hci_spec::ConnectionHandle cis_connection_handle,
    hci_spec::CigIdentifier cig_id,
    hci_spec::CisIdentifier cis_id) {
  return DynamicByteBuffer(StaticByteBuffer(
      hci_spec::kLEMetaEventCode,
      0x07,                                 // parameter_total_size (7 bytes)
      hci_spec::kLECISRequestSubeventCode,  // Subevent_Code
      LowerBits(acl_connection_handle),     // ACL_Connection_Handle
      UpperBits(acl_connection_handle),     // ACL_Connection_Handle
      LowerBits(cis_connection_handle),     // CIS_Connection_Handle
      UpperBits(cis_connection_handle),     // CIS_Connection_Handle
      cig_id,                               // CIG_ID
      cis_id)                               // CIS_ID
  );
}

DynamicByteBuffer LEAcceptCisRequestCommandPacket(
    hci_spec::ConnectionHandle cis_handle) {
  return DynamicByteBuffer(
      StaticByteBuffer(LowerBits(hci_spec::kLEAcceptCISRequest),
                       UpperBits(hci_spec::kLEAcceptCISRequest),
                       0x02,
                       LowerBits(cis_handle),
                       UpperBits(cis_handle)));
}

DynamicByteBuffer LERejectCisRequestCommandPacket(
    hci_spec::ConnectionHandle cis_handle,
    pw::bluetooth::emboss::StatusCode reason) {
  return DynamicByteBuffer(
      StaticByteBuffer(LowerBits(hci_spec::kLERejectCISRequest),
                       UpperBits(hci_spec::kLERejectCISRequest),
                       0x03,                   // parameter_total_size (3 bytes)
                       LowerBits(cis_handle),  // Connection_Handle
                       UpperBits(cis_handle),  // Connection_Handle
                       reason)                 // Reason
  );
}

DynamicByteBuffer LESetCIGParametersCommandPacket(
    uint8_t cig_id,
    uint32_t sdu_interval_c_to_p,
    uint32_t sdu_interval_p_to_c,
    pw::bluetooth::emboss::LESleepClockAccuracyRange worst_case_sca,
    pw::bluetooth::emboss::LECISPacking packing,
    pw::bluetooth::emboss::LECISFraming framing,
    uint16_t max_transport_latency_c_to_p,
    uint16_t max_transport_latency_p_to_c,
    pw::span<const bt::iso::CisConfigParams> cis_params) {
  constexpr size_t kStaticSize = 18;
  constexpr size_t kSizePerCis = 9;
  constexpr size_t kCisCountMax = 0xEF;

  constexpr uint8_t kAllPhys = 0b00000111;
  constexpr uint8_t kRetransmissionCount = 2;

  auto packet_size = kStaticSize + kSizePerCis * cis_params.size();
  auto params_size =
      packet_size -
      pw::bluetooth::emboss::CommandHeader::IntrinsicSizeInBytes();

  PW_CHECK(cis_params.size() <= kCisCountMax);
  PW_CHECK(params_size <= std::numeric_limits<uint8_t>::max());

  auto sdu_interval_c_to_p_bytes = ToBytes(sdu_interval_c_to_p);
  auto sdu_interval_p_to_c_bytes = ToBytes(sdu_interval_p_to_c);

  StaticByteBuffer static_part(
      LowerBits(hci_spec::kLESetCIGParameters),  // Command Code ...
      UpperBits(hci_spec::kLESetCIGParameters),  //
      static_cast<uint8_t>(params_size),         // Params size
      cig_id,                                    // CIG Id
      sdu_interval_c_to_p_bytes[0],              // SDU interval ...
      sdu_interval_c_to_p_bytes[1],              // Central -> Peripheral ...
      sdu_interval_c_to_p_bytes[2],              //
      sdu_interval_p_to_c_bytes[0],              // SDU Interval ...
      sdu_interval_p_to_c_bytes[1],              // Peripheral -> Central ...
      sdu_interval_p_to_c_bytes[2],              //
      static_cast<uint8_t>(worst_case_sca),      // Worst-case clock accuracy
      static_cast<uint8_t>(packing),             // CIS packing
      static_cast<uint8_t>(framing),             // CIS framing
      LowerBits(max_transport_latency_c_to_p),   // Max latency ...
      UpperBits(max_transport_latency_c_to_p),   // Central -> Peripheral
      LowerBits(max_transport_latency_p_to_c),   // Max latency ...
      UpperBits(max_transport_latency_p_to_c),   // Peripheral -> Central
      static_cast<uint8_t>(cis_params.size())    // CIS count
  );

  static_assert(static_part.static_size() == kStaticSize);

  DynamicByteBuffer packet(packet_size);

  size_t next_cis_pos = kStaticSize;
  static_part.Copy(&packet);
  for (const auto& cis_param : cis_params) {
    StaticByteBuffer next_cis(
        cis_param.cis_id,                     // CIS ID
        LowerBits(cis_param.max_sdu_c_to_p),  // Max SDU size ...
        UpperBits(cis_param.max_sdu_c_to_p),  // Central -> Peripheral
        LowerBits(cis_param.max_sdu_p_to_c),  // Max SDU size ...
        UpperBits(cis_param.max_sdu_p_to_c),  // Peripheral -> Central
        kAllPhys,                             // Allow all PHYs C -> P
        kAllPhys,                             // Allow all PHYs P -> C
        kRetransmissionCount,                 // Retransmission count C -> P
        kRetransmissionCount                  // Retransmission count P -> C
    );

    static_assert(next_cis.static_size() == kSizePerCis);
    auto dest = packet.mutable_view(next_cis_pos, kSizePerCis);
    next_cis.Copy(&dest);
    next_cis_pos += kSizePerCis;
  }

  PW_CHECK(next_cis_pos == packet.size());
  return packet;
}

DynamicByteBuffer LESetCIGParametersCompletePacket(
    uint8_t cig_id,
    const std::vector<hci_spec::ConnectionHandle>& cis_handles,
    pw::bluetooth::emboss::StatusCode status) {
  constexpr size_t kStaticSize = 8;
  constexpr size_t kSizePerCis = 2;
  constexpr size_t kCisCountMax = 0xEF;
  constexpr uint8_t kNumHciCommandPacketsAllowed = 240;

  auto packet_size = kStaticSize + kSizePerCis * cis_handles.size();
  auto params_size =
      packet_size - pw::bluetooth::emboss::EventHeader::IntrinsicSizeInBytes();

  PW_CHECK(cis_handles.size() <= kCisCountMax);
  PW_CHECK(params_size <= std::numeric_limits<uint8_t>::max());
  StaticByteBuffer static_part(
      hci_spec::kCommandCompleteEventCode,       // Event code
      params_size,                               // Params size
      kNumHciCommandPacketsAllowed,              // Command packets allowed
      LowerBits(hci_spec::kLESetCIGParameters),  // Command code ...
      UpperBits(hci_spec::kLESetCIGParameters),  //
      status,                                    // Status code
      cig_id,                                    // CIG ID
      cis_handles.size()                         // CIS count
  );

  static_assert(kStaticSize ==
                pw::bluetooth::emboss::LESetCIGParametersCommandCompleteEvent::
                    MinSizeInBytes());
  static_assert(static_part.static_size() == kStaticSize);
  DynamicByteBuffer packet(packet_size);

  size_t next_cis_pos = kStaticSize;
  static_part.Copy(&packet, 0, next_cis_pos);
  for (const auto& cis_handle : cis_handles) {
    StaticByteBuffer next_cis(LowerBits(cis_handle), UpperBits(cis_handle));

    static_assert(next_cis.static_size() == kSizePerCis);
    auto dest = packet.mutable_view(next_cis_pos, kSizePerCis);
    next_cis.Copy(&dest);
    next_cis_pos += kSizePerCis;
  }

  return packet;
}

DynamicByteBuffer LECreateCISCommandPacket(
    pw::span<const CreateCisHandles> cis_handles) {
  constexpr size_t kStaticSize = 4;
  constexpr size_t kSizePerCis = 4;
  constexpr size_t kCisCountMax = 0xEF;

  auto packet_size = kStaticSize + kSizePerCis * cis_handles.size();
  auto params_size =
      packet_size -
      pw::bluetooth::emboss::CommandHeader::IntrinsicSizeInBytes();

  PW_CHECK(cis_handles.size() <= kCisCountMax);
  PW_CHECK(params_size <= std::numeric_limits<uint8_t>::max());
  StaticByteBuffer static_part(
      LowerBits(hci_spec::kLECreateCIS),        // Command Code ...
      UpperBits(hci_spec::kLECreateCIS),        //
      static_cast<uint8_t>(params_size),        // Params size
      static_cast<uint8_t>(cis_handles.size())  // CIS Count
  );

  static_assert(kStaticSize ==
                pw::bluetooth::emboss::LECreateCISCommand::MinSizeInBytes());
  static_assert(static_part.static_size() == kStaticSize);
  DynamicByteBuffer packet(packet_size);

  size_t next_cis_pos = kStaticSize;
  static_part.Copy(&packet, 0, next_cis_pos);
  for (const auto& handles : cis_handles) {
    StaticByteBuffer next_cis(LowerBits(handles.cis_handle),   // CIS handle ...
                              UpperBits(handles.cis_handle),   //
                              LowerBits(handles.acl_handle),   // ACL handle ...
                              UpperBits(handles.acl_handle));  //

    static_assert(next_cis.static_size() == kSizePerCis);
    auto dest = packet.mutable_view(next_cis_pos, kSizePerCis);
    next_cis.Copy(&dest);
    next_cis_pos += kSizePerCis;
  }

  return packet;
}

DynamicByteBuffer LERequestPeerScaCompletePacket(
    hci_spec::ConnectionHandle conn,
    pw::bluetooth::emboss::LESleepClockAccuracyRange sca) {
  auto packet = hci::EventPacket::New<
      pw::bluetooth::emboss::LERequestPeerSCACompleteSubeventWriter>(
      hci_spec::kLEMetaEventCode);

  auto view = packet.view_t();
  view.le_meta_event().subevent_code().Write(
      hci_spec::kLERequestPeerSCACompleteSubeventCode);
  view.status().Write(pw::bluetooth::emboss::StatusCode::SUCCESS);
  view.connection_handle().Write(conn);
  view.peer_clock_accuracy().Write(sca);

  return DynamicByteBuffer(packet.data());
}

DynamicByteBuffer LECisEstablishedEventPacket(
    pw::bluetooth::emboss::StatusCode status,
    hci_spec::ConnectionHandle connection_handle,
    uint32_t cig_sync_delay_us,
    uint32_t cis_sync_delay_us,
    uint32_t transport_latency_c_to_p_us,
    uint32_t transport_latency_p_to_c_us,
    pw::bluetooth::emboss::IsoPhyType phy_c_to_p,
    pw::bluetooth::emboss::IsoPhyType phy_p_to_c,
    uint8_t nse,
    uint8_t bn_c_to_p,
    uint8_t bn_p_to_c,
    uint8_t ft_c_to_p,
    uint8_t ft_p_to_c,
    uint16_t max_pdu_c_to_p,
    uint16_t max_pdu_p_to_c,
    uint16_t iso_interval) {
  auto packet = hci::EventPacket::New<
      pw::bluetooth::emboss::LECISEstablishedSubeventWriter>(
      hci_spec::kLEMetaEventCode);
  auto view = packet.view_t();

  view.le_meta_event().subevent_code().Write(
      hci_spec::kLECISEstablishedSubeventCode);
  view.status().Write(status);
  view.connection_handle().Write(connection_handle);
  view.cig_sync_delay().Write(cig_sync_delay_us);
  view.cis_sync_delay().Write(cis_sync_delay_us);
  view.transport_latency_c_to_p().Write(transport_latency_c_to_p_us);
  view.transport_latency_p_to_c().Write(transport_latency_p_to_c_us);
  view.phy_c_to_p().Write(phy_c_to_p);
  view.phy_p_to_c().Write(phy_p_to_c);
  view.nse().Write(nse);
  view.bn_c_to_p().Write(bn_c_to_p);
  view.bn_p_to_c().Write(bn_p_to_c);
  view.ft_c_to_p().Write(ft_c_to_p);
  view.ft_p_to_c().Write(ft_p_to_c);
  view.max_pdu_c_to_p().Write(max_pdu_c_to_p);
  view.max_pdu_p_to_c().Write(max_pdu_p_to_c);
  view.iso_interval().Write(iso_interval);

  return DynamicByteBuffer(packet.data());
}

DynamicByteBuffer LESetupIsoDataPathPacket(
    hci_spec::ConnectionHandle connection_handle,
    pw::bluetooth::emboss::DataPathDirection direction,
    uint8_t data_path_id,
    bt::StaticPacket<pw::bluetooth::emboss::CodecIdWriter> codec_id,
    uint32_t controller_delay,
    const std::optional<std::vector<uint8_t>>& codec_configuration) {
  size_t packet_size =
      pw::bluetooth::emboss::LESetupISODataPathCommand::MinSizeInBytes() +
      (codec_configuration.has_value() ? codec_configuration->size() : 0);
  auto packet = hci::CommandPacket::New<
      pw::bluetooth::emboss::LESetupISODataPathCommandWriter>(
      hci_spec::kLESetupISODataPath, packet_size);
  auto view = packet.view_t();
  view.connection_handle().Write(connection_handle);
  view.data_path_direction().Write(direction);
  view.data_path_id().Write(data_path_id);
  view.codec_id().CopyFrom(
      const_cast<bt::StaticPacket<pw::bluetooth::emboss::CodecIdWriter>&>(
          codec_id)
          .view());
  view.controller_delay().Write(controller_delay);
  if (codec_configuration.has_value()) {
    view.codec_configuration_length().Write(codec_configuration->size());
    std::memcpy(view.codec_configuration().BackingStorage().data(),
                codec_configuration->data(),
                codec_configuration->size());
  } else {
    view.codec_configuration_length().Write(0);
  }
  return DynamicByteBuffer(packet.data());
}

DynamicByteBuffer LESetupIsoDataPathResponse(
    pw::bluetooth::emboss::StatusCode status,
    hci_spec::ConnectionHandle connection_handle) {
  return DynamicByteBuffer(StaticByteBuffer(
      hci_spec::kCommandCompleteEventCode,
      0x06,  // parameter_total_size (6 bytes)
      0xF0,  // Num_HCI_CommandPackets allowed to be sent to controller (240)
      LowerBits(hci_spec::kLESetupISODataPath),
      UpperBits(hci_spec::kLESetupISODataPath),
      status,
      LowerBits(connection_handle),
      UpperBits(connection_handle)));
}

DynamicByteBuffer LERequestPeerScaPacket(hci_spec::ConnectionHandle conn) {
  auto packet = hci::CommandPacket::New<
      pw::bluetooth::emboss::LERequestPeerSCACommandWriter>(
      hci_spec::kLERequestPeerSCA);
  auto view = packet.view_t();
  view.connection_handle().Write(conn);
  return DynamicByteBuffer(packet.data());
}

DynamicByteBuffer LEPeriodicAdvertisingCreateSyncPacket(
    DeviceAddress address,
    uint8_t sid,
    uint16_t sync_timeout,
    bool filter_duplicates,
    bool use_periodic_advertiser_list) {
  auto packet = hci::CommandPacket::New<
      pw::bluetooth::emboss::LEPeriodicAdvertisingCreateSyncCommandWriter>(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC);
  auto view = packet.view_t();
  view.options().use_periodic_advertiser_list().Write(
      use_periodic_advertiser_list);
  view.options().enable_duplicate_filtering().Write(filter_duplicates);
  view.advertising_sid().Write(sid);

  pw::bluetooth::emboss::LEPeerAddressTypeNoAnon address_type =
      DeviceAddress::DeviceAddrToLePeerAddrNoAnon(address.type());

  view.advertiser_address_type().Write(address_type);
  view.advertiser_address().CopyFrom(address.value().view());
  view.skip().Write(0);
  view.sync_timeout().Write(sync_timeout);
  view.sync_cte_type().BackingStorage().WriteUInt(0);
  return DynamicByteBuffer(packet.data());
}

DynamicByteBuffer LEPeriodicAdvertisingCreateSyncCancelPacket() {
  auto packet = hci::CommandPacket::New<
      pw::bluetooth::emboss::
          LEPeriodicAdvertisingCreateSyncCancelCommandWriter>(
      pw::bluetooth::emboss::OpCode::
          LE_PERIODIC_ADVERTISING_CREATE_SYNC_CANCEL);
  return DynamicByteBuffer(packet.data());
}

DynamicByteBuffer LEAddDeviceToPeriodicAdvertiserListPacket(
    DeviceAddress address, uint8_t sid) {
  auto packet = hci::CommandPacket::New<
      pw::bluetooth::emboss::LEAddDeviceToPeriodicAdvertiserListCommandWriter>(
      hci_spec::kLEAddDeviceToPeriodicAdvertiserList);
  auto view = packet.view_t();
  pw::bluetooth::emboss::LEPeerAddressTypeNoAnon address_type =
      DeviceAddress::DeviceAddrToLePeerAddrNoAnon(address.type());
  view.advertiser_address_type().Write(address_type);
  view.advertiser_address().CopyFrom(address.value().view());
  view.advertising_sid().Write(sid);
  return DynamicByteBuffer(packet.data());
}

DynamicByteBuffer LERemoveDeviceFromPeriodicAdvertiserListPacket(
    DeviceAddress address, uint8_t sid) {
  auto packet = hci::CommandPacket::New<
      pw::bluetooth::emboss::
          LERemoveDeviceFromPeriodicAdvertiserListCommandWriter>(
      hci_spec::kLERemoveDeviceFromPeriodicAdvertiserList);
  auto view = packet.view_t();
  pw::bluetooth::emboss::LEPeerAddressTypeNoAnon address_type =
      DeviceAddress::DeviceAddrToLePeerAddrNoAnon(address.type());
  view.advertiser_address_type().Write(address_type);
  view.advertiser_address().CopyFrom(address.value().view());
  view.advertising_sid().Write(sid);
  return DynamicByteBuffer(packet.data());
}

DynamicByteBuffer LEPeriodicAdvertisingSyncEstablishedEventPacketV1(
    pw::bluetooth::emboss::StatusCode status,
    hci_spec::SyncHandle sync_handle,
    uint8_t advertising_sid,
    DeviceAddress address,
    pw::bluetooth::emboss::LEPhy phy,
    uint16_t interval,
    pw::bluetooth::emboss::LEClockAccuracy clock_accuracy) {
  auto packet = hci::EventPacket::New<
      pw::bluetooth::emboss::
          LEPeriodicAdvertisingSyncEstablishedSubeventV1Writer>(
      hci_spec::kLEMetaEventCode);
  auto view = packet.view_t();
  view.le_meta_event().subevent_code_enum().Write(
      pw::bluetooth::emboss::LeSubEventCode::
          PERIODIC_ADVERTISING_SYNC_ESTABLISHED);
  view.status().Write(status);
  view.sync_handle().Write(sync_handle);
  view.advertising_sid().Write(advertising_sid);
  pw::bluetooth::emboss::LEAddressType address_type =
      DeviceAddress::DeviceAddrToLeAddr(address.type());
  view.advertiser_address_type().Write(address_type);
  view.advertiser_address().CopyFrom(address.value().view());
  view.advertiser_phy().Write(phy);
  view.periodic_advertising_interval().Write(interval);
  view.advertiser_clock_accuracy().Write(clock_accuracy);
  return DynamicByteBuffer(packet.data());
}

DynamicByteBuffer LEPeriodicAdvertisingSyncEstablishedEventPacketV2(
    pw::bluetooth::emboss::StatusCode status,
    hci_spec::SyncHandle sync_handle,
    uint8_t advertising_sid,
    DeviceAddress address,
    pw::bluetooth::emboss::LEPhy phy,
    uint16_t interval,
    pw::bluetooth::emboss::LEClockAccuracy clock_accuracy,
    uint8_t num_subevents) {
  auto packet = hci::EventPacket::New<
      pw::bluetooth::emboss::
          LEPeriodicAdvertisingSyncEstablishedSubeventV2Writer>(
      hci_spec::kLEMetaEventCode);
  auto view = packet.view_t();
  view.le_meta_event().subevent_code_enum().Write(
      pw::bluetooth::emboss::LeSubEventCode::
          PERIODIC_ADVERTISING_SYNC_ESTABLISHED_V2);
  view.status().Write(status);
  view.sync_handle().Write(sync_handle);
  view.advertising_sid().Write(advertising_sid);
  pw::bluetooth::emboss::LEAddressType address_type =
      DeviceAddress::DeviceAddrToLeAddr(address.type());
  view.advertiser_address_type().Write(address_type);
  view.advertiser_address().CopyFrom(address.value().view());
  view.advertiser_phy().Write(phy);
  view.periodic_advertising_interval().Write(interval);
  view.advertiser_clock_accuracy().Write(clock_accuracy);
  view.num_subevents().Write(num_subevents);
  return DynamicByteBuffer(packet.data());
}

DynamicByteBuffer LEPeriodicAdvertisingReportEventPacketV1(
    hci_spec::SyncHandle sync_handle,
    pw::bluetooth::emboss::LEPeriodicAdvertisingDataStatus data_status,
    const DynamicByteBuffer& data) {
  const size_t subevent_size =
      pw::bluetooth::emboss::LEPeriodicAdvertisingReportSubeventV1View::
          MinSizeInBytes()
              .Read() +
      data.size();
  auto packet = hci::EventPacket::New<
      pw::bluetooth::emboss::LEPeriodicAdvertisingReportSubeventV1Writer>(
      hci_spec::kLEMetaEventCode, subevent_size);
  auto view = packet.view_t();
  view.le_meta_event().subevent_code_enum().Write(
      pw::bluetooth::emboss::LeSubEventCode::PERIODIC_ADVERTISING_REPORT);
  view.sync_handle().Write(sync_handle);
  view.tx_power().Write(0);
  view.rssi().Write(0);
  view.cte_type().Write(pw::bluetooth::emboss::LECteType::AOA_CTE);
  view.data_status().Write(data_status);
  view.data_length().Write(data.size());
  std::memcpy(view.data().BackingStorage().data(), data.data(), data.size());
  return DynamicByteBuffer(packet.data());
}

DynamicByteBuffer LEPeriodicAdvertisingReportEventPacketV2(
    hci_spec::SyncHandle sync_handle,
    uint16_t event_counter,
    uint8_t subevent,
    pw::bluetooth::emboss::LEPeriodicAdvertisingDataStatus data_status,
    const DynamicByteBuffer& data) {
  const size_t subevent_size =
      pw::bluetooth::emboss::LEPeriodicAdvertisingReportSubeventV2View::
          MinSizeInBytes()
              .Read() +
      data.size();
  auto packet = hci::EventPacket::New<
      pw::bluetooth::emboss::LEPeriodicAdvertisingReportSubeventV2Writer>(
      hci_spec::kLEMetaEventCode, subevent_size);
  auto view = packet.view_t();
  view.le_meta_event().subevent_code_enum().Write(
      pw::bluetooth::emboss::LeSubEventCode::PERIODIC_ADVERTISING_REPORT_V2);
  view.sync_handle().Write(sync_handle);
  view.tx_power().Write(0);
  view.rssi().Write(0);
  view.cte_type().Write(pw::bluetooth::emboss::LECteType::AOA_CTE);
  view.periodic_event_counter().Write(event_counter);
  view.subevent().Write(subevent);
  view.data_status().Write(data_status);
  view.data_length().Write(data.size());
  std::memcpy(view.data().BackingStorage().data(), data.data(), data.size());
  return DynamicByteBuffer(packet.data());
}

DynamicByteBuffer LESyncLostEventPacket(hci_spec::SyncHandle sync_handle) {
  auto packet = hci::EventPacket::New<
      pw::bluetooth::emboss::LEPeriodicAdvertisingSyncLostSubeventWriter>(
      hci_spec::kLEMetaEventCode);
  auto view = packet.view_t();
  view.le_meta_event().subevent_code().Write(
      hci_spec::kLEPeriodicAdvertisingSyncLostSubeventCode);
  view.sync_handle().Write(sync_handle);
  return DynamicByteBuffer(packet.data());
}

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
    bool encryption) {
  auto packet = hci::EventPacket::New<
      pw::bluetooth::emboss::LEBigInfoAdvertisingReportSubeventWriter>(
      hci_spec::kLEMetaEventCode,
      pw::bluetooth::emboss::LEBigInfoAdvertisingReportSubeventView::
          SizeInBytes());
  auto view = packet.view_t();
  view.le_meta_event().subevent_code_enum().Write(
      pw::bluetooth::emboss::LeSubEventCode::BIG_INFO_ADVERTISING_REPORT);
  view.sync_handle().Write(sync_handle);
  view.num_bis().Write(num_bis);
  view.nse().Write(nse);
  view.iso_interval().Write(iso_interval);
  view.bn().Write(bn);
  view.pto().Write(pto);
  view.irc().Write(irc);
  view.max_pdu().Write(max_pdu);
  view.sdu_interval().Write(sdu_interval);
  view.max_sdu().Write(max_sdu);
  view.phy().Write(phy);
  view.framing().Write(framing);
  view.encryption().Write(encryption ? 0x01 : 0x00);
  return DynamicByteBuffer(packet.data());
}

DynamicByteBuffer LEPeriodicAdvertisingSyncTransferReceivedEventPacket(
    pw::bluetooth::emboss::StatusCode status,
    hci_spec::ConnectionHandle connection_handle,
    uint16_t service_data,
    hci_spec::SyncHandle sync_handle,
    uint8_t advertising_sid,
    DeviceAddress address,
    pw::bluetooth::emboss::LEPhy phy,
    uint16_t pa_interval,
    pw::bluetooth::emboss::LEClockAccuracy advertiser_clock_accuracy) {
  auto packet = hci::EventPacket::New<
      pw::bluetooth::emboss::
          LEPeriodicAdvertisingSyncTransferReceivedSubeventV1Writer>(
      hci_spec::kLEMetaEventCode);
  auto view = packet.view_t();
  view.le_meta_event().subevent_code_enum().Write(
      pw::bluetooth::emboss::LeSubEventCode::
          PERIODIC_ADVERTISING_SYNC_TRANSFER_RECEIVED);
  view.status().Write(status);
  view.connection_handle().Write(connection_handle);
  view.service_data().Write(service_data);
  view.sync_handle().Write(sync_handle);
  view.advertising_sid().Write(advertising_sid);
  pw::bluetooth::emboss::LEAddressType address_type =
      DeviceAddress::DeviceAddrToLeAddr(address.type());
  view.advertiser_address_type().Write(address_type);
  view.advertiser_address().CopyFrom(address.value().view());
  view.advertiser_phy().Write(phy);
  view.periodic_advertising_interval().Write(pa_interval);
  view.advertiser_clock_accuracy().Write(advertiser_clock_accuracy);
  return DynamicByteBuffer(packet.data());
}

DynamicByteBuffer LEPeriodicAdvertisingTerminateSyncPacket(
    hci_spec::SyncHandle sync_handle) {
  auto packet = hci::CommandPacket::New<
      pw::bluetooth::emboss::LEPeriodicAdvertisingTerminateSyncCommandWriter>(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_TERMINATE_SYNC);
  packet.view_t().sync_handle().Write(sync_handle);
  return DynamicByteBuffer(packet.data());
}

DynamicByteBuffer LESetPeriodicAdvertisingSyncTransferParamsPacket(
    hci_spec::ConnectionHandle connection_handle,
    pw::bluetooth::emboss::PeriodicAdvertisingSyncTransferMode mode,
    uint16_t sync_timeout) {
  auto packet = hci::CommandPacket::New<
      pw::bluetooth::emboss::
          LESetPeriodicAdvertisingSyncTransferParametersCommandWriter>(
      pw::bluetooth::emboss::OpCode::
          LE_SET_PERIODIC_ADVERTISING_SYNC_TRANSFER_PARAMETERS);
  auto view = packet.view_t();
  view.connection_handle().Write(connection_handle);
  view.mode().Write(mode);
  view.skip().Write(0);
  view.sync_timeout().Write(sync_timeout);
  view.sync_cte_type().BackingStorage().WriteUInt(0);
  return DynamicByteBuffer(packet.data());
}

DynamicByteBuffer LEPeriodicAdvertisingSyncTransferPacket(
    hci_spec::ConnectionHandle connection_handle,
    uint16_t service_data,
    hci_spec::SyncHandle sync_handle) {
  auto packet = hci::CommandPacket::New<
      pw::bluetooth::emboss::LEPeriodicAdvertisingSyncTransferCommandWriter>(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_SYNC_TRANSFER);
  auto view = packet.view_t();
  view.connection_handle().Write(connection_handle);
  view.service_data().Write(service_data);
  view.sync_handle().Write(sync_handle);
  return DynamicByteBuffer(packet.data());
}

DynamicByteBuffer LEStartEncryptionPacket(hci_spec::ConnectionHandle conn,
                                          uint64_t random_number,
                                          uint16_t encrypted_diversifier,
                                          UInt128 ltk) {
  const BufferView rand(&random_number, sizeof(random_number));
  return DynamicByteBuffer(StaticByteBuffer(
      LowerBits(hci_spec::kLEStartEncryption),
      UpperBits(hci_spec::kLEStartEncryption),
      0x1c,             // parameter_total_size (28 bytes)
      LowerBits(conn),  // Connection_Handle
      UpperBits(conn),  // Connection_Handle
      // Random_Number (8 bytes)
      rand[0],
      rand[1],
      rand[2],
      rand[3],
      rand[4],
      rand[5],
      rand[6],
      rand[7],
      LowerBits(encrypted_diversifier),  // Encrypted_Diversifier
      UpperBits(encrypted_diversifier),  // Encrypted_Diversifier
      // Long_Term_Key (16 bytes)
      ltk[0],
      ltk[1],
      ltk[2],
      ltk[3],
      ltk[4],
      ltk[5],
      ltk[6],
      ltk[7],
      ltk[8],
      ltk[9],
      ltk[10],
      ltk[11],
      ltk[12],
      ltk[13],
      ltk[14],
      ltk[15]));
}

DynamicByteBuffer LinkKeyNotificationPacket(DeviceAddress address,
                                            UInt128 link_key,
                                            hci_spec::LinkKeyType key_type) {
  const auto addr = address.value().bytes();
  return DynamicByteBuffer(StaticByteBuffer(
      hci_spec::kLinkKeyNotificationEventCode,
      0x17,  // parameter_total_size (23 bytes)
      // peer BD_ADDR (6 bytes)
      addr[0],
      addr[1],
      addr[2],
      addr[3],
      addr[4],
      addr[5],
      // Link_Key (16 bytes)
      link_key[0],
      link_key[1],
      link_key[2],
      link_key[3],
      link_key[4],
      link_key[5],
      link_key[6],
      link_key[7],
      link_key[8],
      link_key[9],
      link_key[10],
      link_key[11],
      link_key[12],
      link_key[13],
      link_key[14],
      link_key[15],
      key_type  // Key_Type
      ));
}

DynamicByteBuffer LinkKeyRequestPacket(DeviceAddress address) {
  const auto addr = address.value().bytes();
  return DynamicByteBuffer(
      StaticByteBuffer(hci_spec::kLinkKeyRequestEventCode,
                       0x06,  // parameter_total_size (6 bytes)
                       // peer BD_ADDR (6 bytes)
                       addr[0],
                       addr[1],
                       addr[2],
                       addr[3],
                       addr[4],
                       addr[5]));
}

DynamicByteBuffer LinkKeyRequestNegativeReplyPacket(DeviceAddress address) {
  const auto addr = address.value().bytes();
  return DynamicByteBuffer(
      StaticByteBuffer(LowerBits(hci_spec::kLinkKeyRequestNegativeReply),
                       UpperBits(hci_spec::kLinkKeyRequestNegativeReply),
                       0x06,  // parameter_total_size (6 bytes)
                       // peer BD_ADDR (6 bytes)
                       addr[0],
                       addr[1],
                       addr[2],
                       addr[3],
                       addr[4],
                       addr[5]));
}

DynamicByteBuffer LinkKeyRequestNegativeReplyResponse(DeviceAddress address) {
  const auto addr = address.value().bytes();
  return DynamicByteBuffer(StaticByteBuffer(
      hci_spec::kCommandCompleteEventCode,
      0x0A,  // parameter_total_size (10 bytes)
      0xF0,  // Num_HCI_Command_Packets allowed to be sent to controller (240)
      LowerBits(hci_spec::kLinkKeyRequestNegativeReply),  // Command_Opcode
      UpperBits(hci_spec::kLinkKeyRequestNegativeReply),  // Command_Opcode
      pw::bluetooth::emboss::StatusCode::SUCCESS,         // Status
      // peer BD_ADDR (6 bytes)
      addr[0],
      addr[1],
      addr[2],
      addr[3],
      addr[4],
      addr[5]));
}

DynamicByteBuffer LinkKeyRequestReplyPacket(DeviceAddress address,
                                            UInt128 link_key) {
  const auto addr = address.value().bytes();
  return DynamicByteBuffer(
      StaticByteBuffer(LowerBits(hci_spec::kLinkKeyRequestReply),
                       UpperBits(hci_spec::kLinkKeyRequestReply),
                       0x16,  // parameter_total_size (22 bytes)
                       // peer BD_ADDR (6 bytes)
                       addr[0],
                       addr[1],
                       addr[2],
                       addr[3],
                       addr[4],
                       addr[5],
                       // Link_Key (16 bytes)
                       link_key[0],
                       link_key[1],
                       link_key[2],
                       link_key[3],
                       link_key[4],
                       link_key[5],
                       link_key[6],
                       link_key[7],
                       link_key[8],
                       link_key[9],
                       link_key[10],
                       link_key[11],
                       link_key[12],
                       link_key[13],
                       link_key[14],
                       link_key[15]));
}

DynamicByteBuffer LinkKeyRequestReplyResponse(DeviceAddress address) {
  const auto addr = address.value().bytes();
  return DynamicByteBuffer(StaticByteBuffer(
      hci_spec::kCommandCompleteEventCode,
      0x0A,  // parameter_total_size (10 bytes)
      0xF0,  // Num_HCI_Command_Packets allowed to be sent to controller (240)
      LowerBits(hci_spec::kLinkKeyRequestReply),   // Command_Opcode
      UpperBits(hci_spec::kLinkKeyRequestReply),   // Command_Opcode
      pw::bluetooth::emboss::StatusCode::SUCCESS,  // Status
      // peer BD_ADDR (6 bytes)
      addr[0],
      addr[1],
      addr[2],
      addr[3],
      addr[4],
      addr[5]));
}

DynamicByteBuffer NumberOfCompletedPacketsPacket(
    hci_spec::ConnectionHandle conn, uint16_t num_packets) {
  return DynamicByteBuffer(StaticByteBuffer(
      0x13,
      0x05,  // Number Of Completed Packet HCI event header, parameters length
      0x01,  // Num_Handles
      LowerBits(conn),         // Connection_Handle
      UpperBits(conn),         // Connection_Handle
      LowerBits(num_packets),  // Num_Completed_Packets
      UpperBits(num_packets)   // Num_Completed_Packets
      ));
}

DynamicByteBuffer NumberOfCompletedPacketsPacketWithInvalidSize(
    hci_spec::ConnectionHandle conn, uint16_t num_packets) {
  return DynamicByteBuffer(StaticByteBuffer(
      0x13,
      0x05,  // Number Of Completed Packet HCI event header, parameters length
      0x03,  // Num_Handles
      LowerBits(conn),         // Connection_Handle
      UpperBits(conn),         // Connection_Handle
      LowerBits(num_packets),  // Num_Completed_Packets
      UpperBits(num_packets)   // Num_Completed_Packets
      ));
}

DynamicByteBuffer PinCodeRequestPacket(DeviceAddress address) {
  const auto addr = address.value().bytes();
  return DynamicByteBuffer(
      StaticByteBuffer(hci_spec::kPinCodeRequestEventCode,
                       0x06,  // parameter_total_size (6 bytes)
                       // peer BD_ADDR (6 bytes)
                       addr[0],
                       addr[1],
                       addr[2],
                       addr[3],
                       addr[4],
                       addr[5]));
}

DynamicByteBuffer PinCodeRequestNegativeReplyPacket(DeviceAddress address) {
  const auto addr = address.value().bytes();
  return DynamicByteBuffer(
      StaticByteBuffer(LowerBits(hci_spec::kPinCodeRequestNegativeReply),
                       UpperBits(hci_spec::kPinCodeRequestNegativeReply),
                       0x06,  // parameter_total_size (6 bytes)
                       // peer BD_ADDR (6 bytes)
                       addr[0],
                       addr[1],
                       addr[2],
                       addr[3],
                       addr[4],
                       addr[5]));
}

DynamicByteBuffer PinCodeRequestNegativeReplyResponse(DeviceAddress address) {
  const auto addr = address.value().bytes();
  return DynamicByteBuffer(StaticByteBuffer(
      hci_spec::kCommandCompleteEventCode,
      0x0A,  // parameter_total_size (10 bytes)
      0xF0,  // Num_HCI_Command_Packets allowed to be sent to controller (240)
      LowerBits(hci_spec::kPinCodeRequestNegativeReply),  // Command_Opcode
      UpperBits(hci_spec::kPinCodeRequestNegativeReply),  // Command_Opcode
      pw::bluetooth::emboss::StatusCode::SUCCESS,         // Status
      // peer BD_ADDR (6 bytes)
      addr[0],
      addr[1],
      addr[2],
      addr[3],
      addr[4],
      addr[5]));
}

DynamicByteBuffer PinCodeRequestReplyPacket(DeviceAddress address,
                                            uint8_t pin_length,
                                            std::string pin_code) {
  const auto addr = address.value().bytes();

  StaticByteBuffer<16> pin;
  pin.Write(BufferView(pin_code));

  return DynamicByteBuffer(
      StaticByteBuffer(LowerBits(hci_spec::kPinCodeRequestReply),
                       UpperBits(hci_spec::kPinCodeRequestReply),
                       0x17,  // parameter_total_size (23 bytes)
                       // peer BD_ADDR (6 bytes)
                       addr[0],
                       addr[1],
                       addr[2],
                       addr[3],
                       addr[4],
                       addr[5],
                       pin_length,  // PIN_Code_Length
                       // PIN_Code
                       pin[0],
                       pin[1],
                       pin[2],
                       pin[3],
                       pin[4],
                       pin[5],
                       pin[6],
                       pin[7],
                       pin[8],
                       pin[9],
                       pin[10],
                       pin[11],
                       pin[12],
                       pin[13],
                       pin[14],
                       pin[15]));
}

DynamicByteBuffer PinCodeRequestReplyResponse(DeviceAddress address) {
  const auto addr = address.value().bytes();
  return DynamicByteBuffer(StaticByteBuffer(
      hci_spec::kCommandCompleteEventCode,
      0x0A,  // parameter_total_size (10 bytes)
      0xF0,  // Num_HCI_Command_Packets allowed to be sent to controller (240)
      LowerBits(hci_spec::kPinCodeRequestReply),   // Command_Opcode
      UpperBits(hci_spec::kPinCodeRequestReply),   // Command_Opcode
      pw::bluetooth::emboss::StatusCode::SUCCESS,  // Status
      // peer BD_ADDR (6 bytes)
      addr[0],
      addr[1],
      addr[2],
      addr[3],
      addr[4],
      addr[5]));
}

DynamicByteBuffer ReadRemoteExtended1CompletePacket(
    hci_spec::ConnectionHandle conn) {
  return DynamicByteBuffer(StaticByteBuffer(
      hci_spec::kReadRemoteExtendedFeaturesCompleteEventCode,
      0x0D,  // parameter_total_size (13 bytes)
      pw::bluetooth::emboss::StatusCode::SUCCESS,  // Status
      LowerBits(conn),                             // Connection_Handle
      UpperBits(conn),                             // Connection_Handle
      0x01,                                        // Page_Number
      0x03,                                        // Max_Page_Number (3 pages)
      // Extended_LMP_Features (8 bytes)
      0x0F,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00
      // lmp_features_page1: Secure Simple Pairing (Host Support), LE Supported
      // (Host), Previously used, Secure Connections (Host Support)
      ));
}

DynamicByteBuffer ReadRemoteExtended1CompletePacketNoSsp(
    hci_spec::ConnectionHandle conn) {
  return DynamicByteBuffer(StaticByteBuffer(
      hci_spec::kReadRemoteExtendedFeaturesCompleteEventCode,
      0x0D,  // parameter_total_size (13 bytes)
      pw::bluetooth::emboss::StatusCode::SUCCESS,  // Status
      LowerBits(conn),                             // Connection_Handle
      UpperBits(conn),                             // Connection_Handle
      0x01,                                        // Page_Number
      0x03,                                        // Max_Page_Number (3 pages)
      // Extended_LMP_Features (8 bytes)
      0x0E,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00
      // lmp_features_page1: LE Supported (Host), Previously used, Secure
      // Connections (Host Support)
      ));
}

DynamicByteBuffer ReadRemoteExtended1Packet(hci_spec::ConnectionHandle conn) {
  return DynamicByteBuffer(StaticByteBuffer(
      LowerBits(hci_spec::kReadRemoteExtendedFeatures),
      UpperBits(hci_spec::kReadRemoteExtendedFeatures),
      0x03,             // parameter_total_size (3 bytes)
      LowerBits(conn),  // Connection_Handle
      UpperBits(conn),  // Connection_Handle
      0x01              // Page_Number (1)
      ));
}

DynamicByteBuffer ReadRemoteExtended2CompletePacket(
    hci_spec::ConnectionHandle conn) {
  return DynamicByteBuffer(StaticByteBuffer(
      hci_spec::kReadRemoteExtendedFeaturesCompleteEventCode,
      0x0D,  // parameter_total_size (13 bytes)
      pw::bluetooth::emboss::StatusCode::SUCCESS,  // Status
      LowerBits(conn),                             // Connection_Handle
      UpperBits(conn),                             // Connection_Handle
      0x02,                                        // Page_Number
      0x03,                                        // Max_Page_Number (3 pages)
      // Extended_LMP_Features (8 bytes)
      0x00,
      0x00,
      0x00,
      0x00,
      0x02,
      0x00,
      0xFF,
      0x00
      // lmp_features_page2: All the bits should be ignored
      ));
}

DynamicByteBuffer ReadRemoteExtended2Packet(hci_spec::ConnectionHandle conn) {
  return DynamicByteBuffer(StaticByteBuffer(
      LowerBits(hci_spec::kReadRemoteExtendedFeatures),
      UpperBits(hci_spec::kReadRemoteExtendedFeatures),
      0x03,             // parameter_total_size (3 bytes)
      LowerBits(conn),  // Connection_Handle
      UpperBits(conn),  // Connection_Handle
      0x02              // Page_Number (2)
      ));
}

DynamicByteBuffer ReadRemoteVersionInfoCompletePacket(
    hci_spec::ConnectionHandle conn) {
  return DynamicByteBuffer(StaticByteBuffer(
      hci_spec::kReadRemoteVersionInfoCompleteEventCode,
      0x08,  // parameter_total_size (8 bytes)
      pw::bluetooth::emboss::StatusCode::SUCCESS,  // Status
      LowerBits(conn),                             // Connection_Handle
      UpperBits(conn),                             // Connection_Handle
      pw::bluetooth::emboss::CoreSpecificationVersion::V4_2,  // Version
      0xE0,  // Company_Identifier (Google)
      0x00,  // Company_Identifier (Google)
      0xAD,  // Subversion (Anything)
      0xDE   // Subversion (Anything)
      ));
}

DynamicByteBuffer ReadRemoteVersionInfoPacket(hci_spec::ConnectionHandle conn) {
  return DynamicByteBuffer(StaticByteBuffer(
      LowerBits(hci_spec::kReadRemoteVersionInfo),
      UpperBits(hci_spec::kReadRemoteVersionInfo),
      0x02,             // parameter_total_size (2 bytes)
      LowerBits(conn),  // Connection_Handle
      UpperBits(conn)   // Connection_Handle
      ));
}

DynamicByteBuffer ReadRemoteSupportedFeaturesCompletePacket(
    hci_spec::ConnectionHandle conn, bool extended_features) {
  return DynamicByteBuffer(StaticByteBuffer(
      hci_spec::kReadRemoteSupportedFeaturesCompleteEventCode,
      0x0B,  // parameter_total_size (11 bytes)
      pw::bluetooth::emboss::StatusCode::SUCCESS,  // Status
      LowerBits(conn),                             // Connection_Handle
      UpperBits(conn),                             // Connection_Handle
      // LMP_Features (8 bytes)
      0xFF,
      0x00,
      0x00,
      0x00,
      0x04,
      0x00,
      0x08,
      (extended_features ? 0x80 : 0x00)
      // lmp_features_page0: 3 slot packets, 5 slot packets, Encryption, Slot
      // Offset, Timing Accuracy, Role Switch, Hold Mode, Sniff Mode, LE
      // Supported (Controller), Secure Simple Pairing (Controller Support),
      // Extended Features if enabled
      ));
}

DynamicByteBuffer ReadRemoteSupportedFeaturesPacket(
    hci_spec::ConnectionHandle conn) {
  return DynamicByteBuffer(StaticByteBuffer(
      LowerBits(hci_spec::kReadRemoteSupportedFeatures),
      UpperBits(hci_spec::kReadRemoteSupportedFeatures),
      0x02,             // parameter_total_size (2 bytes)
      LowerBits(conn),  // Connection_Handle
      UpperBits(conn)   // Connection_Handle
      ));
}

DynamicByteBuffer ReadScanEnable() {
  return DynamicByteBuffer(StaticByteBuffer(
      LowerBits(hci_spec::kReadScanEnable),
      UpperBits(hci_spec::kReadScanEnable),
      0x00  // No parameters
      ));
}

DynamicByteBuffer ReadScanEnableResponse(uint8_t scan_enable) {
  return DynamicByteBuffer(StaticByteBuffer(
      hci_spec::kCommandCompleteEventCode,
      0x05,  // parameter_total_size (5 bytes)
      0xF0,  // Num_HCI_Command_Packets allowed to be sent to controller (240)
      LowerBits(hci_spec::kReadScanEnable),        // Command_Opcode
      UpperBits(hci_spec::kReadScanEnable),        // Command_Opcode
      pw::bluetooth::emboss::StatusCode::SUCCESS,  // Status
      scan_enable                                  // Scan_Enable
      ));
}

DynamicByteBuffer RejectConnectionRequestPacket(
    DeviceAddress address, pw::bluetooth::emboss::StatusCode reason) {
  const auto addr = address.value().bytes();
  return DynamicByteBuffer(StaticByteBuffer(
      LowerBits(hci_spec::kRejectConnectionRequest),
      UpperBits(hci_spec::kRejectConnectionRequest),
      0x07,  // parameter_total_size (7 bytes)
      // peer BD_ADDR (6 bytes)
      addr[0],
      addr[1],
      addr[2],
      addr[3],
      addr[4],
      addr[5],
      reason  // Reason
      ));
}
DynamicByteBuffer RejectSynchronousConnectionRequest(
    DeviceAddress address, pw::bluetooth::emboss::StatusCode status_code) {
  const auto addr = address.value().bytes();
  return DynamicByteBuffer(StaticByteBuffer(
      LowerBits(hci_spec::kRejectSynchronousConnectionRequest),
      UpperBits(hci_spec::kRejectSynchronousConnectionRequest),
      0x07,  // parameter_total_size (7 bytes)
      // peer BD_ADDR (6 bytes)
      addr[0],
      addr[1],
      addr[2],
      addr[3],
      addr[4],
      addr[5],
      status_code  // Reason
      ));
}

DynamicByteBuffer RemoteNameRequestCompletePacket(DeviceAddress address,
                                                  const std::string& name) {
  const auto addr = address.value().bytes();
  auto event = DynamicByteBuffer(
      pw::bluetooth::emboss::RemoteNameRequestCompleteEventView::
          IntrinsicSizeInBytes()
              .Read());
  event.SetToZeros();
  const StaticByteBuffer header(
      hci_spec::kRemoteNameRequestCompleteEventCode,
      0xff,  // parameter_total_size (255 bytes)
      pw::bluetooth::emboss::StatusCode::SUCCESS,  // Status
      // peer BD_ADDR (6 bytes)
      addr[0],
      addr[1],
      addr[2],
      addr[3],
      addr[4],
      addr[5]);
  header.Copy(&event);
  event.Write(reinterpret_cast<const uint8_t*>(name.data()),
              name.size(),
              header.size());
  return event;
}

DynamicByteBuffer RemoteNameRequestPacket(DeviceAddress address) {
  const auto addr = address.value().bytes();
  return DynamicByteBuffer(StaticByteBuffer(
      LowerBits(hci_spec::kRemoteNameRequest),
      UpperBits(hci_spec::kRemoteNameRequest),
      0x0a,  // parameter_total_size (10 bytes)
      // peer BD_ADDR (6 bytes)
      addr[0],
      addr[1],
      addr[2],
      addr[3],
      addr[4],
      addr[5],
      0x00,  // Page_Scan_Repetition_Mode (R0)
      0x00,  // Reserved
      0x00,  // Clock_Offset
      0x00   // Clock_Offset
      ));
}

DynamicByteBuffer RoleChangePacket(DeviceAddress address,
                                   pw::bluetooth::emboss::ConnectionRole role,
                                   pw::bluetooth::emboss::StatusCode status) {
  const auto addr = address.value().bytes();
  return DynamicByteBuffer(StaticByteBuffer(
      hci_spec::kRoleChangeEventCode,
      0x08,    // parameter_total_size (8 bytes)
      status,  // Status
      // peer BD_ADDR (6 bytes)
      addr[0],
      addr[1],
      addr[2],
      addr[3],
      addr[4],
      addr[5],
      role  // Role
      ));
}

DynamicByteBuffer ScoDataPacket(
    hci_spec::ConnectionHandle conn,
    hci_spec::SynchronousDataPacketStatusFlag flag,
    const BufferView& payload,
    std::optional<uint8_t> payload_length_override) {
  // Flag is bits 4-5 in the higher octet of |handle_and_flags|, i.e.
  // 0b00xx000000000000.
  conn |= static_cast<uint8_t>(flag) << 12;
  StaticByteBuffer header(
      LowerBits(conn),
      UpperBits(conn),
      payload_length_override ? *payload_length_override : payload.size());
  DynamicByteBuffer out(header.size() + payload.size());
  header.Copy(&out);
  MutableBufferView payload_view = out.mutable_view(header.size());
  payload.Copy(&payload_view);
  return out;
}

DynamicByteBuffer SetConnectionEncryption(hci_spec::ConnectionHandle conn,
                                          bool enable) {
  return DynamicByteBuffer(StaticByteBuffer(
      LowerBits(hci_spec::kSetConnectionEncryption),
      UpperBits(hci_spec::kSetConnectionEncryption),
      0x03,                         // parameter_total_size (3 bytes)
      LowerBits(conn),              // Connection_Handle
      UpperBits(conn),              // Connection_Handle
      static_cast<uint8_t>(enable)  // Encryption_Enable
      ));
}

DynamicByteBuffer SimplePairingCompletePacket(
    DeviceAddress address, pw::bluetooth::emboss::StatusCode status_code) {
  const auto addr = address.value().bytes();
  return DynamicByteBuffer(
      StaticByteBuffer(hci_spec::kSimplePairingCompleteEventCode,
                       0x07,  // parameter_total_size (7 bytes)
                       status_code,
                       // peer BD_ADDR (6 bytes)
                       addr[0],
                       addr[1],
                       addr[2],
                       addr[3],
                       addr[4],
                       addr[5]));
}

DynamicByteBuffer StartA2dpOffloadRequest(
    const l2cap::A2dpOffloadManager::Configuration& config,
    hci_spec::ConnectionHandle connection_handle,
    l2cap::ChannelId l2cap_channel_id,
    uint16_t l2cap_mtu_size) {
  constexpr size_t kPacketSize =
      android_emb::StartA2dpOffloadCommand::MaxSizeInBytes();
  auto packet =
      hci::CommandPacket::New<android_emb::StartA2dpOffloadCommandWriter>(
          android_hci::kA2dpOffloadCommand, kPacketSize);
  auto view = packet.view_t();

  view.vendor_command().sub_opcode().Write(
      android_hci::kStartA2dpOffloadCommandSubopcode);
  view.codec_type().Write(config.codec);
  view.max_latency().Write(config.max_latency);
  view.scms_t_enable().CopyFrom(
      const_cast<l2cap::A2dpOffloadManager::Configuration&>(config)
          .scms_t_enable.view());
  view.sampling_frequency().Write(config.sampling_frequency);
  view.bits_per_sample().Write(config.bits_per_sample);
  view.channel_mode().Write(config.channel_mode);
  view.encoded_audio_bitrate().Write(config.encoded_audio_bit_rate);
  view.connection_handle().Write(connection_handle);
  view.l2cap_channel_id().Write(l2cap_channel_id);
  view.l2cap_mtu_size().Write(l2cap_mtu_size);

  switch (config.codec) {
    case android_emb::A2dpCodecType::SBC:
      view.sbc_codec_information().CopyFrom(
          const_cast<l2cap::A2dpOffloadManager::Configuration&>(config)
              .sbc_configuration.view());
      break;
    case android_emb::A2dpCodecType::AAC:
      view.aac_codec_information().CopyFrom(
          const_cast<l2cap::A2dpOffloadManager::Configuration&>(config)
              .aac_configuration.view());
      break;
    case android_emb::A2dpCodecType::LDAC:
      view.ldac_codec_information().CopyFrom(
          const_cast<l2cap::A2dpOffloadManager::Configuration&>(config)
              .ldac_configuration.view());
      break;
    case android_emb::A2dpCodecType::APTX:
    case android_emb::A2dpCodecType::APTX_HD:
    default:
      break;
  }

  return packet.release();
}

DynamicByteBuffer StopA2dpOffloadRequest() {
  return DynamicByteBuffer(
      StaticByteBuffer(LowerBits(android_hci::kA2dpOffloadCommand),
                       UpperBits(android_hci::kA2dpOffloadCommand),
                       0x01,  // parameter_total_size (1 byte)
                       android_hci::kStopA2dpOffloadCommandSubopcode));
}

DynamicByteBuffer SynchronousConnectionCompletePacket(
    hci_spec::ConnectionHandle conn,
    DeviceAddress address,
    hci_spec::LinkType link_type,
    pw::bluetooth::emboss::StatusCode status) {
  const auto addr = address.value().bytes();
  return DynamicByteBuffer(StaticByteBuffer(
      hci_spec::kSynchronousConnectionCompleteEventCode,
      0x11,             // parameter_total_size (17 bytes)
      status,           // Status
      LowerBits(conn),  // Connection_Handle
      UpperBits(conn),  // Connection_Handle
      // peer BD_ADDR (6 bytes)
      addr[0],
      addr[1],
      addr[2],
      addr[3],
      addr[4],
      addr[5],
      link_type,  // Link_Type
      0x00,       // Transmission_Interval interval
      0x00,       // Retransmission_Window
      0x00,       // RX_Packet_Length
      0x00,       // RX_Packet_Length
      0x00,       // TX_Packet_Length
      0x00,       // TX_Packet_Length
      0x00        // Air_Mode
      ));
}

DynamicByteBuffer UserConfirmationRequestPacket(DeviceAddress address,
                                                uint32_t passkey) {
  const auto addr = address.value().bytes();
  const auto passkey_bytes = ToBytes(passkey);
  return DynamicByteBuffer(StaticByteBuffer(
      hci_spec::kUserConfirmationRequestEventCode,
      0x0A,  // parameter_total_size (10 bytes)
      // peer BD_ADDR (6 bytes)
      addr[0],
      addr[1],
      addr[2],
      addr[3],
      addr[4],
      addr[5],
      passkey_bytes[0],  // Numeric_Value
      passkey_bytes[1],  // Numeric_Value
      passkey_bytes[2],  // Numeric_Value
      passkey_bytes[3]   // Numeric_Value
      ));
}

DynamicByteBuffer UserConfirmationRequestNegativeReplyPacket(
    DeviceAddress address) {
  const auto addr = address.value().bytes();
  return DynamicByteBuffer(StaticByteBuffer(
      LowerBits(hci_spec::kUserConfirmationRequestNegativeReply),
      UpperBits(hci_spec::kUserConfirmationRequestNegativeReply),
      0x06,  // parameter_total_size (6 bytes)
      // peer BD_ADDR (6 bytes)
      addr[0],
      addr[1],
      addr[2],
      addr[3],
      addr[4],
      addr[5]));
}

DynamicByteBuffer UserConfirmationRequestReplyPacket(DeviceAddress address) {
  const auto addr = address.value().bytes();
  return DynamicByteBuffer(
      StaticByteBuffer(LowerBits(hci_spec::kUserConfirmationRequestReply),
                       UpperBits(hci_spec::kUserConfirmationRequestReply),
                       0x06,  // parameter_total_size (6 bytes)
                       // peer BD_ADDR (6 bytes)
                       addr[0],
                       addr[1],
                       addr[2],
                       addr[3],
                       addr[4],
                       addr[5]));
}

DynamicByteBuffer UserPasskeyNotificationPacket(DeviceAddress address,
                                                uint32_t passkey) {
  const auto addr = address.value().bytes();
  const auto passkey_bytes = ToBytes(passkey);
  return DynamicByteBuffer(StaticByteBuffer(
      hci_spec::kUserPasskeyNotificationEventCode,
      0x0A,  // parameter_total_size (10 bytes)
      // peer BD_ADDR (6 bytes)
      addr[0],
      addr[1],
      addr[2],
      addr[3],
      addr[4],
      addr[5],
      passkey_bytes[0],  // Numeric_Value
      passkey_bytes[1],  // Numeric_Value
      passkey_bytes[2],  // Numeric_Value
      passkey_bytes[3]   // Numeric_Value
      ));
}

DynamicByteBuffer UserPasskeyRequestNegativeReply(DeviceAddress address) {
  const auto addr = address.value().bytes();
  return DynamicByteBuffer(
      StaticByteBuffer(LowerBits(hci_spec::kUserPasskeyRequestNegativeReply),
                       UpperBits(hci_spec::kUserPasskeyRequestNegativeReply),
                       0x06,  // parameter_total_size (6 bytes)
                       // peer BD_ADDR (6 bytes)
                       addr[0],
                       addr[1],
                       addr[2],
                       addr[3],
                       addr[4],
                       addr[5]));
}

DynamicByteBuffer UserPasskeyRequestNegativeReplyResponse(
    DeviceAddress address) {
  const auto addr = address.value().bytes();
  return DynamicByteBuffer(StaticByteBuffer(
      hci_spec::kCommandCompleteEventCode,
      0x0A,  // parameter_total_size (10 bytes)
      0xF0,  // Num_HCI_Command_Packets allowed to be sent to controller (240)
      LowerBits(hci_spec::kUserPasskeyRequestNegativeReply),  // Command_Opcode
      UpperBits(hci_spec::kUserPasskeyRequestNegativeReply),  // Command_Opcode
      pw::bluetooth::emboss::StatusCode::SUCCESS,             // Status
      // peer BD_ADDR (6 bytes)
      addr[0],
      addr[1],
      addr[2],
      addr[3],
      addr[4],
      addr[5]));
}

DynamicByteBuffer UserPasskeyRequestPacket(DeviceAddress address) {
  const auto addr = address.value().bytes();
  return DynamicByteBuffer(
      StaticByteBuffer(hci_spec::kUserPasskeyRequestEventCode,
                       0x06,  // parameter_total_size (6 bytes)
                       // peer BD_ADDR (6 bytes)
                       addr[0],
                       addr[1],
                       addr[2],
                       addr[3],
                       addr[4],
                       addr[5]));
}

DynamicByteBuffer UserPasskeyRequestReplyPacket(DeviceAddress address,
                                                uint32_t passkey) {
  const auto addr = address.value().bytes();
  const auto passkey_bytes = ToBytes(passkey);
  return DynamicByteBuffer(StaticByteBuffer(
      LowerBits(hci_spec::kUserPasskeyRequestReply),
      UpperBits(hci_spec::kUserPasskeyRequestReply),
      0x0A,  // parameter_total_size (10 bytes)
      // peer BD_ADDR (6 bytes)
      addr[0],
      addr[1],
      addr[2],
      addr[3],
      addr[4],
      addr[5],
      passkey_bytes[0],  // Numeric_Value
      passkey_bytes[1],  // Numeric_Value
      passkey_bytes[2],  // Numeric_Value
      passkey_bytes[3]   // Numeric_Value
      ));
}

DynamicByteBuffer UserPasskeyRequestReplyResponse(DeviceAddress address) {
  const auto addr = address.value().bytes();
  return DynamicByteBuffer(StaticByteBuffer(
      hci_spec::kCommandCompleteEventCode,
      0x0A,  // parameter_total_size (10 bytes)
      0xF0,  // Num_HCI_Command_Packets allowed to be sent to controller (240)
      LowerBits(hci_spec::kUserPasskeyRequestReply),  // Command_Opcode
      UpperBits(hci_spec::kUserPasskeyRequestReply),  // Command_Opcode
      pw::bluetooth::emboss::StatusCode::SUCCESS,     // Status
      // peer BD_ADDR (6 bytes)
      addr[0],
      addr[1],
      addr[2],
      addr[3],
      addr[4],
      addr[5]));
}

DynamicByteBuffer WriteAutomaticFlushTimeoutPacket(
    hci_spec::ConnectionHandle conn, uint16_t flush_timeout) {
  return DynamicByteBuffer(StaticByteBuffer(
      LowerBits(hci_spec::kWriteAutomaticFlushTimeout),
      UpperBits(hci_spec::kWriteAutomaticFlushTimeout),
      0x04,                      // parameter_total_size (4 bytes)
      LowerBits(conn),           // Connection_Handle
      UpperBits(conn),           // Connection_Handle
      LowerBits(flush_timeout),  // Flush_Timeout
      UpperBits(flush_timeout)   // Flush_Timeout
      ));
}

DynamicByteBuffer WriteInquiryScanActivity(uint16_t scan_interval,
                                           uint16_t scan_window) {
  return DynamicByteBuffer(StaticByteBuffer(
      LowerBits(hci_spec::kWriteInquiryScanActivity),
      UpperBits(hci_spec::kWriteInquiryScanActivity),
      0x04,                      // parameter_total_size (4 bytes)
      LowerBits(scan_interval),  // Inquiry_Scan_Interval
      UpperBits(scan_interval),  // Inquiry_Scan_Interval
      LowerBits(scan_window),    // Inquiry_Scan_Window
      UpperBits(scan_window)     // Inquiry_Scan_Window
      ));
}

DynamicByteBuffer WritePageScanActivityPacket(uint16_t scan_interval,
                                              uint16_t scan_window) {
  return DynamicByteBuffer(StaticByteBuffer(
      LowerBits(hci_spec::kWritePageScanActivity),
      UpperBits(hci_spec::kWritePageScanActivity),
      0x04,                      // parameter_total_size (4 bytes)
      LowerBits(scan_interval),  // Page_Scan_Interval
      UpperBits(scan_interval),  // Page_Scan_Interval
      LowerBits(scan_window),    // Page_Scan_Window
      UpperBits(scan_window)     // Page_Scan_Window
      ));
}

DynamicByteBuffer WritePageScanTypePacket(uint8_t scan_type) {
  return DynamicByteBuffer(StaticByteBuffer(
      LowerBits(hci_spec::kWritePageScanType),
      UpperBits(hci_spec::kWritePageScanType),
      0x01,      // parameter_total_size (1 byte)
      scan_type  // Page_Scan_Type
      ));
}

DynamicByteBuffer WritePageScanTypeResponse() {
  return CommandCompletePacket(hci_spec::kWritePageScanType,
                               pw::bluetooth::emboss::StatusCode::SUCCESS);
}

DynamicByteBuffer WritePageTimeoutPacket(uint16_t page_timeout) {
  return DynamicByteBuffer(StaticByteBuffer(
      LowerBits(hci_spec::kWritePageTimeout),
      UpperBits(hci_spec::kWritePageTimeout),
      0x02,                     // parameter_total_size (2 bytes)
      LowerBits(page_timeout),  // Page_Timeout
      UpperBits(page_timeout)   // Page_Timeout
      ));
}

DynamicByteBuffer WritePinTypePacket(uint8_t pin_type) {
  return DynamicByteBuffer(StaticByteBuffer(
      LowerBits(hci_spec::kWritePinType),
      UpperBits(hci_spec::kWritePinType),
      0x01,     // parameter_total_size (1 byte)
      pin_type  // PIN_Type
      ));
}

DynamicByteBuffer WriteScanEnable(uint8_t scan_enable) {
  return DynamicByteBuffer(StaticByteBuffer(
      LowerBits(hci_spec::kWriteScanEnable),
      UpperBits(hci_spec::kWriteScanEnable),
      0x01,        // parameter_total_size (1 byte)
      scan_enable  // Scan_Enable
      ));
}

}  // namespace bt::testing
