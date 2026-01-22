// Copyright 2024 The Pigweed Authors
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

#include "pw_bluetooth_proxy_private/test_utils.h"

#include <cstdint>

#include "pw_allocator/null_allocator.h"
#include "pw_allocator/synchronized_allocator.h"
#include "pw_allocator/testing.h"
#include "pw_assert/check.h"
#include "pw_bluetooth/emboss_util.h"
#include "pw_bluetooth/hci_common.emb.h"
#include "pw_bluetooth/hci_data.emb.h"
#include "pw_bluetooth/hci_events.emb.h"
#include "pw_bluetooth/hci_h4.emb.h"
#include "pw_bluetooth/l2cap_frames.emb.h"
#include "pw_bluetooth_proxy/basic_l2cap_channel.h"
#include "pw_bluetooth_proxy/direction.h"
#include "pw_bluetooth_proxy/h4_packet.h"
#include "pw_bluetooth_proxy/internal/logical_transport.h"
#include "pw_bluetooth_proxy/l2cap_channel_common.h"
#include "pw_bluetooth_proxy/proxy_host.h"
#include "pw_status/status.h"
#include "pw_status/try.h"
#include "pw_unit_test/framework.h"

namespace pw::bluetooth::proxy {

// Allocate storage and populate an ACL packet header with the given length.
Result<AclFrameWithStorage> SetupAcl(uint16_t handle, uint16_t l2cap_length) {
  AclFrameWithStorage frame;

  frame.storage.resize(l2cap_length + emboss::AclDataFrame::MinSizeInBytes() +
                       AclFrameWithStorage::kH4HeaderSize);
  std::fill(frame.storage.begin(), frame.storage.end(), 0);
  frame.storage[0] = cpp23::to_underlying(emboss::H4PacketType::ACL_DATA);
  PW_TRY_ASSIGN(frame.writer,
                MakeEmbossWriter<emboss::AclDataFrameWriter>(frame.hci_span()));
  frame.writer.header().handle().Write(handle);
  frame.writer.header().packet_boundary_flag().Write(
      emboss::AclDataPacketBoundaryFlag::FIRST_NON_FLUSHABLE);
  frame.writer.header().broadcast_flag().Write(
      emboss::AclDataPacketBroadcastFlag::POINT_TO_POINT);
  frame.writer.data_total_length().Write(l2cap_length);
  EXPECT_EQ(l2cap_length,
            frame.writer.payload().BackingStorage().SizeInBytes());
  return frame;
}

Result<BFrameWithStorage> SetupBFrame(uint16_t handle,
                                      uint16_t channel_id,
                                      uint16_t bframe_len) {
  BFrameWithStorage frame;
  PW_TRY_ASSIGN(
      frame.acl,
      SetupAcl(handle,
               bframe_len + emboss::BasicL2capHeader::IntrinsicSizeInBytes()));

  PW_TRY_ASSIGN(frame.writer,
                MakeEmbossWriter<emboss::BFrameWriter>(
                    frame.acl.writer.payload().BackingStorage().data(),
                    frame.acl.writer.payload().SizeInBytes()));
  frame.writer.pdu_length().Write(bframe_len);
  frame.writer.channel_id().Write(channel_id);
  EXPECT_TRUE(frame.writer.Ok());
  EXPECT_EQ(frame.writer.payload().SizeInBytes(), bframe_len);
  return frame;
}

Result<CFrameWithStorage> SetupCFrame(uint16_t handle,
                                      uint16_t channel_id,
                                      uint16_t cframe_len) {
  CFrameWithStorage frame;
  PW_TRY_ASSIGN(
      frame.acl,
      SetupAcl(handle,
               cframe_len + emboss::BasicL2capHeader::IntrinsicSizeInBytes()));

  PW_TRY_ASSIGN(frame.writer,
                MakeEmbossWriter<emboss::CFrameWriter>(
                    frame.acl.writer.payload().BackingStorage().data(),
                    frame.acl.writer.payload().SizeInBytes()));
  frame.writer.pdu_length().Write(cframe_len);
  frame.writer.channel_id().Write(channel_id);
  EXPECT_TRUE(frame.writer.Ok());
  EXPECT_EQ(frame.writer.payload().SizeInBytes(), cframe_len);
  return frame;
}

Result<KFrameWithStorage> SetupKFrame(uint16_t handle,
                                      uint16_t channel_id,
                                      uint16_t mps,
                                      uint16_t segment_no,
                                      span<const uint8_t> payload) {
  uint16_t sdu_length_field_offset = segment_no == 0 ? kSduLengthFieldSize : 0;
  // Requested segment encodes payload starting at `payload[payload_offset]`.
  uint16_t payload_offset =
      segment_no * mps - (segment_no == 0 ? 0 : kSduLengthFieldSize);
  if (payload_offset >= payload.size()) {
    return Status::OutOfRange();
  }
  uint16_t remaining_payload_length = payload.size() - payload_offset;
  uint16_t segment_pdu_length =
      remaining_payload_length + sdu_length_field_offset;
  if (segment_pdu_length > mps) {
    segment_pdu_length = mps;
  }

  KFrameWithStorage kframe;
  PW_TRY_ASSIGN(kframe.acl,
                SetupAcl(handle,
                         segment_pdu_length +
                             emboss::BasicL2capHeader::IntrinsicSizeInBytes()));

  if (segment_no == 0) {
    PW_TRY_ASSIGN(kframe.writer,
                  MakeEmbossWriter<emboss::FirstKFrameWriter>(
                      kframe.acl.writer.payload().BackingStorage().data(),
                      kframe.acl.writer.payload().SizeInBytes()));
    emboss::FirstKFrameWriter first_kframe_writer =
        std::get<emboss::FirstKFrameWriter>(kframe.writer);
    first_kframe_writer.pdu_length().Write(segment_pdu_length);
    first_kframe_writer.channel_id().Write(channel_id);
    first_kframe_writer.sdu_length().Write(payload.size());
    EXPECT_TRUE(first_kframe_writer.Ok());
    PW_CHECK(TryToCopyToEmbossStruct(
        /*emboss_dest=*/first_kframe_writer.payload(),
        /*src=*/payload.subspan(payload_offset,
                                segment_pdu_length - sdu_length_field_offset)));
  } else {
    PW_TRY_ASSIGN(kframe.writer,
                  MakeEmbossWriter<emboss::SubsequentKFrameWriter>(
                      kframe.acl.writer.payload().BackingStorage().data(),
                      kframe.acl.writer.payload().SizeInBytes()));
    emboss::SubsequentKFrameWriter subsequent_kframe_writer =
        std::get<emboss::SubsequentKFrameWriter>(kframe.writer);
    subsequent_kframe_writer.pdu_length().Write(segment_pdu_length);
    subsequent_kframe_writer.channel_id().Write(channel_id);
    EXPECT_TRUE(subsequent_kframe_writer.Ok());
    PW_CHECK(TryToCopyToEmbossStruct(
        /*emboss_dest=*/subsequent_kframe_writer.payload(),
        /*src=*/payload.subspan(payload_offset,
                                segment_pdu_length - sdu_length_field_offset)));
  }

  return kframe;
}

// Send an LE_Read_Buffer_Size (V2) CommandComplete event to `proxy` to request
// the reservation of a number of LE ACL send credits.
Status ProxyHostTest::SendLeReadBufferResponseFromController(
    ProxyHost& proxy,
    uint8_t num_credits_to_reserve,
    uint16_t le_acl_data_packet_length) {
  std::array<
      uint8_t,
      emboss::LEReadBufferSizeV2CommandCompleteEventWriter::SizeInBytes()>
      hci_arr{};
  H4PacketWithHci h4_packet{emboss::H4PacketType::UNKNOWN, hci_arr};
  PW_TRY_ASSIGN(auto view,
                CreateAndPopulateToHostEventWriter<
                    emboss::LEReadBufferSizeV2CommandCompleteEventWriter>(
                    h4_packet, emboss::EventCode::COMMAND_COMPLETE));
  view.command_complete().command_opcode().Write(
      emboss::OpCode::LE_READ_BUFFER_SIZE_V2);
  view.total_num_le_acl_data_packets().Write(num_credits_to_reserve);
  view.le_acl_data_packet_length().Write(le_acl_data_packet_length);

  proxy.HandleH4HciFromController(std::move(h4_packet));
  RunDispatcher();
  return OkStatus();
}

Status ProxyHostTest::SendReadBufferResponseFromController(
    ProxyHost& proxy,
    uint8_t num_credits_to_reserve,
    uint16_t acl_data_packet_length) {
  std::array<uint8_t,
             emboss::ReadBufferSizeCommandCompleteEventWriter::SizeInBytes()>
      hci_arr{};
  H4PacketWithHci h4_packet{emboss::H4PacketType::UNKNOWN, hci_arr};
  PW_TRY_ASSIGN(auto view,
                CreateAndPopulateToHostEventWriter<
                    emboss::ReadBufferSizeCommandCompleteEventWriter>(
                    h4_packet, emboss::EventCode::COMMAND_COMPLETE));
  view.command_complete().command_opcode().Write(
      emboss::OpCode::READ_BUFFER_SIZE);
  view.total_num_acl_data_packets().Write(num_credits_to_reserve);
  view.acl_data_packet_length().Write(acl_data_packet_length);
  view.synchronous_data_packet_length().Write(0xFF);
  EXPECT_TRUE(view.Ok());

  proxy.HandleH4HciFromController(std::move(h4_packet));
  RunDispatcher();
  return OkStatus();
}

static constexpr size_t NumberOfCompletedPacketsSize(size_t num_connections) {
  return emboss::NumberOfCompletedPacketsEvent::MinSizeInBytes() +
         num_connections *
             emboss::NumberOfCompletedPacketsEventData::IntrinsicSizeInBytes();
}

Status ProxyHostTest::SendNumberOfCompletedPackets(
    ProxyHost& proxy,
    std::initializer_list<std::pair<uint16_t, uint16_t>>
        packets_per_connection) {
  constexpr const size_t kMaxConnections = 5;
  PW_CHECK_UINT_LE(packets_per_connection.size(), kMaxConnections);

  std::array<uint8_t, NumberOfCompletedPacketsSize(kMaxConnections)> hci_arr;
  hci_arr.fill(0);
  span<uint8_t> hci_span(
      hci_arr.data(),
      NumberOfCompletedPacketsSize(packets_per_connection.size()));
  H4PacketWithHci nocp_event{emboss::H4PacketType::EVENT, hci_span};
  PW_TRY_ASSIGN(auto view,
                MakeEmbossWriter<emboss::NumberOfCompletedPacketsEventWriter>(
                    nocp_event.GetHciSpan()));
  view.header().event_code().Write(
      emboss::EventCode::NUMBER_OF_COMPLETED_PACKETS);
  view.header().parameter_total_size().Write(
      nocp_event.GetHciSpan().size() -
      emboss::EventHeader::IntrinsicSizeInBytes());
  view.num_handles().Write(packets_per_connection.size());

  size_t i = 0;
  for (const auto& [handle, num_packets] : packets_per_connection) {
    view.nocp_data()[i].connection_handle().Write(handle);
    view.nocp_data()[i].num_completed_packets().Write(num_packets);
    ++i;
  }

  proxy.HandleH4HciFromController(std::move(nocp_event));
  RunDispatcher();
  return OkStatus();
}

// Send a Connection_Complete event to `proxy` indicating the provided
// `handle` has disconnected.
Status ProxyHostTest::SendConnectionCompleteEvent(ProxyHost& proxy,
                                                  uint16_t handle,
                                                  emboss::StatusCode status) {
  std::array<uint8_t, emboss::ConnectionCompleteEvent::IntrinsicSizeInBytes()>
      hci_arr{};
  H4PacketWithHci h4_packet{emboss::H4PacketType::EVENT, hci_arr};
  PW_TRY_ASSIGN(
      auto view,
      CreateAndPopulateToHostEventWriter<emboss::ConnectionCompleteEventWriter>(
          h4_packet, emboss::EventCode::CONNECTION_COMPLETE));
  view.status().Write(status);
  view.connection_handle().Write(handle);
  proxy.HandleH4HciFromController(std::move(h4_packet));
  RunDispatcher();
  return OkStatus();
}

// Send a LE_Connection_Complete event to `proxy` indicating the provided
// `handle` has disconnected.
Status ProxyHostTest::SendLeConnectionCompleteEvent(ProxyHost& proxy,
                                                    uint16_t handle,
                                                    emboss::StatusCode status) {
  std::array<uint8_t,
             emboss::LEConnectionCompleteSubevent::IntrinsicSizeInBytes()>
      hci_arr_dc{};
  H4PacketWithHci dc_event{emboss::H4PacketType::EVENT, hci_arr_dc};
  PW_TRY_ASSIGN(auto view,
                MakeEmbossWriter<emboss::LEConnectionCompleteSubeventWriter>(
                    dc_event.GetHciSpan()));
  view.le_meta_event().header().event_code().Write(
      emboss::EventCode::LE_META_EVENT);
  view.le_meta_event().subevent_code_enum().Write(
      emboss::LeSubEventCode::CONNECTION_COMPLETE);
  view.status().Write(status);
  view.connection_handle().Write(handle);
  view.connection_interval().Write(0x0006);
  view.supervision_timeout().Write(0x000A);
  proxy.HandleH4HciFromController(std::move(dc_event));
  RunDispatcher();
  return OkStatus();
}

// Send a Disconnection_Complete event to `proxy` indicating the provided
// `handle` has disconnected.
Status ProxyHostTest::SendDisconnectionCompleteEvent(ProxyHost& proxy,
                                                     uint16_t handle,
                                                     Direction direction,
                                                     bool successful) {
  std::array<uint8_t,
             sizeof(emboss::H4PacketType) +
                 emboss::DisconnectionCompleteEvent::IntrinsicSizeInBytes()>
      h4_arr_dc;
  h4_arr_dc.fill(0);
  H4PacketWithHci dc_event_from_controller{emboss::H4PacketType::EVENT,
                                           pw::span(h4_arr_dc).subspan(1)};
  H4PacketWithH4 dc_event_from_host{emboss::H4PacketType::EVENT, h4_arr_dc};
  PW_TRY_ASSIGN(auto view,
                MakeEmbossWriter<emboss::DisconnectionCompleteEventWriter>(
                    dc_event_from_controller.GetHciSpan()));
  view.header().event_code().Write(emboss::EventCode::DISCONNECTION_COMPLETE);
  view.header().parameter_total_size().Write(
      emboss::DisconnectionCompleteEvent::IntrinsicSizeInBytes() -
      emboss::EventHeader::IntrinsicSizeInBytes());
  view.status().Write(successful ? emboss::StatusCode::SUCCESS
                                 : emboss::StatusCode::HARDWARE_FAILURE);
  view.connection_handle().Write(handle);
  if (direction == Direction::kFromController) {
    proxy.HandleH4HciFromController(std::move(dc_event_from_controller));
  } else if (direction == Direction::kFromHost) {
    proxy.HandleH4HciFromHost(std::move(dc_event_from_host));
  } else {
    return Status::InvalidArgument();
  }
  RunDispatcher();
  return OkStatus();
}

Status ProxyHostTest::SendL2capConnectionReq(ProxyHost& proxy,
                                             Direction direction,
                                             uint16_t handle,
                                             uint16_t source_cid,
                                             uint16_t psm) {
  // First send CONNECTION_REQ to setup partial connection.
  constexpr size_t kConnectionReqLen =
      emboss::L2capConnectionReq::IntrinsicSizeInBytes();
  PW_TRY_ASSIGN(
      CFrameWithStorage cframe,
      SetupCFrame(handle,
                  cpp23::to_underlying(emboss::L2capFixedCid::ACL_U_SIGNALING),
                  kConnectionReqLen));

  emboss::L2capConnectionReqWriter conn_req_writer =
      emboss::MakeL2capConnectionReqView(
          cframe.writer.payload().BackingStorage().data(),
          cframe.writer.payload().SizeInBytes());
  conn_req_writer.command_header().code().Write(
      emboss::L2capSignalingPacketCode::CONNECTION_REQ);
  // Note data_length doesn't include command header.
  conn_req_writer.command_header().data_length().Write(
      kConnectionReqLen -
      emboss::L2capSignalingCommandHeader::IntrinsicSizeInBytes());
  conn_req_writer.psm().Write(psm);
  conn_req_writer.source_cid().Write(source_cid);

  if (direction == Direction::kFromController) {
    H4PacketWithHci packet{emboss::H4PacketType::ACL_DATA,
                           cframe.acl.hci_span()};
    proxy.HandleH4HciFromController(std::move(packet));
  } else if (direction == Direction::kFromHost) {
    H4PacketWithH4 packet{emboss::H4PacketType::ACL_DATA, cframe.acl.h4_span()};
    proxy.HandleH4HciFromHost(std::move(packet));
  } else {
    return Status::InvalidArgument();
  }
  RunDispatcher();
  return OkStatus();
}

Status ProxyHostTest::SendL2capConfigureReq(ProxyHost& proxy,
                                            Direction direction,
                                            uint16_t handle,
                                            uint16_t destination_cid,
                                            L2capOptions& l2cap_options) {
  size_t kOptionsSize = 0;
  if (l2cap_options.mtu.has_value()) {
    kOptionsSize += emboss::L2capMtuConfigurationOption::IntrinsicSizeInBytes();
  }
  const size_t kConfigureReqLen =
      emboss::L2capConfigureReq::MinSizeInBytes() + kOptionsSize;

  PW_TRY_ASSIGN(
      CFrameWithStorage cframe,
      SetupCFrame(handle,
                  cpp23::to_underlying(emboss::L2capFixedCid::ACL_U_SIGNALING),
                  kConfigureReqLen));

  auto configure_req_writer = MakeEmbossWriter<emboss::L2capConfigureReqWriter>(
      cframe.writer.payload().SizeInBytes(),
      cframe.writer.payload().BackingStorage().data(),
      cframe.writer.payload().SizeInBytes());
  configure_req_writer->command_header().code().Write(
      emboss::L2capSignalingPacketCode::CONFIGURATION_REQ);
  configure_req_writer->command_header().data_length().Write(
      kConfigureReqLen -
      emboss::L2capSignalingCommandHeader::IntrinsicSizeInBytes());
  configure_req_writer->destination_cid().Write(destination_cid);
  configure_req_writer->continuation_flag().Write(false);

  if (l2cap_options.mtu.has_value()) {
    constexpr size_t buffer_size =
        emboss::L2capMtuConfigurationOption::IntrinsicSizeInBytes();
    std::array<uint8_t, buffer_size> l2cap_option_buffer{};

    auto l2cap_option =
        MakeEmbossWriter<emboss::L2capMtuConfigurationOptionWriter>(
            &l2cap_option_buffer);
    l2cap_option->header().option_type().Write(
        emboss::L2capConfigurationOptionType::MTU);
    l2cap_option->header().option_length().Write(2);
    l2cap_option->mtu().Write(l2cap_options.mtu.value().mtu);

    memcpy(configure_req_writer->options().BackingStorage().data(),
           l2cap_option_buffer.data(),
           l2cap_option_buffer.size());
  }

  if (direction == Direction::kFromController) {
    H4PacketWithHci configuration_req_packet{emboss::H4PacketType::ACL_DATA,
                                             cframe.acl.hci_span()};
    proxy.HandleH4HciFromController(std::move(configuration_req_packet));
  } else {
    H4PacketWithH4 configure_req_packet{emboss::H4PacketType::ACL_DATA,
                                        cframe.acl.h4_span()};
    proxy.HandleH4HciFromHost(std::move(configure_req_packet));
  }
  RunDispatcher();
  return OkStatus();
}

Status ProxyHostTest::SendL2capConfigureRsp(
    ProxyHost& proxy,
    Direction direction,
    uint16_t handle,
    uint16_t local_cid,
    emboss::L2capConfigurationResult result) {
  constexpr size_t kConfigureRspLen =
      emboss::L2capConfigureRsp::MinSizeInBytes();
  PW_TRY_ASSIGN(
      CFrameWithStorage cframe,
      SetupCFrame(handle,
                  cpp23::to_underlying(emboss::L2capFixedCid::ACL_U_SIGNALING),
                  kConfigureRspLen));

  auto configure_rsp_writer = MakeEmbossWriter<emboss::L2capConfigureRspWriter>(
      cframe.writer.payload().SizeInBytes(),
      cframe.writer.payload().BackingStorage().data(),
      cframe.writer.payload().SizeInBytes());
  configure_rsp_writer->command_header().code().Write(
      emboss::L2capSignalingPacketCode::CONFIGURATION_RSP);
  configure_rsp_writer->command_header().data_length().Write(
      kConfigureRspLen -
      emboss::L2capSignalingCommandHeader::IntrinsicSizeInBytes());
  configure_rsp_writer->source_cid().Write(local_cid);
  configure_rsp_writer->continuation_flag().Write(false);
  configure_rsp_writer->result().Write(result);

  if (direction == Direction::kFromController) {
    H4PacketWithHci configure_rsp_packet{emboss::H4PacketType::ACL_DATA,
                                         cframe.acl.hci_span()};
    proxy.HandleH4HciFromController(std::move(configure_rsp_packet));
  } else {
    H4PacketWithH4 configure_rsp_packet{emboss::H4PacketType::ACL_DATA,
                                        cframe.acl.h4_span()};
    proxy.HandleH4HciFromHost(std::move(configure_rsp_packet));
  }
  RunDispatcher();
  return OkStatus();
}

Status ProxyHostTest::SendL2capConnectionRsp(
    ProxyHost& proxy,
    Direction direction,
    uint16_t handle,
    uint16_t source_cid,
    uint16_t destination_cid,
    emboss::L2capConnectionRspResultCode result_code) {
  constexpr size_t kConnectionRspLen =
      emboss::L2capConnectionRsp::MinSizeInBytes();
  PW_TRY_ASSIGN(
      auto cframe,
      SetupCFrame(handle,
                  cpp23::to_underlying(emboss::L2capFixedCid::ACL_U_SIGNALING),
                  kConnectionRspLen));

  emboss::L2capConnectionRspWriter conn_rsp_writer =
      emboss::MakeL2capConnectionRspView(
          cframe.writer.payload().BackingStorage().data(),
          cframe.writer.payload().SizeInBytes());
  conn_rsp_writer.command_header().code().Write(
      emboss::L2capSignalingPacketCode::CONNECTION_RSP);

  conn_rsp_writer.command_header().data_length().Write(
      kConnectionRspLen -
      emboss::L2capSignalingCommandHeader::IntrinsicSizeInBytes());
  conn_rsp_writer.source_cid().Write(source_cid);
  conn_rsp_writer.destination_cid().Write(destination_cid);
  conn_rsp_writer.result().Write(result_code);

  if (direction == Direction::kFromController) {
    H4PacketWithHci packet{emboss::H4PacketType::ACL_DATA,
                           cframe.acl.hci_span()};
    proxy.HandleH4HciFromController(std::move(packet));
  } else if (direction == Direction::kFromHost) {
    H4PacketWithH4 packet{emboss::H4PacketType::ACL_DATA, cframe.acl.h4_span()};
    proxy.HandleH4HciFromHost(std::move(packet));
  } else {
    return Status::InvalidArgument();
  }
  RunDispatcher();
  return OkStatus();
}

Status ProxyHostTest::SendL2capDisconnectRsp(ProxyHost& proxy,
                                             Direction direction,
                                             AclTransportType transport,
                                             uint16_t handle,
                                             uint16_t source_cid,
                                             uint16_t destination_cid) {
  constexpr size_t kDisconnectionRspLen =
      emboss::L2capDisconnectionRsp::MinSizeInBytes();
  PW_TRY_ASSIGN(
      auto cframe,
      SetupCFrame(
          handle,
          transport == AclTransportType::kBrEdr
              ? cpp23::to_underlying(emboss::L2capFixedCid::ACL_U_SIGNALING)
              : cpp23::to_underlying(emboss::L2capFixedCid::LE_U_SIGNALING),
          kDisconnectionRspLen));

  emboss::L2capDisconnectionRspWriter disconn_rsp_writer =
      emboss::MakeL2capDisconnectionRspView(
          cframe.writer.payload().BackingStorage().data(),
          cframe.writer.payload().SizeInBytes());
  disconn_rsp_writer.command_header().code().Write(
      emboss::L2capSignalingPacketCode::DISCONNECTION_RSP);

  disconn_rsp_writer.command_header().data_length().Write(
      kDisconnectionRspLen -
      emboss::L2capSignalingCommandHeader::IntrinsicSizeInBytes());
  disconn_rsp_writer.source_cid().Write(source_cid);
  disconn_rsp_writer.destination_cid().Write(destination_cid);

  if (direction == Direction::kFromHost) {
    H4PacketWithH4 packet{emboss::H4PacketType::ACL_DATA, cframe.acl.h4_span()};
    proxy.HandleH4HciFromHost(std::move(packet));
  } else if (direction == Direction::kFromController) {
    H4PacketWithHci packet{emboss::H4PacketType::ACL_DATA,
                           cframe.acl.hci_span()};
    proxy.HandleH4HciFromController(std::move(packet));
  } else {
    return Status::InvalidArgument();
  }
  RunDispatcher();
  return OkStatus();
}

void ProxyHostTest::SendL2capBFrame(ProxyHost& proxy,
                                    uint16_t handle,
                                    pw::span<const uint8_t> payload,
                                    size_t pdu_length,
                                    uint16_t channel_id) {
  constexpr size_t kHeadersSize =
      emboss::AclDataFrameHeader::IntrinsicSizeInBytes() +
      emboss::BasicL2capHeader::IntrinsicSizeInBytes();

  const size_t acl_data_size =
      emboss::BasicL2capHeader::IntrinsicSizeInBytes() + payload.size();

  std::vector<uint8_t> hci_buf(kHeadersSize + payload.size());
  H4PacketWithHci h4_packet{emboss::H4PacketType::ACL_DATA, hci_buf};

  // ACL header
  Result<emboss::AclDataFrameWriter> acl =
      MakeEmbossWriter<emboss::AclDataFrameWriter>(h4_packet.GetHciSpan());
  acl->header().handle().Write(handle);
  acl->data_total_length().Write(acl_data_size);

  // L2CAP B-Frame header
  emboss::BFrameWriter bframe = emboss::MakeBFrameView(
      acl->payload().BackingStorage().data(), acl->payload().SizeInBytes());
  bframe.pdu_length().Write(pdu_length);
  bframe.channel_id().Write(channel_id);

  // Payload
  std::copy(payload.begin(),
            payload.end(),
            h4_packet.GetHciSpan().begin() + kHeadersSize);

  proxy.HandleH4HciFromController(std::move(h4_packet));
  RunDispatcher();
}

void ProxyHostTest::SendAclContinuingFrag(ProxyHost& proxy,
                                          uint16_t handle,
                                          pw::span<const uint8_t> payload) {
  constexpr size_t kHeadersSize =
      emboss::AclDataFrameHeader::IntrinsicSizeInBytes();
  // No BasicL2capHeader.

  const size_t acl_data_size =
      // No BasicL2capHeader.
      payload.size();

  std::vector<uint8_t> hci_buf(kHeadersSize + payload.size());
  H4PacketWithHci h4_packet{emboss::H4PacketType::ACL_DATA, hci_buf};

  // ACL header
  Result<emboss::AclDataFrameWriter> acl =
      MakeEmbossWriter<emboss::AclDataFrameWriter>(h4_packet.GetHciSpan());
  acl->header().handle().Write(handle);
  acl->header().packet_boundary_flag().Write(
      emboss::AclDataPacketBoundaryFlag::CONTINUING_FRAGMENT);
  acl->header().broadcast_flag().Write(
      emboss::AclDataPacketBroadcastFlag::POINT_TO_POINT);
  acl->data_total_length().Write(acl_data_size);

  // Payload
  std::copy(payload.begin(),
            payload.end(),
            h4_packet.GetHciSpan().begin() + kHeadersSize);

  proxy.HandleH4HciFromController(std::move(h4_packet));
  RunDispatcher();
}

L2capChannel::State GetState(const internal::GenericL2capChannel& channel) {
  auto* l2cap = channel.InternalForTesting();
  return l2cap == nullptr ? L2capChannel::State::kClosed : l2cap->state();
}

Allocator* ProxyHostTest::GetProxyHostAllocator() {
  // This is static as it is too large to fit in the test fixture when using the
  // "light" backend.
  static allocator::test::AllocatorForTest<16384> alloc;
  static allocator::SynchronizedAllocator<sync::Mutex> sync{alloc};
  return &sync;
}

pw::Result<L2capCoc> ProxyHostTest::BuildCocWithResult(ProxyHost& proxy,
                                                       CocParameters params) {
  return proxy.AcquireL2capCoc(
      /*rx_multibuf_allocator=*/sut_multibuf_allocator_,
      params.handle,
      {.cid = params.local_cid,
       .mtu = params.rx_mtu,
       .mps = params.rx_mps,
       .credits = params.rx_credits},
      {.cid = params.remote_cid,
       .mtu = params.tx_mtu,
       .mps = params.tx_mps,
       .credits = params.tx_credits},
      std::move(params.receive_fn),
      std::move(params.event_fn));
}

L2capCoc ProxyHostTest::BuildCoc(ProxyHost& proxy, CocParameters params) {
  pw::Result<L2capCoc> channel = BuildCocWithResult(proxy, std::move(params));
  PW_TEST_EXPECT_OK(channel);
  return std::move(channel.value());
}

Result<BasicL2capChannel> ProxyHostTest::BuildBasicL2capChannelWithResult(
    ProxyHost& proxy, BasicL2capParameters params) {
  MultiBufAllocator* rx_multibuf_allocator = params.rx_multibuf_allocator
                                                 ? params.rx_multibuf_allocator
                                                 : &sut_multibuf_allocator_;
  return proxy.AcquireBasicL2capChannel(
      *rx_multibuf_allocator,
      params.handle,
      params.local_cid,
      params.remote_cid,
      params.transport,
      std::move(params.payload_from_controller_fn),
      std::move(params.payload_from_host_fn),
      std::move(params.event_fn));
}

BasicL2capChannel ProxyHostTest::BuildBasicL2capChannel(
    ProxyHost& proxy, BasicL2capParameters params) {
  pw::Result<BasicL2capChannel> channel =
      BuildBasicL2capChannelWithResult(proxy, std::move(params));
  PW_TEST_EXPECT_OK(channel);
  return std::move(channel.value());
}

Result<GattNotifyChannel> ProxyHostTest::BuildGattNotifyChannelWithResult(
    ProxyHost& proxy, GattNotifyChannelParameters params) {
  return proxy.AcquireGattNotifyChannel(
      params.handle, params.attribute_handle, std::move(params.event_fn));
}

GattNotifyChannel ProxyHostTest::BuildGattNotifyChannel(
    ProxyHost& proxy, GattNotifyChannelParameters params) {
  pw::Result<GattNotifyChannel> channel =
      BuildGattNotifyChannelWithResult(proxy, std::move(params));
  PW_TEST_EXPECT_OK(channel);
  return std::move(channel.value());
}

Result<UniquePtr<ChannelProxy>>
ProxyHostTest::BuildBasicModeChannelProxyWithResult(
    ProxyHost& proxy, BasicChannelProxyParameters&& params) {
  return proxy.InterceptBasicModeChannel(
      params.connection_handle,
      params.local_channel_id,
      params.remote_channel_id,
      params.transport,
      std::move(params.payload_from_controller_fn),
      std::move(params.payload_from_host_fn),
      std::move(params.event_fn));
}

UniquePtr<ChannelProxy> ProxyHostTest::BuildBasicModeChannelProxy(
    ProxyHost& proxy, BasicChannelProxyParameters&& params) {
  Result<UniquePtr<ChannelProxy>> result =
      BuildBasicModeChannelProxyWithResult(proxy, std::move(params));
  PW_TEST_EXPECT_OK(result);
  return std::move(result.value());
}

}  // namespace pw::bluetooth::proxy
