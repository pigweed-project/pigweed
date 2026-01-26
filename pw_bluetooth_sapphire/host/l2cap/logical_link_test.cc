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

#include "pw_bluetooth_sapphire/internal/host/l2cap/logical_link.h"

#include <memory>
#include <optional>

#include "pw_bluetooth_sapphire/fake_lease_provider.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"
#include "pw_bluetooth_sapphire/internal/host/hci/connection.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/channel.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/l2cap_defs.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/logical_link.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/test_packets.h"
#include "pw_bluetooth_sapphire/internal/host/testing/controller_test.h"
#include "pw_bluetooth_sapphire/internal/host/testing/mock_controller.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_packets.h"
#include "pw_bluetooth_sapphire/internal/host/transport/acl_data_channel.h"
#include "pw_bluetooth_sapphire/internal/host/transport/link_type.h"

namespace bt::l2cap::internal {
namespace {
using Conn = hci::Connection;

using TestingBase =
    bt::testing::FakeDispatcherControllerTest<bt::testing::MockController>;

const hci_spec::ConnectionHandle kConnHandle = 0x0001;

class LogicalLinkTest : public TestingBase {
 public:
  LogicalLinkTest() = default;
  ~LogicalLinkTest() override = default;
  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(LogicalLinkTest);

 protected:
  void SetUp() override {
    TestingBase::SetUp();
    InitializeACLDataChannel();

    NewLogicalLink();
  }
  void TearDown() override {
    if (link_) {
      link_->Close();
      link_ = nullptr;
    }

    a2dp_offload_manager_ = nullptr;

    TestingBase::TearDown();
  }
  void NewLogicalLink(bt::LinkType type = bt::LinkType::kLE,
                      bool random_channel_ids = true) {
    const size_t kMaxPayload = kDefaultMTU;
    auto query_service_cb = [](hci_spec::ConnectionHandle, Psm) {
      return std::nullopt;
    };
    a2dp_offload_manager_ = std::make_unique<A2dpOffloadManager>(
        transport()->command_channel()->AsWeakPtr());
    link_ = std::make_unique<LogicalLink>(
        kConnHandle,
        type,
        pw::bluetooth::emboss::ConnectionRole::CENTRAL,
        kMaxPayload,
        std::move(query_service_cb),
        transport()->acl_data_channel(),
        transport()->command_channel(),
        random_channel_ids,
        *a2dp_offload_manager_,
        dispatcher(),
        lease_provider_);
  }
  void ResetAndCreateNewLogicalLink(LinkType type = LinkType::kACL,
                                    bool random_channel_ids = true) {
    link()->Close();
    DeleteLink();
    NewLogicalLink(type, random_channel_ids);
  }

  LogicalLink* link() const { return link_.get(); }
  void DeleteLink() { link_ = nullptr; }

 private:
  pw::bluetooth_sapphire::testing::FakeLeaseProvider lease_provider_;
  std::unique_ptr<LogicalLink> link_;
  std::unique_ptr<A2dpOffloadManager> a2dp_offload_manager_;
};

struct QueueAclConnectionRetVal {
  l2cap::CommandId extended_features_id;
  l2cap::CommandId fixed_channels_supported_id;
};

static constexpr l2cap::ExtendedFeatures kExtendedFeatures =
    l2cap::kExtendedFeaturesBitEnhancedRetransmission;

using LogicalLinkDeathTest = LogicalLinkTest;

TEST_F(LogicalLinkDeathTest, DestructedWithoutClosingDies) {
  // Deleting the link without calling `Close` on it should trigger an
  // assertion.
  ASSERT_DEATH_IF_SUPPORTED(DeleteLink(), ".*closed.*");
}

TEST_F(LogicalLinkTest, FixedChannelHasCorrectMtu) {
  Channel::WeakPtr fixed_chan = link()->OpenFixedChannel(kATTChannelId);
  ASSERT_TRUE(fixed_chan.is_alive());
  EXPECT_EQ(kMaxMTU, fixed_chan->max_rx_sdu_size());
  EXPECT_EQ(kMaxMTU, fixed_chan->max_tx_sdu_size());
}

TEST_F(LogicalLinkTest, DropsBroadcastPackets) {
  ResetAndCreateNewLogicalLink();

  QueueAclConnectionRetVal cmd_ids;
  cmd_ids.extended_features_id = 1;
  cmd_ids.fixed_channels_supported_id = 2;

  const auto kExtFeaturesRsp = l2cap::testing::AclExtFeaturesInfoRsp(
      cmd_ids.extended_features_id, kConnHandle, kExtendedFeatures);
  EXPECT_ACL_PACKET_OUT(test_device(),
                        l2cap::testing::AclExtFeaturesInfoReq(
                            cmd_ids.extended_features_id, kConnHandle),
                        &kExtFeaturesRsp);
  EXPECT_ACL_PACKET_OUT(test_device(),
                        l2cap::testing::AclFixedChannelsSupportedInfoReq(
                            cmd_ids.fixed_channels_supported_id, kConnHandle));

  Channel::WeakPtr connectionless_chan =
      link()->OpenFixedChannel(kConnectionlessChannelId);
  ASSERT_TRUE(connectionless_chan.is_alive());

  size_t rx_count = 0;
  bool activated = connectionless_chan->Activate(
      [&](ByteBufferPtr) { rx_count++; }, []() {});
  ASSERT_TRUE(activated);

  StaticByteBuffer group_frame(0x0A,
                               0x00,  // Length (PSM + info = 10)
                               0x02,
                               0x00,  // Connectionless data channel
                               0xF0,
                               0x0F,  // PSM
                               'S',
                               'a',
                               'p',
                               'p',
                               'h',
                               'i',
                               'r',
                               'e'  // Info Payload
  );
  hci::ACLDataPacketPtr packet = hci::ACLDataPacket::New(
      kConnHandle,
      hci_spec::ACLPacketBoundaryFlag::kCompletePDU,
      hci_spec::ACLBroadcastFlag::kActivePeripheralBroadcast,
      static_cast<uint16_t>(group_frame.size()));
  ASSERT_TRUE(packet);
  packet->mutable_view()->mutable_payload_data().Write(group_frame);

  link()->HandleRxPacket(std::move(packet));

  // Should be dropped.
  EXPECT_EQ(0u, rx_count);
}

// LE links are unsupported, so result should be an error. No command should be
// sent.
TEST_F(LogicalLinkTest, SetBrEdrAutomaticFlushTimeoutFailsForLELink) {
  constexpr std::chrono::milliseconds kTimeout(100);
  ResetAndCreateNewLogicalLink(LinkType::kLE);

  bool cb_called = false;
  link()->SetBrEdrAutomaticFlushTimeout(kTimeout, [&](auto result) {
    cb_called = true;
    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(
        ToResult(
            pw::bluetooth::emboss::StatusCode::INVALID_HCI_COMMAND_PARAMETERS),
        result.error_value());
  });
  EXPECT_TRUE(cb_called);
}

TEST_F(LogicalLinkTest, SetAutomaticFlushTimeoutSuccess) {
  ResetAndCreateNewLogicalLink();

  QueueAclConnectionRetVal cmd_ids;
  cmd_ids.extended_features_id = 1;
  cmd_ids.fixed_channels_supported_id = 2;

  const auto kExtFeaturesRsp = l2cap::testing::AclExtFeaturesInfoRsp(
      cmd_ids.extended_features_id, kConnHandle, kExtendedFeatures);
  EXPECT_ACL_PACKET_OUT(test_device(),
                        l2cap::testing::AclExtFeaturesInfoReq(
                            cmd_ids.extended_features_id, kConnHandle),
                        &kExtFeaturesRsp);
  EXPECT_ACL_PACKET_OUT(test_device(),
                        l2cap::testing::AclFixedChannelsSupportedInfoReq(
                            cmd_ids.fixed_channels_supported_id, kConnHandle));

  std::optional<hci::Result<>> cb_status;
  auto result_cb = [&](auto status) { cb_status = status; };

  // Test command complete error
  const auto kCommandCompleteError = bt::testing::CommandCompletePacket(
      hci_spec::kWriteAutomaticFlushTimeout,
      pw::bluetooth::emboss::StatusCode::UNKNOWN_CONNECTION_ID);
  EXPECT_CMD_PACKET_OUT(
      test_device(),
      bt::testing::WriteAutomaticFlushTimeoutPacket(link()->handle(), 0),
      &kCommandCompleteError);
  link()->SetBrEdrAutomaticFlushTimeout(
      pw::chrono::SystemClock::duration::max(), result_cb);
  RunUntilIdle();
  ASSERT_TRUE(cb_status.has_value());
  ASSERT_TRUE(cb_status->is_error());
  EXPECT_EQ(ToResult(pw::bluetooth::emboss::StatusCode::UNKNOWN_CONNECTION_ID),
            *cb_status);
  cb_status.reset();

  // Test flush timeout = 0 (no command should be sent)
  link()->SetBrEdrAutomaticFlushTimeout(std::chrono::milliseconds(0),
                                        result_cb);
  RunUntilIdle();
  ASSERT_TRUE(cb_status.has_value());
  EXPECT_TRUE(cb_status->is_error());
  EXPECT_EQ(
      ToResult(
          pw::bluetooth::emboss::StatusCode::INVALID_HCI_COMMAND_PARAMETERS),
      *cb_status);

  // Test infinite flush timeout (flush timeout of 0 should be sent).
  const auto kCommandComplete = bt::testing::CommandCompletePacket(
      hci_spec::kWriteAutomaticFlushTimeout,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(),
      bt::testing::WriteAutomaticFlushTimeoutPacket(link()->handle(), 0),
      &kCommandComplete);
  link()->SetBrEdrAutomaticFlushTimeout(
      pw::chrono::SystemClock::duration::max(), result_cb);
  RunUntilIdle();
  ASSERT_TRUE(cb_status.has_value());
  EXPECT_EQ(fit::ok(), *cb_status);
  cb_status.reset();

  // Test msec to parameter conversion
  // (hci_spec::kMaxAutomaticFlushTimeoutDuration(1279) * conversion_factor(1.6)
  // = 2046).
  EXPECT_CMD_PACKET_OUT(
      test_device(),
      bt::testing::WriteAutomaticFlushTimeoutPacket(link()->handle(), 2046),
      &kCommandComplete);
  link()->SetBrEdrAutomaticFlushTimeout(
      hci_spec::kMaxAutomaticFlushTimeoutDuration, result_cb);
  RunUntilIdle();
  ASSERT_TRUE(cb_status.has_value());
  EXPECT_EQ(fit::ok(), *cb_status);
  cb_status.reset();

  // Test too large flush timeout (no command should be sent).
  link()->SetBrEdrAutomaticFlushTimeout(
      hci_spec::kMaxAutomaticFlushTimeoutDuration +
          std::chrono::milliseconds(1),
      result_cb);
  RunUntilIdle();
  ASSERT_TRUE(cb_status.has_value());
  EXPECT_TRUE(cb_status->is_error());
  EXPECT_EQ(
      ToResult(
          pw::bluetooth::emboss::StatusCode::INVALID_HCI_COMMAND_PARAMETERS),
      *cb_status);
}

TEST_F(LogicalLinkTest, OpensLeDynamicChannel) {
  ResetAndCreateNewLogicalLink(LinkType::kLE, false);
  static constexpr uint16_t kPsm = 0x015;
  static constexpr ChannelParameters kParams{
      .mode = CreditBasedFlowControlMode::kLeCreditBasedFlowControl,
      .max_rx_sdu_size = std::nullopt,
      .flush_timeout = std::nullopt,
  };

  transport()->acl_data_channel()->SetDataRxHandler(
      fit::bind_member<&LogicalLink::HandleRxPacket>(link()));

  const auto req =
      l2cap::testing::AclLeCreditBasedConnectionReq(1,
                                                    kConnHandle,
                                                    kPsm,
                                                    kFirstDynamicChannelId,
                                                    kDefaultMTU,
                                                    kMaxInboundPduPayloadSize,
                                                    /*credits=*/0);
  const auto rsp = l2cap::testing::AclLeCreditBasedConnectionRsp(
      /*id=*/1,
      /*link_handle=*/kConnHandle,
      /*cid=*/kFirstDynamicChannelId,
      /*mtu=*/64,
      /*mps=*/32,
      /*credits=*/1,
      /*result=*/LECreditBasedConnectionResult::kSuccess);
  EXPECT_ACL_PACKET_OUT(test_device(), req, &rsp);

  WeakPtr<Channel> channel;
  link()->OpenChannel(
      kPsm, kParams, [&](auto result) { channel = std::move(result); });
  RunUntilIdle();
  ASSERT_TRUE(channel.is_alive());
  channel->Activate([](auto) {}, []() {});

  EXPECT_ACL_PACKET_OUT(
      test_device(),
      l2cap::testing::AclKFrame(
          kConnHandle, channel->remote_id(), StaticByteBuffer(0x08)));
  channel->Send(NewBuffer(0x08));
  channel->Send(NewBuffer(0x09));
  RunUntilIdle();

  EXPECT_ACL_PACKET_OUT(
      test_device(),
      l2cap::testing::AclKFrame(
          kConnHandle, channel->remote_id(), StaticByteBuffer(0x09)));
  test_device()->SendACLDataChannelPacket(
      l2cap::testing::AclFlowControlCreditInd(
          1, kConnHandle, channel->remote_id(), /*credits=*/1));
  RunUntilIdle();
}

TEST_F(LogicalLinkTest, OpenFixedChannelsAsync) {
  ResetAndCreateNewLogicalLink();
  transport()->acl_data_channel()->SetDataRxHandler(
      fit::bind_member<&LogicalLink::HandleRxPacket>(link()));

  QueueAclConnectionRetVal cmd_ids;
  cmd_ids.extended_features_id = 1;
  cmd_ids.fixed_channels_supported_id = 2;

  EXPECT_ACL_PACKET_OUT(test_device(),
                        l2cap::testing::AclExtFeaturesInfoReq(
                            cmd_ids.extended_features_id, kConnHandle));
  const auto kFixedChannelsRsp =
      l2cap::testing::AclFixedChannelsSupportedInfoRsp(
          cmd_ids.fixed_channels_supported_id,
          kConnHandle,
          kFixedChannelsSupportedBitSM |
              kFixedChannelsSupportedBitConnectionless);
  EXPECT_ACL_PACKET_OUT(test_device(),
                        l2cap::testing::AclFixedChannelsSupportedInfoReq(
                            cmd_ids.fixed_channels_supported_id, kConnHandle),
                        &kFixedChannelsRsp);

  std::optional<Channel::WeakPtr> sm_channel;
  link()->OpenFixedChannelAsync(
      kSMPChannelId,
      [&sm_channel](Channel::WeakPtr chan) { sm_channel = std::move(chan); });
  EXPECT_FALSE(sm_channel.has_value());
  std::optional<Channel::WeakPtr> connectionless_channel;
  link()->OpenFixedChannelAsync(
      kConnectionlessChannelId,
      [&connectionless_channel](Channel::WeakPtr chan) {
        connectionless_channel = std::move(chan);
      });
  EXPECT_FALSE(connectionless_channel.has_value());
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
  ASSERT_TRUE(sm_channel.has_value());
  ASSERT_TRUE(sm_channel.value().is_alive());
  ASSERT_TRUE(connectionless_channel.has_value());
  ASSERT_TRUE(connectionless_channel.value().is_alive());
}

TEST_F(LogicalLinkTest, OpenFixedChannelAsyncFailureNotSupported) {
  ResetAndCreateNewLogicalLink();
  transport()->acl_data_channel()->SetDataRxHandler(
      fit::bind_member<&LogicalLink::HandleRxPacket>(link()));

  QueueAclConnectionRetVal cmd_ids;
  cmd_ids.extended_features_id = 1;
  cmd_ids.fixed_channels_supported_id = 2;

  const auto kExtFeaturesRsp = l2cap::testing::AclExtFeaturesInfoRsp(
      cmd_ids.extended_features_id, kConnHandle, kExtendedFeatures);
  EXPECT_ACL_PACKET_OUT(test_device(),
                        l2cap::testing::AclExtFeaturesInfoReq(
                            cmd_ids.extended_features_id, kConnHandle),
                        &kExtFeaturesRsp);
  const auto kFixedChannelsRsp =
      l2cap::testing::AclFixedChannelsSupportedInfoRsp(
          cmd_ids.fixed_channels_supported_id,
          kConnHandle,
          kFixedChannelsSupportedBitSignaling);  // SM not supported
  EXPECT_ACL_PACKET_OUT(test_device(),
                        l2cap::testing::AclFixedChannelsSupportedInfoReq(
                            cmd_ids.fixed_channels_supported_id, kConnHandle),
                        &kFixedChannelsRsp);

  std::optional<Channel::WeakPtr> channel;
  link()->OpenFixedChannelAsync(
      kSMPChannelId,
      [&channel](Channel::WeakPtr chan) { channel = std::move(chan); });
  EXPECT_FALSE(channel.has_value());
  RunUntilIdle();
  ASSERT_TRUE(channel.has_value());
  ASSERT_FALSE(channel.value().is_alive());
}

TEST_F(LogicalLinkTest, SignalCreditsAvailable) {
  constexpr ChannelId kExpectedCid = 0x4321;
  constexpr uint16_t kExpectedCredits = 0x3141;
  ResetAndCreateNewLogicalLink(LinkType::kLE, false);

  const auto cmd = l2cap::testing::AclFlowControlCreditInd(
      1, kConnHandle, kExpectedCid, kExpectedCredits);
  EXPECT_ACL_PACKET_OUT(test_device(), cmd);
  link()->SignalCreditsAvailable(kExpectedCid, kExpectedCredits);
  RunUntilIdle();
}

TEST_F(LogicalLinkTest, AutoSniffDisabledOnLELink) {
  ResetAndCreateNewLogicalLink(LinkType::kLE);
  // Autosniff is enabled on ACL links.
  ASSERT_FALSE(link()->AutosniffEnabled());
}

TEST_F(LogicalLinkTest, GoesIntoSniffModeWhenInactive) {
  ResetAndCreateNewLogicalLink(LinkType::kACL);

  // Autosniff is enabled on ACL links.
  ASSERT_TRUE(link()->AutosniffEnabled());

  // ==== Stage 0: General setup =====
  transport()->acl_data_channel()->SetDataRxHandler(
      fit::bind_member<&LogicalLink::HandleRxPacket>(link()));

  QueueAclConnectionRetVal cmd_ids;
  cmd_ids.extended_features_id = 1;
  cmd_ids.fixed_channels_supported_id = 2;
  const auto kExtFeaturesRsp = l2cap::testing::AclExtFeaturesInfoRsp(
      cmd_ids.extended_features_id, kConnHandle, kExtendedFeatures);
  EXPECT_ACL_PACKET_OUT(test_device(),
                        l2cap::testing::AclExtFeaturesInfoReq(
                            cmd_ids.extended_features_id, kConnHandle),
                        &kExtFeaturesRsp);

  const auto kFixedChannelsRsp =
      l2cap::testing::AclFixedChannelsSupportedInfoRsp(
          cmd_ids.fixed_channels_supported_id,
          kConnHandle,
          kFixedChannelsSupportedBitSignaling);  // SM not supported

  EXPECT_ACL_PACKET_OUT(test_device(),
                        l2cap::testing::AclFixedChannelsSupportedInfoReq(
                            cmd_ids.fixed_channels_supported_id, kConnHandle),
                        &kFixedChannelsRsp);

  RunUntilIdle();

  // ==== Stage 1: Idle link leads to request sniff mode  =====
  const auto kEnterSniffCmdComplete = bt::testing::CommandCompletePacket(
      hci_spec::kSniffMode, pw::bluetooth::emboss::StatusCode::SUCCESS);

  // Now we run for a bit to generate an idle autosniff trigger
  const auto enter_sniff_cmd =
      StaticByteBuffer(LowerBits(hci_spec::kSniffMode),
                       UpperBits(hci_spec::kSniffMode),
                       0x0a,  // parameter_total_size (10 byte payload)
                       LowerBits(kConnHandle),
                       UpperBits(kConnHandle),
                       // Max interval (816)
                       0x30,
                       0x03,
                       // Min interval (400)
                       0x90,
                       0x01,
                       // sniff attempt (4)
                       0x04,
                       0x00,
                       // Sniff timeout (1)
                       0x01,
                       0x00);

  // ==== Stage 1: Idle link leads to request sniff mode  =====
  // Autosniff code should trigger this command
  EXPECT_CMD_PACKET_OUT(
      test_device(), enter_sniff_cmd, &kEnterSniffCmdComplete);

  // Run for at least the autosniff timeout + a bit.
  RunFor(LogicalLink::kAutosniffTimeout + std::chrono::milliseconds(1));

  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());

  // ==== Stage 2: Send the link into sniff mode =====

  // Mock the event to go into sniff mode
  auto into_sniff_event =
      hci::EventPacket::New<pw::bluetooth::emboss::ModeChangeEventWriter>(
          hci_spec::kModeChangeEventCode);
  into_sniff_event.view_t().status().Write(
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  into_sniff_event.view_t().connection_handle().Write(kConnHandle);
  into_sniff_event.view_t().current_mode().Write(
      pw::bluetooth::emboss::AclConnectionMode::SNIFF);

  test_device()->SendCommandChannelPacket(into_sniff_event.data());

  RunUntilIdle();

  // Hopefully now we should be in sniff mode
  ASSERT_EQ(link()->AutosniffMode(),
            pw::bluetooth::emboss::AclConnectionMode::SNIFF);

  bt_log(INFO, "logical_link_test", "Entered sniff mode while idle");

  // Run any pending tasks.
  RunUntilIdle();

  // ==== Stage 3: Send data packet on link to come off sniff mode ====

  const auto kExitSniffCommandComplete = bt::testing::CommandCompletePacket(
      hci_spec::kExitSniffMode, pw::bluetooth::emboss::StatusCode::SUCCESS);
  // Autosniff code should send the following command
  auto exit_sniff_cmd =
      StaticByteBuffer(LowerBits(hci_spec::kExitSniffMode),
                       UpperBits(hci_spec::kExitSniffMode),
                       0x02,  // parameter_total_size (10 byte payload)
                       LowerBits(kConnHandle),
                       UpperBits(kConnHandle));
  EXPECT_CMD_PACKET_OUT(
      test_device(), exit_sniff_cmd, &kExitSniffCommandComplete);

  test_device()->SendACLDataChannelPacket(
      l2cap::testing::AclFlowControlCreditInd(
          1, kConnHandle, 1, /*credits=*/1));

  RunUntilIdle();

  // ==== Stage 4: Switch link into ACTIVE mode ====

  // Mock the mode change back into active.
  auto into_active_event =
      hci::EventPacket::New<pw::bluetooth::emboss::ModeChangeEventWriter>(
          hci_spec::kModeChangeEventCode);
  into_active_event.view_t().status().Write(
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  into_active_event.view_t().connection_handle().Write(kConnHandle);
  into_active_event.view_t().current_mode().Write(
      pw::bluetooth::emboss::AclConnectionMode::ACTIVE);

  test_device()->SendCommandChannelPacket(into_active_event.data());

  RunUntilIdle();
  // Should be back to active mode
  ASSERT_EQ(link()->AutosniffMode(),
            pw::bluetooth::emboss::AclConnectionMode::ACTIVE);

  // ==== Stage 5: Goes back into sniff mode after timer expiring again ====

  // Run for at least the autosniff timeout + a bit.
  EXPECT_CMD_PACKET_OUT(
      test_device(), enter_sniff_cmd, &kEnterSniffCmdComplete);
  RunFor(LogicalLink::kAutosniffTimeout + std::chrono::milliseconds(1));
  RunUntilIdle();
}

TEST_F(LogicalLinkTest, SniffSuppression) {
  ResetAndCreateNewLogicalLink(LinkType::kACL);

  // Autosniff is enabled on ACL links.
  ASSERT_TRUE(link()->AutosniffEnabled());

  // ==== Stage 0: General setup =====
  transport()->acl_data_channel()->SetDataRxHandler(
      fit::bind_member<&LogicalLink::HandleRxPacket>(link()));

  QueueAclConnectionRetVal cmd_ids;
  cmd_ids.extended_features_id = 1;
  cmd_ids.fixed_channels_supported_id = 2;
  const auto kExtFeaturesRsp = l2cap::testing::AclExtFeaturesInfoRsp(
      cmd_ids.extended_features_id, kConnHandle, kExtendedFeatures);
  EXPECT_ACL_PACKET_OUT(test_device(),
                        l2cap::testing::AclExtFeaturesInfoReq(
                            cmd_ids.extended_features_id, kConnHandle),
                        &kExtFeaturesRsp);

  const auto kFixedChannelsRsp =
      l2cap::testing::AclFixedChannelsSupportedInfoRsp(
          cmd_ids.fixed_channels_supported_id,
          kConnHandle,
          kFixedChannelsSupportedBitSignaling);  // SM not supported

  EXPECT_ACL_PACKET_OUT(test_device(),
                        l2cap::testing::AclFixedChannelsSupportedInfoReq(
                            cmd_ids.fixed_channels_supported_id, kConnHandle),
                        &kFixedChannelsRsp);

  RunUntilIdle();

  // ==== Stage 1: Idle link leads to request sniff mode  =====
  const auto kEnterSniffCmdComplete = bt::testing::CommandCompletePacket(
      hci_spec::kSniffMode, pw::bluetooth::emboss::StatusCode::SUCCESS);

  // Now we run for a bit to generate an idle autosniff trigger
  const auto enter_sniff_cmd =
      StaticByteBuffer(LowerBits(hci_spec::kSniffMode),
                       UpperBits(hci_spec::kSniffMode),
                       0x0a,  // parameter_total_size (10 byte payload)
                       LowerBits(kConnHandle),
                       UpperBits(kConnHandle),
                       // Max interval (816)
                       0x30,
                       0x03,
                       // Min interval (400)
                       0x90,
                       0x01,
                       // sniff attempt (4)
                       0x04,
                       0x00,
                       // Sniff timeout (1)
                       0x01,
                       0x00);

  // ==== Stage 1: Idle link leads to request sniff mode  =====
  // Autosniff code should trigger this command
  EXPECT_CMD_PACKET_OUT(
      test_device(), enter_sniff_cmd, &kEnterSniffCmdComplete);

  // Run for at least the autosniff timeout + a bit.
  RunFor(LogicalLink::kAutosniffTimeout + std::chrono::milliseconds(1));

  // ==== Stage 2: Send the link into sniff mode =====

  // Mock the event to go into sniff mode
  auto into_sniff_event =
      hci::EventPacket::New<pw::bluetooth::emboss::ModeChangeEventWriter>(
          hci_spec::kModeChangeEventCode);
  into_sniff_event.view_t().status().Write(
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  into_sniff_event.view_t().connection_handle().Write(kConnHandle);
  into_sniff_event.view_t().current_mode().Write(
      pw::bluetooth::emboss::AclConnectionMode::SNIFF);

  test_device()->SendCommandChannelPacket(into_sniff_event.data());

  RunUntilIdle();

  // Hopefully now we should be in sniff mode
  ASSERT_EQ(link()->AutosniffMode(),
            pw::bluetooth::emboss::AclConnectionMode::SNIFF);

  bt_log(INFO, "logical_link_test", "Entered sniff mode while idle");

  // Run any pending tasks.
  RunUntilIdle();

  // ==== Stage 3: Suppress sniff mode, which should disable sniff.
  const auto kExitSniffCommandComplete = bt::testing::CommandCompletePacket(
      hci_spec::kExitSniffMode, pw::bluetooth::emboss::StatusCode::SUCCESS);
  // Autosniff code should send the following command
  auto exit_sniff_cmd =
      StaticByteBuffer(LowerBits(hci_spec::kExitSniffMode),
                       UpperBits(hci_spec::kExitSniffMode),
                       0x02,  // parameter_total_size (10 byte payload)
                       LowerBits(kConnHandle),
                       UpperBits(kConnHandle));
  EXPECT_CMD_PACKET_OUT(
      test_device(), exit_sniff_cmd, &kExitSniffCommandComplete);

  auto suppression = link()->SuppressAutosniff("testing");

  RunUntilIdle();

  // ==== Stage 4: Switch link into ACTIVE mode ====

  // Mock the mode change back into active.
  auto into_active_event =
      hci::EventPacket::New<pw::bluetooth::emboss::ModeChangeEventWriter>(
          hci_spec::kModeChangeEventCode);
  into_active_event.view_t().status().Write(
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  into_active_event.view_t().connection_handle().Write(kConnHandle);
  into_active_event.view_t().current_mode().Write(
      pw::bluetooth::emboss::AclConnectionMode::ACTIVE);

  test_device()->SendCommandChannelPacket(into_active_event.data());

  RunUntilIdle();
  // Should be back to active mode
  ASSERT_EQ(link()->AutosniffMode(),
            pw::bluetooth::emboss::AclConnectionMode::ACTIVE);

  auto suppression2 = link()->SuppressAutosniff("second testing");

  // ==== Stage 5: an idle link will NOT cause sniff to happen.
  RunFor(LogicalLink::kAutosniffTimeout + std::chrono::milliseconds(1));

  // ==== Stage 5a: Dropping one interest doesn't re-enable sniff
  suppression.reset();

  // ==== Stage 6: Back into sniff mode after last suppression
  suppression2.reset();

  // Run for at least the autosniff timeout + a bit.
  EXPECT_CMD_PACKET_OUT(
      test_device(), enter_sniff_cmd, &kEnterSniffCmdComplete);
  RunFor(LogicalLink::kAutosniffTimeout + std::chrono::milliseconds(1));
  RunUntilIdle();
}

}  // namespace
}  // namespace bt::l2cap::internal
