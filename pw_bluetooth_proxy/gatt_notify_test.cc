// Copyright 2025 The Pigweed Authors
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

#include <cstdint>

#include "pw_bluetooth/att.emb.h"
#include "pw_bluetooth/hci_data.emb.h"
#include "pw_bluetooth/hci_h4.emb.h"
#include "pw_bluetooth/l2cap_frames.emb.h"
#include "pw_bluetooth_proxy/h4_packet.h"
#include "pw_bluetooth_proxy/l2cap_channel_common.h"
#include "pw_bluetooth_proxy/proxy_host.h"
#include "pw_bluetooth_proxy_private/test_utils.h"
#include "pw_function/function.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_status/try.h"
#include "pw_unit_test/framework.h"
#include "pw_unit_test/status_macros.h"

namespace pw::bluetooth::proxy {

namespace {
constexpr uint16_t kConnectionHandle = 123;

struct AttNotifyWithStorage {
  BFrameWithStorage bframe;
  emboss::AttHandleValueNtfWriter writer;
};

Result<AttNotifyWithStorage> SetupAttNotify(
    uint16_t connection_handle,
    uint16_t attribute_handle,
    const pw::span<const uint8_t> attribute_value) {
  const uint16_t kAttChannelId = 0x0004;
  const size_t att_size =
      emboss::AttHandleValueNtf::MinSizeInBytes() + attribute_value.size();

  AttNotifyWithStorage frame;
  PW_TRY_ASSIGN(
      frame.bframe,
      SetupBFrame(
          connection_handle, kAttChannelId, static_cast<uint16_t>(att_size)));

  EXPECT_EQ(att_size, frame.bframe.writer.payload().SizeInBytes());
  PW_TRY_ASSIGN(frame.writer,
                MakeEmbossWriter<emboss::AttHandleValueNtfWriter>(
                    static_cast<int32_t>(attribute_value.size()),
                    frame.bframe.writer.payload().BackingStorage().data(),
                    frame.bframe.writer.payload().SizeInBytes()));
  EXPECT_TRUE(frame.writer.IsComplete());

  frame.writer.attribute_opcode().Write(
      emboss::AttOpcode::ATT_HANDLE_VALUE_NTF);
  frame.writer.attribute_handle().Write(attribute_handle);

  EXPECT_TRUE(
      TryToCopyToEmbossStruct(frame.writer.attribute_value(), attribute_value));

  return frame;
}

class GattNotifyTest : public ProxyHostTest {};

TEST_F(GattNotifyTest, TestSetupAttNotify) {
  std::array<uint8_t, 1> attribute_value = {0xFA};
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      AttNotifyWithStorage att,
      SetupAttNotify(0x0ACB, 0x4321, pw::span(attribute_value)));

  // Built from the preceding values in little endian order.
  std::array<uint8_t, 13> expected_gatt_notify_packet = {0x02,
                                                         0xCB,
                                                         0x0A,
                                                         0x08,
                                                         0x00,
                                                         0x04,
                                                         0x00,
                                                         0x04,
                                                         0x00,
                                                         0x1B,
                                                         0x21,
                                                         0x43,
                                                         0xFA};

  EXPECT_EQ(att.bframe.acl.h4_span().size(),
            expected_gatt_notify_packet.size());
  EXPECT_TRUE(std::equal(att.bframe.acl.h4_span().begin(),
                         att.bframe.acl.h4_span().end(),
                         expected_gatt_notify_packet.begin(),
                         expected_gatt_notify_packet.end()));
}

TEST_F(GattNotifyTest, GetAttributeHandle) {
  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  GattNotifyChannel channel =
      BuildGattNotifyChannel(proxy, {.attribute_handle = 0x234});
  EXPECT_EQ(channel.attribute_handle(), 0x234);
}

TEST_F(GattNotifyTest, Send1ByteAttribute) {
  struct {
    int sends_called = 0;
    // First four bits 0x0 encode PB & BC flags
    uint16_t handle = kConnectionHandle;
    // Length of L2CAP PDU
    uint16_t acl_data_total_length = 0x0008;
    // Length of ATT PDU
    uint16_t pdu_length = 0x0004;
    // Attribute protocol channel ID (0x0004)
    uint16_t channel_id = 0x0004;
    // ATT_HANDLE_VALUE_NTF opcode 0x1B
    uint8_t attribute_opcode = 0x1B;
    uint16_t attribute_handle = 0x4321;
    std::array<uint8_t, 1> attribute_value = {0xFA};
    AttNotifyWithStorage att;
  } capture;

  PW_TEST_ASSERT_OK_AND_ASSIGN(
      capture.att,
      SetupAttNotify(/*connection_handle=*/capture.handle,
                     /*attribute_handle=*/capture.attribute_handle,
                     capture.attribute_value));

  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      [&capture](H4PacketWithH4&& packet) {
        capture.sends_called++;
        EXPECT_EQ(packet.GetH4Type(), emboss::H4PacketType::ACL_DATA);
        EXPECT_EQ(packet.GetH4Span().size(),
                  capture.att.bframe.acl.h4_span().size());
        EXPECT_TRUE(std::equal(packet.GetH4Span().begin(),
                               packet.GetH4Span().end(),
                               capture.att.bframe.acl.h4_span().begin(),
                               capture.att.bframe.acl.h4_span().end()));
        PW_TEST_ASSERT_OK_AND_ASSIGN(
            auto acl,
            MakeEmbossView<emboss::AclDataFrameView>(packet.GetHciSpan()));
        emboss::BFrameView l2cap =
            emboss::MakeBFrameView(acl.payload().BackingStorage().data(),
                                   acl.data_total_length().Read());
        emboss::AttHandleValueNtfView gatt_notify =
            emboss::MakeAttHandleValueNtfView(
                capture.attribute_value.size(),
                l2cap.payload().BackingStorage().data(),
                l2cap.pdu_length().Read());
        EXPECT_EQ(acl.header().handle().Read(), capture.handle);
        EXPECT_EQ(acl.header().packet_boundary_flag().Read(),
                  emboss::AclDataPacketBoundaryFlag::FIRST_NON_FLUSHABLE);
        EXPECT_EQ(acl.header().broadcast_flag().Read(),
                  emboss::AclDataPacketBroadcastFlag::POINT_TO_POINT);
        EXPECT_EQ(acl.data_total_length().Read(),
                  capture.acl_data_total_length);
        EXPECT_EQ(l2cap.pdu_length().Read(), capture.pdu_length);
        EXPECT_EQ(l2cap.channel_id().Read(), capture.channel_id);
        EXPECT_EQ(gatt_notify.attribute_opcode().Read(),
                  static_cast<emboss::AttOpcode>(capture.attribute_opcode));
        EXPECT_EQ(gatt_notify.attribute_handle().Read(),
                  capture.attribute_handle);
        EXPECT_EQ(gatt_notify.attribute_value()[0].Read(),
                  capture.attribute_value[0]);
      });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/1,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  // Allow proxy to reserve 1 credit.
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, 1));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  GattNotifyChannel channel = BuildGattNotifyChannel(
      proxy,
      {.handle = capture.handle, .attribute_handle = capture.attribute_handle});
  multibuf::MultiBuf mbuf = MultiBufFromArray(capture.attribute_value);
  PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
  RunDispatcher();

  EXPECT_EQ(capture.sends_called, 1);
}

TEST_F(GattNotifyTest, Send2ByteAttribute) {
  struct {
    int sends_called = 0;
    // Max connection_handle value; first four bits 0x0 encode PB & BC flags
    const uint16_t handle = kConnectionHandle;
    // Length of L2CAP PDU
    const uint16_t acl_data_total_length = 0x0009;
    // Length of ATT PDU
    const uint16_t pdu_length = 0x0005;
    // Attribute protocol channel ID (0x0004)
    const uint16_t channel_id = 0x0004;
    // ATT_HANDLE_VALUE_NTF opcode 0x1B
    const uint8_t attribute_opcode = 0x1B;
    const uint16_t attribute_handle = 0x1234;
    const std::array<uint8_t, 2> attribute_value = {0xAB, 0xCD};
    AttNotifyWithStorage att;
  } capture;

  PW_TEST_ASSERT_OK_AND_ASSIGN(
      capture.att,
      SetupAttNotify(/*connection_handle=*/capture.handle,
                     /*attribute_handle=*/capture.attribute_handle,
                     capture.attribute_value));

  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      [&capture](H4PacketWithH4&& packet) {
        ++capture.sends_called;
        EXPECT_EQ(packet.GetH4Type(), emboss::H4PacketType::ACL_DATA);
        EXPECT_EQ(packet.GetH4Span().size(),
                  capture.att.bframe.acl.h4_span().size());
        EXPECT_TRUE(std::equal(packet.GetH4Span().begin(),
                               packet.GetH4Span().end(),
                               capture.att.bframe.acl.h4_span().begin(),
                               capture.att.bframe.acl.h4_span().end()));
        PW_TEST_ASSERT_OK_AND_ASSIGN(
            auto acl,
            MakeEmbossView<emboss::AclDataFrameView>(packet.GetHciSpan()));
        emboss::BFrameView l2cap = emboss::MakeBFrameView(
            acl.payload().BackingStorage().data(), acl.SizeInBytes());
        emboss::AttHandleValueNtfView gatt_notify =
            emboss::MakeAttHandleValueNtfView(
                capture.attribute_value.size(),
                l2cap.payload().BackingStorage().data(),
                l2cap.pdu_length().Read());
        EXPECT_EQ(acl.header().handle().Read(), capture.handle);
        EXPECT_EQ(acl.header().packet_boundary_flag().Read(),
                  emboss::AclDataPacketBoundaryFlag::FIRST_NON_FLUSHABLE);
        EXPECT_EQ(acl.header().broadcast_flag().Read(),
                  emboss::AclDataPacketBroadcastFlag::POINT_TO_POINT);
        EXPECT_EQ(acl.data_total_length().Read(),
                  capture.acl_data_total_length);
        EXPECT_EQ(l2cap.pdu_length().Read(), capture.pdu_length);
        EXPECT_EQ(l2cap.channel_id().Read(), capture.channel_id);
        EXPECT_EQ(gatt_notify.attribute_opcode().Read(),
                  static_cast<emboss::AttOpcode>(capture.attribute_opcode));
        EXPECT_EQ(gatt_notify.attribute_handle().Read(),
                  capture.attribute_handle);
        EXPECT_EQ(gatt_notify.attribute_value()[0].Read(),
                  capture.attribute_value[0]);
        EXPECT_EQ(gatt_notify.attribute_value()[1].Read(),
                  capture.attribute_value[1]);
      });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/1,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  // Allow proxy to reserve 1 credit.
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, 1));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  GattNotifyChannel channel = BuildGattNotifyChannel(
      proxy,
      {.handle = capture.handle, .attribute_handle = capture.attribute_handle});
  multibuf::MultiBuf mbuf = MultiBufFromArray(capture.attribute_value);
  PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
  RunDispatcher();

  EXPECT_EQ(capture.sends_called, 1);
}

TEST_F(GattNotifyTest, ReturnsErrorIfAttributeTooLarge) {
  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4 packet) { FAIL(); });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  const uint16_t kLeAclLength = 250;
  PW_TEST_EXPECT_OK(
      SendLeReadBufferResponseFromController(proxy, 0, kLeAclLength));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  // attribute_value 1 byte too large
  std::array<uint8_t,
             kLeAclLength - emboss::BasicL2capHeader::IntrinsicSizeInBytes() -
                 emboss::AttHandleValueNtf::MinSizeInBytes() + 1>
      attribute_value_too_large;
  GattNotifyChannel channel = BuildGattNotifyChannel(proxy, {});
  multibuf::MultiBuf mbuf = MultiBufFromArray(attribute_value_too_large);
  EXPECT_EQ(channel.Write(std::move(mbuf)).status, Status::InvalidArgument());
}

TEST_F(GattNotifyTest, ChannelIsNotConstructedIfParametersInvalid) {
  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4 packet) { FAIL(); });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  // attribute value is zero
  EXPECT_EQ(
      BuildGattNotifyChannelWithResult(proxy, {.attribute_handle = 0}).status(),
      PW_STATUS_INVALID_ARGUMENT);

  // connection_handle too large
  EXPECT_EQ(
      BuildGattNotifyChannelWithResult(proxy, {.handle = 0x0FFF}).status(),
      PW_STATUS_INVALID_ARGUMENT);
}

TEST_F(GattNotifyTest, PayloadIsReturnedOnError) {
  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4 packet) { FAIL(); });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  const std::array<const uint8_t, 2> attribute_value = {5};

  GattNotifyChannel channel = BuildGattNotifyChannel(proxy, {});
  multibuf::MultiBuf mbuf = MultiBufFromSpan(pw::span{attribute_value});
  StatusWithMultiBuf result = channel.Write(std::move(mbuf));
  ASSERT_EQ(result.status, Status::FailedPrecondition());
  auto bytes = result.buf->ContiguousSpan().value();
  EXPECT_EQ(bytes.size(), attribute_value.size());
  EXPECT_EQ(bytes[0], static_cast<const std::byte>(attribute_value[0]));
}

}  // namespace
}  // namespace pw::bluetooth::proxy
