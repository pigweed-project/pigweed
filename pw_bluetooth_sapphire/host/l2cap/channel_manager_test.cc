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

#include "pw_bluetooth_sapphire/internal/host/l2cap/channel_manager.h"

#include <pw_assert/check.h>

#include <memory>
#include <type_traits>

#include "pw_bluetooth_sapphire/fake_lease_provider.h"
#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/vendor_protocol.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/channel.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/channel_manager_mock_controller_test_fixture.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/l2cap_defs.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/logical_link.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/test_packets.h"
#include "pw_bluetooth_sapphire/internal/host/testing/gtest_helpers.h"
#include "pw_bluetooth_sapphire/internal/host/testing/inspect.h"
#include "pw_bluetooth_sapphire/internal/host/testing/inspect_util.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_packets.h"
#include "pw_bluetooth_sapphire/internal/host/transport/acl_data_packet.h"
#include "pw_bluetooth_sapphire/internal/host/transport/mock_acl_data_channel.h"

namespace bt::l2cap {
namespace {

namespace android_hci = bt::hci_spec::vendor::android;
namespace android_emb = pw::bluetooth::vendor::android_hci;
using namespace inspect::testing;
using namespace bt::testing;

using LEFixedChannels = ChannelManager::LEFixedChannels;
using BrEdrFixedChannels = ChannelManager::BrEdrFixedChannels;

using AclPriority = pw::bluetooth::AclPriority;

constexpr hci_spec::ConnectionHandle kTestHandle1 = 0x0001;
constexpr hci_spec::ConnectionHandle kTestHandle2 = 0x0002;
constexpr Psm kTestPsm = 0x0001;
constexpr ChannelId kLocalId = 0x0040;
constexpr ChannelId kRemoteId = 0x9042;
constexpr CommandId kPeerConfigRequestId = 153;
constexpr hci::AclDataChannel::PacketPriority kLowPriority =
    hci::AclDataChannel::PacketPriority::kLow;
constexpr hci::AclDataChannel::PacketPriority kHighPriority =
    hci::AclDataChannel::PacketPriority::kHigh;
constexpr ChannelParameters kChannelParams;

constexpr pw::chrono::SystemClock::duration kFlushTimeout =
    std::chrono::milliseconds(10);
constexpr uint16_t kExpectedFlushTimeoutParam =
    16;  // 10ms * kFlushTimeoutMsToCommandParameterConversionFactor(1.6)

// 2x Information Requests: Extended Features, Fixed Channels Supported
constexpr size_t kConnectionCreationPacketCount = 2;

void DoNothing() {}
void NopRxCallback(ByteBufferPtr) {}
void NopLeConnParamCallback(const hci_spec::LEPreferredConnectionParameters&) {}
void NopSecurityCallback(hci_spec::ConnectionHandle,
                         sm::SecurityLevel,
                         sm::ResultFunction<>) {}

// Holds expected outbound data packets including the source location where the
// expectation is set.
struct PacketExpectation {
  const char* file_name;
  int line_number;
  DynamicByteBuffer data;
  bt::LinkType ll_type;
  hci::AclDataChannel::PacketPriority priority;
};

// Helpers to set an outbound packet expectation with the link type and source
// location boilerplate prefilled.
// TODO(fxbug.dev/42075355): Remove packet priorities from expectations
#define EXPECT_LE_PACKET_OUT(packet_buffer, priority) \
  ExpectOutboundPacket(                               \
      bt::LinkType::kLE, (priority), (packet_buffer), __FILE__, __LINE__)

// EXPECT_ACL_PACKET_OUT is already defined by MockController.
#define EXPECT_ACL_PACKET_OUT_(packet_buffer, priority) \
  ExpectOutboundPacket(                                 \
      bt::LinkType::kACL, (priority), (packet_buffer), __FILE__, __LINE__)

auto MakeExtendedFeaturesInformationRequest(CommandId id,
                                            hci_spec::ConnectionHandle handle) {
  return StaticByteBuffer(
      // ACL data header (handle, length: 10)
      LowerBits(handle),
      UpperBits(handle),
      0x0a,
      0x00,
      // L2CAP B-frame header (length: 6, chanel-id: 0x0001 (ACL sig))
      0x06,
      0x00,
      0x01,
      0x00,
      // Extended Features Information Request (ID, length: 2, type)
      0x0a,
      id,
      0x02,
      0x00,
      LowerBits(
          static_cast<uint16_t>(InformationType::kExtendedFeaturesSupported)),
      UpperBits(
          static_cast<uint16_t>(InformationType::kExtendedFeaturesSupported)));
}

auto ConfigurationRequest(
    CommandId id,
    ChannelId dst_id,
    uint16_t mtu = kDefaultMTU,
    std::optional<RetransmissionAndFlowControlMode> mode = std::nullopt,
    uint8_t max_inbound_transmissions = 0) {
  if (mode.has_value()) {
    return DynamicByteBuffer(StaticByteBuffer(
        // ACL data header (handle: 0x0001, length: 27 bytes)
        0x01,
        0x00,
        0x1b,
        0x00,
        // L2CAP B-frame header (length: 23 bytes, channel-id: 0x0001 (ACL sig))
        0x17,
        0x00,
        0x01,
        0x00,
        // Configuration Request (ID, length: 19, dst cid, flags: 0)
        0x04,
        id,
        0x13,
        0x00,
        LowerBits(dst_id),
        UpperBits(dst_id),
        0x00,
        0x00,
        // Mtu option (ID, Length, MTU)
        0x01,
        0x02,
        LowerBits(mtu),
        UpperBits(mtu),
        // Retransmission & Flow Control option (type, length: 9, mode,
        // tx_window: 63, max_retransmit: 0, retransmit timeout: 0 ms, monitor
        // timeout: 0 ms, mps: 65535)
        0x04,
        0x09,
        static_cast<uint8_t>(*mode),
        kErtmMaxUnackedInboundFrames,
        max_inbound_transmissions,
        0x00,
        0x00,
        0x00,
        0x00,
        LowerBits(kMaxInboundPduPayloadSize),
        UpperBits(kMaxInboundPduPayloadSize)));
  }
  return DynamicByteBuffer(StaticByteBuffer(
      // ACL data header (handle: 0x0001, length: 16 bytes)
      0x01,
      0x00,
      0x10,
      0x00,
      // L2CAP B-frame header (length: 12 bytes, channel-id: 0x0001 (ACL sig))
      0x0c,
      0x00,
      0x01,
      0x00,
      // Configuration Request (ID, length: 8, dst cid, flags: 0)
      0x04,
      id,
      0x08,
      0x00,
      LowerBits(dst_id),
      UpperBits(dst_id),
      0x00,
      0x00,
      // Mtu option (ID, Length, MTU)
      0x01,
      0x02,
      LowerBits(mtu),
      UpperBits(mtu)));
}

auto OutboundConnectionResponse(CommandId id) {
  return testing::AclConnectionRsp(id, kTestHandle1, kRemoteId, kLocalId);
}

auto InboundConnectionResponse(CommandId id) {
  return testing::AclConnectionRsp(id, kTestHandle1, kLocalId, kRemoteId);
}

auto InboundConfigurationRequest(
    CommandId id,
    uint16_t mtu = kDefaultMTU,
    std::optional<RetransmissionAndFlowControlMode> mode = std::nullopt,
    uint8_t max_inbound_transmissions = 0) {
  return ConfigurationRequest(
      id, kLocalId, mtu, mode, max_inbound_transmissions);
}

auto InboundConfigurationResponse(CommandId id) {
  return StaticByteBuffer(
      // ACL data header (handle: 0x0001, length: 14 bytes)
      0x01,
      0x00,
      0x0e,
      0x00,
      // L2CAP B-frame header (length: 10 bytes, channel-id: 0x0001 (ACL sig))
      0x0a,
      0x00,
      0x01,
      0x00,
      // Configuration Response (ID: 2, length: 6, src cid, flags: 0, res:
      // success)
      0x05,
      id,
      0x06,
      0x00,
      LowerBits(kLocalId),
      UpperBits(kLocalId),
      0x00,
      0x00,
      0x00,
      0x00);
}

auto InboundConnectionRequest(CommandId id) {
  return StaticByteBuffer(
      // ACL data header (handle: 0x0001, length: 12 bytes)
      0x01,
      0x00,
      0x0c,
      0x00,
      // L2CAP B-frame header (length: 8 bytes, channel-id: 0x0001 (ACL sig))
      0x08,
      0x00,
      0x01,
      0x00,
      // Connection Request (ID, length: 4, psm, src cid)
      0x02,
      id,
      0x04,
      0x00,
      LowerBits(kTestPsm),
      UpperBits(kTestPsm),
      LowerBits(kRemoteId),
      UpperBits(kRemoteId));
}

auto OutboundConnectionRequest(CommandId id) {
  return StaticByteBuffer(
      // ACL data header (handle: 0x0001, length: 12 bytes)
      0x01,
      0x00,
      0x0c,
      0x00,
      // L2CAP B-frame header (length: 8 bytes, channel-id: 0x0001 (ACL sig))
      0x08,
      0x00,
      0x01,
      0x00,
      // Connection Request (ID, length: 4, psm, src cid)
      0x02,
      id,
      0x04,
      0x00,
      LowerBits(kTestPsm),
      UpperBits(kTestPsm),
      LowerBits(kLocalId),
      UpperBits(kLocalId));
}

auto OutboundConfigurationRequest(
    CommandId id,
    uint16_t mtu = kMaxMTU,
    std::optional<RetransmissionAndFlowControlMode> mode = std::nullopt) {
  return ConfigurationRequest(
      id, kRemoteId, mtu, mode, kErtmMaxInboundRetransmissions);
}

// |max_transmissions| is ignored per Core Spec v5.0 Vol 3, Part A, Sec 5.4 but
// still parameterized because this needs to match the value that is sent by our
// L2CAP configuration logic.
auto OutboundConfigurationResponse(
    CommandId id,
    uint16_t mtu = kDefaultMTU,
    std::optional<RetransmissionAndFlowControlMode> mode = std::nullopt,
    uint8_t max_transmissions = 0) {
  const uint8_t kConfigLength = 10 + (mode.has_value() ? 11 : 0);
  const uint16_t kL2capLength = kConfigLength + 4;
  const uint16_t kAclLength = kL2capLength + 4;

  if (mode.has_value()) {
    return DynamicByteBuffer(StaticByteBuffer(
        // ACL data header (handle: 0x0001, length: 14 bytes)
        0x01,
        0x00,
        LowerBits(kAclLength),
        UpperBits(kAclLength),
        // L2CAP B-frame header (length: 10 bytes, channel-id: 0x0001 (ACL sig))
        LowerBits(kL2capLength),
        UpperBits(kL2capLength),
        0x01,
        0x00,
        // Configuration Response (ID, length, src cid, flags: 0, res: success)
        0x05,
        id,
        kConfigLength,
        0x00,
        LowerBits(kRemoteId),
        UpperBits(kRemoteId),
        0x00,
        0x00,
        0x00,
        0x00,
        // MTU option (ID, Length, MTU)
        0x01,
        0x02,
        LowerBits(mtu),
        UpperBits(mtu),
        // Retransmission & Flow Control option (type, length: 9, mode,
        // TxWindow, MaxTransmit, rtx timeout: 2 secs, monitor timeout: 12 secs,
        // mps)
        0x04,
        0x09,
        static_cast<uint8_t>(*mode),
        kErtmMaxUnackedInboundFrames,
        max_transmissions,
        LowerBits(kErtmReceiverReadyPollTimerMsecs),
        UpperBits(kErtmReceiverReadyPollTimerMsecs),
        LowerBits(kErtmMonitorTimerMsecs),
        UpperBits(kErtmMonitorTimerMsecs),
        LowerBits(kMaxInboundPduPayloadSize),
        UpperBits(kMaxInboundPduPayloadSize)));
  }

  return DynamicByteBuffer(StaticByteBuffer(
      // ACL data header (handle: 0x0001, length: 14 bytes)
      0x01,
      0x00,
      LowerBits(kAclLength),
      UpperBits(kAclLength),
      // L2CAP B-frame header (length, channel-id: 0x0001 (ACL sig))
      LowerBits(kL2capLength),
      UpperBits(kL2capLength),
      0x01,
      0x00,
      // Configuration Response (ID, length, src cid, flags: 0, res: success)
      0x05,
      id,
      kConfigLength,
      0x00,
      LowerBits(kRemoteId),
      UpperBits(kRemoteId),
      0x00,
      0x00,
      0x00,
      0x00,
      // MTU option (ID, Length, MTU)
      0x01,
      0x02,
      LowerBits(mtu),
      UpperBits(mtu)));
}

auto OutboundDisconnectionRequest(CommandId id) {
  return StaticByteBuffer(
      // ACL data header (handle: 0x0001, length: 12 bytes)
      0x01,
      0x00,
      0x0c,
      0x00,
      // L2CAP B-frame header (length: 8 bytes, channel-id: 0x0001 (ACL sig))
      0x08,
      0x00,
      0x01,
      0x00,
      // Disconnection Request (ID, length: 4, dst cid, src cid)
      0x06,
      id,
      0x04,
      0x00,
      LowerBits(kRemoteId),
      UpperBits(kRemoteId),
      LowerBits(kLocalId),
      UpperBits(kLocalId));
}

auto OutboundDisconnectionResponse(CommandId id) {
  return StaticByteBuffer(
      // ACL data header (handle: 0x0001, length: 12 bytes)
      0x01,
      0x00,
      0x0c,
      0x00,
      // L2CAP B-frame header (length: 8 bytes, channel-id: 0x0001 (ACL sig))
      0x08,
      0x00,
      0x01,
      0x00,
      // Disconnection Response (ID, length: 4, dst cid, src cid)
      0x07,
      id,
      0x04,
      0x00,
      LowerBits(kLocalId),
      UpperBits(kLocalId),
      LowerBits(kRemoteId),
      UpperBits(kRemoteId));
}

A2dpOffloadManager::Configuration BuildConfiguration(
    android_emb::A2dpCodecType codec = android_emb::A2dpCodecType::SBC) {
  A2dpOffloadManager::Configuration config;
  config.codec = codec;
  config.max_latency = 0xFFFF;
  config.scms_t_enable.view().enabled().Write(
      pw::bluetooth::emboss::GenericEnableParam::DISABLE);
  config.scms_t_enable.view().header().Write(0x00);
  config.sampling_frequency = android_emb::A2dpSamplingFrequency::HZ_44100;
  config.bits_per_sample = android_emb::A2dpBitsPerSample::BITS_PER_SAMPLE_16;
  config.channel_mode = android_emb::A2dpChannelMode::MONO;
  config.encoded_audio_bit_rate = 0x0;

  switch (codec) {
    case android_emb::A2dpCodecType::SBC:
      config.sbc_configuration.view().block_length().Write(
          android_emb::SbcBlockLen::BLOCK_LEN_4);
      config.sbc_configuration.view().subbands().Write(
          android_emb::SbcSubBands::SUBBANDS_4);
      config.sbc_configuration.view().allocation_method().Write(
          android_emb::SbcAllocationMethod::SNR);
      config.sbc_configuration.view().min_bitpool_value().Write(0x00);
      config.sbc_configuration.view().max_bitpool_value().Write(0xFF);
      break;
    case android_emb::A2dpCodecType::AAC:
      config.aac_configuration.view().object_type().Write(0x00);
      config.aac_configuration.view().variable_bit_rate().Write(
          android_emb::AacEnableVariableBitRate::DISABLE);
      break;
    case android_emb::A2dpCodecType::LDAC:
      config.ldac_configuration.view().vendor_id().Write(
          android_hci::kLdacVendorId);
      config.ldac_configuration.view().codec_id().Write(
          android_hci::kLdacCodecId);
      config.ldac_configuration.view().bitrate_index().Write(
          android_emb::LdacBitrateIndex::LOW);
      config.ldac_configuration.view().ldac_channel_mode().stereo().Write(true);
      break;
    case android_emb::A2dpCodecType::APTX:
    case android_emb::A2dpCodecType::APTX_HD:
      break;
  }

  return config;
}

using TestingBase = FakeDispatcherControllerTest<MockController>;

// ChannelManager test fixture that uses MockAclDataChannel to inject inbound
// data and test outbound data. Unexpected outbound packets will cause test
// failures.
class ChannelManagerMockAclChannelTest : public TestingBase {
 public:
  ChannelManagerMockAclChannelTest() = default;
  ~ChannelManagerMockAclChannelTest() override = default;

  void SetUp() override {
    SetUp(hci_spec::kMaxACLPayloadSize, hci_spec::kMaxACLPayloadSize);
  }

  void SetUp(size_t max_acl_payload_size, size_t max_le_payload_size) {
    TestingBase::SetUp();

    acl_data_channel_.set_bredr_buffer_info(
        hci::DataBufferInfo(max_acl_payload_size, /*max_num_packets=*/1));
    acl_data_channel_.set_le_buffer_info(
        hci::DataBufferInfo(max_le_payload_size, /*max_num_packets=*/1));
    acl_data_channel_.set_send_packets_cb(
        fit::bind_member<&ChannelManagerMockAclChannelTest::SendPackets>(this));

    // TODO(fxbug.dev/42141538): Make these tests not depend on strict channel
    // ID ordering.
    chanmgr_ = ChannelManager::Create(&acl_data_channel_,
                                      transport()->command_channel(),
                                      /*random_channel_ids=*/false,
                                      dispatcher(),
                                      lease_provider());

    packet_rx_handler_ = [this](std::unique_ptr<hci::ACLDataPacket> packet) {
      acl_data_channel_.ReceivePacket(std::move(packet));
    };

    next_command_id_ = 1;
  }

  void TearDown() override {
    while (!expected_packets_.empty()) {
      auto& expected = expected_packets_.front();
      ADD_FAILURE_AT(expected.file_name, expected.line_number)
          << "Didn't receive expected outbound " << expected.data.size()
          << "-byte packet";
      expected_packets_.pop();
    }
    chanmgr_ = nullptr;
    packet_rx_handler_ = nullptr;
    TestingBase::TearDown();
  }

  // Helper functions for registering logical links with default arguments.
  [[nodiscard]] ChannelManager::LEFixedChannels RegisterLE(
      hci_spec::ConnectionHandle handle,
      pw::bluetooth::emboss::ConnectionRole role,
      LinkErrorCallback link_error_cb = DoNothing,
      LEConnectionParameterUpdateCallback cpuc = NopLeConnParamCallback,
      SecurityUpgradeCallback suc = NopSecurityCallback) {
    return chanmgr()->AddLEConnection(handle,
                                      role,
                                      std::move(link_error_cb),
                                      std::move(cpuc),
                                      std::move(suc));
  }

  struct QueueRegisterACLRetVal {
    CommandId extended_features_id;
    CommandId fixed_channels_supported_id;
  };

  QueueRegisterACLRetVal QueueRegisterACL(
      hci_spec::ConnectionHandle handle,
      pw::bluetooth::emboss::ConnectionRole role,
      LinkErrorCallback link_error_cb = DoNothing,
      SecurityUpgradeCallback suc = NopSecurityCallback,
      fit::callback<void(ChannelManager::BrEdrFixedChannels)>
          fixed_channels_callback = [](BrEdrFixedChannels) {}) {
    QueueRegisterACLRetVal return_val;
    return_val.extended_features_id = NextCommandId();
    return_val.fixed_channels_supported_id = NextCommandId();

    EXPECT_ACL_PACKET_OUT_(MakeExtendedFeaturesInformationRequest(
                               return_val.extended_features_id, handle),
                           kHighPriority);
    EXPECT_ACL_PACKET_OUT_(testing::AclFixedChannelsSupportedInfoReq(
                               return_val.fixed_channels_supported_id, handle),
                           kHighPriority);
    RegisterACL(handle,
                role,
                std::move(link_error_cb),
                std::move(suc),
                std::move(fixed_channels_callback));
    return return_val;
  }

  void RegisterACL(
      hci_spec::ConnectionHandle handle,
      pw::bluetooth::emboss::ConnectionRole role,
      LinkErrorCallback link_error_cb = DoNothing,
      SecurityUpgradeCallback suc = NopSecurityCallback,
      fit::callback<void(ChannelManager::BrEdrFixedChannels)>
          fixed_channels_callback = [](auto) {}) {
    chanmgr()->AddACLConnection(handle,
                                role,
                                std::move(link_error_cb),
                                std::move(suc),
                                std::move(fixed_channels_callback));
  }

  void ReceiveL2capInformationResponses(
      CommandId extended_features_id,
      CommandId fixed_channels_supported_id,
      l2cap::ExtendedFeatures features = 0u,
      l2cap::FixedChannelsSupported channels = 0u) {
    ReceiveAclDataPacket(testing::AclExtFeaturesInfoRsp(
        extended_features_id, kTestHandle1, features));
    ReceiveAclDataPacket(testing::AclFixedChannelsSupportedInfoRsp(
        fixed_channels_supported_id, kTestHandle1, channels));
  }

  Channel::WeakPtr ActivateNewFixedChannel(
      ChannelId id,
      hci_spec::ConnectionHandle conn_handle = kTestHandle1,
      Channel::ClosedCallback closed_cb = DoNothing,
      Channel::RxCallback rx_cb = NopRxCallback) {
    auto chan = chanmgr()->OpenFixedChannel(conn_handle, id);
    if (!chan.is_alive() ||
        !chan->Activate(std::move(rx_cb), std::move(closed_cb))) {
      return Channel::WeakPtr();
    }

    return chan;
  }

  // |activated_cb| will be called with opened and activated Channel if
  // successful and nullptr otherwise.
  void ActivateOutboundChannel(
      Psm psm,
      ChannelParameters chan_params,
      ChannelCallback activated_cb,
      hci_spec::ConnectionHandle conn_handle = kTestHandle1,
      Channel::ClosedCallback closed_cb = DoNothing,
      Channel::RxCallback rx_cb = NopRxCallback) {
    ChannelCallback open_cb = [activated_cb = std::move(activated_cb),
                               rx_cb = std::move(rx_cb),
                               closed_cb =
                                   std::move(closed_cb)](auto chan) mutable {
      if (!chan.is_alive() ||
          !chan->Activate(std::move(rx_cb), std::move(closed_cb))) {
        activated_cb(Channel::WeakPtr());
      } else {
        activated_cb(std::move(chan));
      }
    };
    chanmgr()->OpenL2capChannel(
        conn_handle, psm, chan_params, std::move(open_cb));
  }

  void SetUpOutboundChannelWithCallback(
      ChannelId local_id,
      ChannelId remote_id,
      Channel::ClosedCallback closed_cb,
      ChannelParameters channel_params,
      fit::function<void(Channel::WeakPtr)> channel_cb) {
    const auto conn_req_id = NextCommandId();
    const auto config_req_id = NextCommandId();
    EXPECT_ACL_PACKET_OUT_(testing::AclConnectionReq(
                               conn_req_id, kTestHandle1, local_id, kTestPsm),
                           kHighPriority);
    EXPECT_ACL_PACKET_OUT_(
        testing::AclConfigReq(
            config_req_id, kTestHandle1, remote_id, kChannelParams),
        kHighPriority);
    EXPECT_ACL_PACKET_OUT_(
        testing::AclConfigRsp(
            kPeerConfigRequestId, kTestHandle1, remote_id, kChannelParams),
        kHighPriority);

    ActivateOutboundChannel(kTestPsm,
                            channel_params,
                            std::move(channel_cb),
                            kTestHandle1,
                            std::move(closed_cb));
    RunUntilIdle();

    ReceiveAclDataPacket(testing::AclConnectionRsp(
        conn_req_id, kTestHandle1, local_id, remote_id));
    ReceiveAclDataPacket(testing::AclConfigReq(
        kPeerConfigRequestId, kTestHandle1, local_id, kChannelParams));
    ReceiveAclDataPacket(testing::AclConfigRsp(
        config_req_id, kTestHandle1, local_id, kChannelParams));

    RunUntilIdle();
    EXPECT_TRUE(AllExpectedPacketsSent());
  }

  Channel::WeakPtr SetUpOutboundChannel(
      ChannelId local_id = kLocalId,
      ChannelId remote_id = kRemoteId,
      Channel::ClosedCallback closed_cb = DoNothing,
      ChannelParameters channel_params = kChannelParams) {
    Channel::WeakPtr channel;
    auto channel_cb = [&channel](l2cap::Channel::WeakPtr activated_chan) {
      channel = std::move(activated_chan);
    };

    SetUpOutboundChannelWithCallback(
        local_id, remote_id, std::move(closed_cb), channel_params, channel_cb);
    EXPECT_TRUE(channel.is_alive());
    return channel;
  }

  // Set an expectation for an outbound ACL data packet. Packets are expected in
  // the order that they're added. The test fails if not all expected packets
  // have been set when the test case completes or if the outbound data doesn't
  // match expectations, including the ordering between LE and ACL packets.
  void ExpectOutboundPacket(bt::LinkType ll_type,
                            hci::AclDataChannel::PacketPriority priority,
                            const ByteBuffer& data,
                            const char* file_name = "",
                            int line_number = 0) {
    expected_packets_.push(
        {file_name, line_number, DynamicByteBuffer(data), ll_type, priority});
  }

  void OpenOutboundErtmChannel(
      ChannelCallback open_cb,
      hci_spec::ConnectionHandle conn_handle = kTestHandle1,
      uint8_t max_outbound_transmit = 3) {
    l2cap::ChannelParameters chan_params;
    auto config_mode =
        l2cap::RetransmissionAndFlowControlMode::kEnhancedRetransmission;
    chan_params.mode = config_mode;

    const auto conn_req_id = NextCommandId();
    const auto config_req_id = NextCommandId();
    EXPECT_ACL_PACKET_OUT_(OutboundConnectionRequest(conn_req_id),
                           kHighPriority);
    EXPECT_ACL_PACKET_OUT_(
        OutboundConfigurationRequest(
            config_req_id, kMaxInboundPduPayloadSize, config_mode),
        kHighPriority);
    const auto kInboundMtu = kDefaultMTU;
    EXPECT_ACL_PACKET_OUT_(OutboundConfigurationResponse(kPeerConfigRequestId,
                                                         kInboundMtu,
                                                         config_mode,
                                                         max_outbound_transmit),
                           kHighPriority);

    chanmgr()->OpenL2capChannel(
        conn_handle, kTestPsm, chan_params, std::move(open_cb));

    ReceiveAclDataPacket(InboundConnectionResponse(conn_req_id));
    ReceiveAclDataPacket(InboundConfigurationRequest(
        kPeerConfigRequestId, kInboundMtu, config_mode, max_outbound_transmit));
    ReceiveAclDataPacket(InboundConfigurationResponse(config_req_id));
  }

  void ActivateOutboundErtmChannel(
      ChannelCallback activated_cb,
      Channel::ClosedCallback closed_cb = DoNothing,
      Channel::RxCallback rx_cb = NopRxCallback) {
    OpenOutboundErtmChannel(
        [activated_cb = std::move(activated_cb),
         rx_cb = std::move(rx_cb),
         closed_cb = std::move(closed_cb)](auto chan) mutable {
          if (!chan.is_alive() ||
              !chan->Activate(std::move(rx_cb), std::move(closed_cb))) {
            activated_cb(Channel::WeakPtr());
          } else {
            activated_cb(std::move(chan));
          }
        });
  }

  void OpenInboundErtmChannel(ChannelCallback channel_cb) {
    CommandId kPeerConnReqId = 3;

    l2cap::ChannelParameters chan_params;
    auto config_mode =
        l2cap::RetransmissionAndFlowControlMode::kEnhancedRetransmission;
    chan_params.mode = config_mode;
    chan_params.max_rx_sdu_size = l2cap::kMinACLMTU;

    const auto cmd_ids = QueueRegisterACL(
        kTestHandle1, pw::bluetooth::emboss::ConnectionRole::CENTRAL);
    ReceiveAclDataPacket(testing::AclExtFeaturesInfoRsp(
        cmd_ids.extended_features_id,
        kTestHandle1,
        kExtendedFeaturesBitEnhancedRetransmission));
    ReceiveAclDataPacket(testing::AclFixedChannelsSupportedInfoRsp(
        cmd_ids.fixed_channels_supported_id,
        kTestHandle1,
        kFixedChannelsSupportedBitSignaling));
    EXPECT_TRUE(chanmgr()->RegisterService(
        kTestPsm, chan_params, std::move(channel_cb)));

    const auto config_req_id = NextCommandId();
    EXPECT_ACL_PACKET_OUT_(OutboundConnectionResponse(kPeerConnReqId),
                           kHighPriority);
    EXPECT_ACL_PACKET_OUT_(
        OutboundConfigurationRequest(
            config_req_id, *chan_params.max_rx_sdu_size, config_mode),
        kHighPriority);
    const auto kInboundMtu = kDefaultMTU;
    EXPECT_ACL_PACKET_OUT_(OutboundConfigurationResponse(
                               kPeerConfigRequestId, kInboundMtu, config_mode),
                           kHighPriority);

    ReceiveAclDataPacket(InboundConnectionRequest(kPeerConnReqId));
    ReceiveAclDataPacket(InboundConfigurationRequest(
        kPeerConfigRequestId, kInboundMtu, config_mode));
    ReceiveAclDataPacket(InboundConfigurationResponse(config_req_id));

    RunUntilIdle();
    EXPECT_TRUE(AllExpectedPacketsSent());
  }

  // Returns true if all expected outbound packets up to this call have been
  // sent by the test case.
  [[nodiscard]] bool AllExpectedPacketsSent() const {
    return expected_packets_.empty();
  }

  void ReceiveAclDataPacket(const ByteBuffer& packet) {
    const size_t payload_size = packet.size() - sizeof(hci_spec::ACLDataHeader);
    PW_CHECK(payload_size <= std::numeric_limits<uint16_t>::max());
    hci::ACLDataPacketPtr acl_packet =
        hci::ACLDataPacket::New(static_cast<uint16_t>(payload_size));
    auto mutable_acl_packet_data = acl_packet->mutable_view()->mutable_data();
    packet.Copy(&mutable_acl_packet_data);
    packet_rx_handler_(std::move(acl_packet));
  }

  ChannelManager* chanmgr() const { return chanmgr_.get(); }

  hci::testing::MockAclDataChannel* acl_data_channel() {
    return &acl_data_channel_;
  }

  CommandId NextCommandId() { return next_command_id_++; }

 private:
  bool SendPackets(std::list<hci::ACLDataPacketPtr> packets) {
    for (auto& packet : packets) {
      const ByteBuffer& data = packet->view().data();
      if (expected_packets_.empty()) {
        ADD_FAILURE() << "Unexpected outbound ACL data";
        std::cout << "{ ";
        PrintByteContainer(data);
        std::cout << " }\n";
      } else {
        const auto& expected = expected_packets_.front();
        // Prints both data in case of mismatch.
        if (!ContainersEqual(expected.data, data)) {
          ADD_FAILURE_AT(expected.file_name, expected.line_number)
              << "Outbound ACL data doesn't match expected";
        }
        expected_packets_.pop();
      }
    }
    return !packets.empty();
  }

  std::unique_ptr<ChannelManager> chanmgr_;
  hci::ACLPacketHandler packet_rx_handler_;
  hci::testing::MockAclDataChannel acl_data_channel_;

  std::queue<PacketExpectation> expected_packets_;

  CommandId next_command_id_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(ChannelManagerMockAclChannelTest);
};

// ChannelManager test fixture that uses a real AclDataChannel and uses
// MockController for HCI packet expectations
using ChannelManagerRealAclChannelTest =
    FakeDispatcherChannelManagerMockControllerTest;

TEST_F(ChannelManagerRealAclChannelTest, OpenFixedChannelErrorNoConn) {
  // This should fail as the ChannelManager has no entry for |kTestHandle1|.
  EXPECT_FALSE(ActivateNewFixedChannel(kATTChannelId).is_alive());

  QueueLEConnection(kTestHandle1,
                    pw::bluetooth::emboss::ConnectionRole::CENTRAL);

  // This should fail as the ChannelManager has no entry for |kTestHandle2|.
  EXPECT_FALSE(ActivateNewFixedChannel(kATTChannelId, kTestHandle2).is_alive());
}

TEST_F(ChannelManagerRealAclChannelTest, OpenFixedChannelErrorDisallowedId) {
  // LE-U link
  QueueLEConnection(kTestHandle1,
                    pw::bluetooth::emboss::ConnectionRole::CENTRAL);

  // ACL-U link
  QueueAclConnection(kTestHandle2);
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  // This should fail as kSMPChannelId is ACL-U only.
  EXPECT_FALSE(ActivateNewFixedChannel(kSMPChannelId, kTestHandle1).is_alive());

  // This should fail as kATTChannelId is LE-U only.
  EXPECT_FALSE(ActivateNewFixedChannel(kATTChannelId, kTestHandle2).is_alive());
}

TEST_F(ChannelManagerRealAclChannelTest,
       DeactivateDynamicChannelInvalidatesChannelPointer) {
  QueueAclConnection(kTestHandle1);
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  Channel::WeakPtr channel;
  auto chan_cb = [&](l2cap::Channel::WeakPtr activated_chan) {
    EXPECT_EQ(kTestHandle1, activated_chan->link_handle());
    channel = std::move(activated_chan);
  };
  QueueOutboundL2capConnection(
      kTestHandle1, kTestPsm, kLocalId, kRemoteId, std::move(chan_cb));

  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
  EXPECT_TRUE(channel.is_alive());
  ASSERT_TRUE(channel->Activate(NopRxCallback, DoNothing));

  EXPECT_ACL_PACKET_OUT(
      test_device(),
      l2cap::testing::AclDisconnectionReq(
          NextCommandId(), kTestHandle1, kLocalId, kRemoteId));

  channel->Deactivate();
  RunUntilIdle();
  ASSERT_FALSE(channel.is_alive());
}

TEST_F(ChannelManagerRealAclChannelTest,
       DeactivateAttChannelInvalidatesChannelPointer) {
  LEFixedChannels fixed_channels = QueueLEConnection(
      kTestHandle1, pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  ASSERT_TRUE(fixed_channels.att->Activate(NopRxCallback, DoNothing));
  fixed_channels.att->Deactivate();
  ASSERT_FALSE(fixed_channels.att.is_alive());
}

TEST_F(ChannelManagerRealAclChannelTest, OpenFixedChannelAndUnregisterLink) {
  // LE-U link
  LEFixedChannels fixed_channels = QueueLEConnection(
      kTestHandle1, pw::bluetooth::emboss::ConnectionRole::CENTRAL);

  bool closed_called = false;
  auto closed_cb = [&closed_called] { closed_called = true; };

  ASSERT_TRUE(fixed_channels.att->Activate(NopRxCallback, closed_cb));
  EXPECT_EQ(kTestHandle1, fixed_channels.att->link_handle());

  // This should notify the channel.
  chanmgr()->RemoveConnection(kTestHandle1);

  RunUntilIdle();

  // |closed_cb| will be called synchronously since it was registered using the
  // current thread's task runner.
  EXPECT_TRUE(closed_called);
}

TEST_F(ChannelManagerRealAclChannelTest, OpenFixedChannelAndCloseChannel) {
  // LE-U link
  LEFixedChannels fixed_channels = QueueLEConnection(
      kTestHandle1, pw::bluetooth::emboss::ConnectionRole::CENTRAL);

  bool closed_called = false;
  auto closed_cb = [&closed_called] { closed_called = true; };

  ASSERT_TRUE(fixed_channels.att->Activate(NopRxCallback, closed_cb));

  // Close the channel before unregistering the link. |closed_cb| should not get
  // called.
  fixed_channels.att->Deactivate();
  chanmgr()->RemoveConnection(kTestHandle1);

  RunUntilIdle();

  EXPECT_FALSE(closed_called);
}

TEST_F(ChannelManagerRealAclChannelTest, FixedChannelsUseBasicMode) {
  LEFixedChannels fixed_channels = QueueLEConnection(
      kTestHandle1, pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  ASSERT_TRUE(fixed_channels.att->Activate(NopRxCallback, DoNothing));
  EXPECT_EQ(RetransmissionAndFlowControlMode::kBasic,
            fixed_channels.att->mode());
}

TEST_F(ChannelManagerRealAclChannelTest,
       OpenAndCloseWithLinkMultipleFixedChannels) {
  // LE-U link
  LEFixedChannels fixed_channels = QueueLEConnection(
      kTestHandle1, pw::bluetooth::emboss::ConnectionRole::CENTRAL);

  bool att_closed = false;
  auto att_closed_cb = [&att_closed] { att_closed = true; };
  ASSERT_TRUE(fixed_channels.att->Activate(NopRxCallback, att_closed_cb));

  bool smp_closed = false;
  auto smp_closed_cb = [&smp_closed] { smp_closed = true; };
  ASSERT_TRUE(fixed_channels.smp->Activate(NopRxCallback, smp_closed_cb));

  fixed_channels.smp->Deactivate();
  chanmgr()->RemoveConnection(kTestHandle1);

  RunUntilIdle();

  EXPECT_TRUE(att_closed);
  EXPECT_FALSE(smp_closed);
}

TEST_F(ChannelManagerRealAclChannelTest,
       SendingPacketsBeforeRemoveConnectionAndVerifyChannelClosed) {
  // LE-U link
  LEFixedChannels fixed_channels = QueueLEConnection(
      kTestHandle1, pw::bluetooth::emboss::ConnectionRole::CENTRAL);

  bool closed_called = false;
  auto closed_cb = [&closed_called] { closed_called = true; };
  auto chan = fixed_channels.att;
  ASSERT_TRUE(chan.is_alive());
  ASSERT_TRUE(chan->Activate(NopRxCallback, closed_cb));

  EXPECT_ACL_PACKET_OUT(test_device(),
                        StaticByteBuffer(
                            // ACL data header (handle: 1, length: 6)
                            0x01,
                            0x00,
                            0x06,
                            0x00,
                            // L2CAP B-frame (length: 2, channel-id: ATT)
                            0x02,
                            0x00,
                            LowerBits(kATTChannelId),
                            UpperBits(kATTChannelId),
                            'h',
                            'i'));

  EXPECT_TRUE(chan->Send(NewBuffer('h', 'i')));
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  chanmgr()->RemoveConnection(kTestHandle1);

  // The L2CAP channel should have been notified of closure immediately.
  EXPECT_TRUE(closed_called);
  EXPECT_FALSE(chan.is_alive());
  RunUntilIdle();
}

// Tests that destroying the ChannelManager cleanly shuts down all channels.
TEST_F(ChannelManagerRealAclChannelTest,
       DestroyingChannelManagerCleansUpChannels) {
  // LE-U link
  LEFixedChannels fixed_channels = QueueLEConnection(
      kTestHandle1, pw::bluetooth::emboss::ConnectionRole::CENTRAL);

  bool closed_called = false;
  auto closed_cb = [&closed_called] { closed_called = true; };
  auto chan = fixed_channels.att;
  ASSERT_TRUE(chan.is_alive());
  ASSERT_TRUE(chan->Activate(NopRxCallback, closed_cb));

  EXPECT_ACL_PACKET_OUT(test_device(),
                        StaticByteBuffer(
                            // ACL data header (handle: 1, length: 6)
                            0x01,
                            0x00,
                            0x06,
                            0x00,
                            // L2CAP B-frame (length: 2, channel-id: ATT)
                            0x02,
                            0x00,
                            LowerBits(kATTChannelId),
                            UpperBits(kATTChannelId),
                            'h',
                            'i'));

  // Send a packet. This should be processed immediately.
  EXPECT_TRUE(chan->Send(NewBuffer('h', 'i')));
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  TearDown();

  EXPECT_TRUE(closed_called);
  EXPECT_FALSE(chan.is_alive());
  // No outbound packet expectations were set, so this test will fail if it
  // sends any data.
  RunUntilIdle();
}

TEST_F(ChannelManagerRealAclChannelTest, DeactivateDoesNotCrashOrHang) {
  // Tests that the clean up task posted to the LogicalLink does not crash when
  // a dynamic registry is not present (which is the case for LE links).
  LEFixedChannels fixed_channels = QueueLEConnection(
      kTestHandle1, pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  ASSERT_TRUE(fixed_channels.att.is_alive());
  ASSERT_TRUE(fixed_channels.att->Activate(NopRxCallback, DoNothing));
  fixed_channels.att->Deactivate();

  // Loop until the clean up task runs.
  RunUntilIdle();
}

TEST_F(ChannelManagerRealAclChannelTest,
       CallingDeactivateFromClosedCallbackDoesNotCrashOrHang) {
  std::optional<BrEdrFixedChannels> fixed_channels;
  QueueAclConnection(kTestHandle1,
                     pw::bluetooth::emboss::ConnectionRole::CENTRAL,
                     [&fixed_channels](BrEdrFixedChannels channels) {
                       fixed_channels = channels;
                     });
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
  ASSERT_TRUE(fixed_channels.has_value());

  auto chan = std::move(fixed_channels->smp);
  ASSERT_TRUE(chan.is_alive());
  chan->Activate(NopRxCallback, [chan] { chan->Deactivate(); });
  chanmgr()->RemoveConnection(kTestHandle1);  // Triggers ClosedCallback.
  RunUntilIdle();
}

TEST_F(ChannelManagerMockAclChannelTest, ReceiveData) {
  // LE-U link
  LEFixedChannels fixed_channels =
      RegisterLE(kTestHandle1, pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  ASSERT_TRUE(fixed_channels.att.is_alive());
  ASSERT_TRUE(fixed_channels.smp.is_alive());

  // We use the ATT channel to control incoming packets and the SMP channel to
  // quit the message loop
  std::vector<std::string> sdus;
  auto att_rx_cb = [&sdus](ByteBufferPtr sdu) {
    PW_DCHECK(sdu);
    sdus.push_back(sdu->ToString());
  };

  bool smp_cb_called = false;
  auto smp_rx_cb = [&smp_cb_called](ByteBufferPtr sdu) {
    PW_DCHECK(sdu);
    EXPECT_EQ(0u, sdu->size());
    smp_cb_called = true;
  };

  ASSERT_TRUE(fixed_channels.att->Activate(att_rx_cb, DoNothing));
  ASSERT_TRUE(fixed_channels.smp->Activate(smp_rx_cb, DoNothing));

  // ATT channel
  ReceiveAclDataPacket(StaticByteBuffer(
      // ACL data header (starting fragment)
      0x01,
      0x00,
      0x09,
      0x00,
      // L2CAP B-frame
      0x05,
      0x00,
      0x04,
      0x00,
      'h',
      'e',
      'l',
      'l',
      'o'));

  ReceiveAclDataPacket(StaticByteBuffer(
      // ACL data header (starting fragment)
      0x01,
      0x00,
      0x09,
      0x00,
      // L2CAP B-frame (partial)
      0x0C,
      0x00,
      0x04,
      0x00,
      'h',
      'o',
      'w',
      ' ',
      'a'));

  ReceiveAclDataPacket(StaticByteBuffer(
      // ACL data header (continuing fragment)
      0x01,
      0x10,
      0x07,
      0x00,
      // L2CAP B-frame (partial)
      'r',
      'e',
      ' ',
      'y',
      'o',
      'u',
      '?'));

  // SMP channel
  ReceiveAclDataPacket(StaticByteBuffer(
      // ACL data header (starting fragment)
      0x01,
      0x00,
      0x04,
      0x00,
      // L2CAP B-frame (empty)
      0x00,
      0x00,
      0x06,
      0x00));

  RunUntilIdle();

  EXPECT_TRUE(smp_cb_called);
  ASSERT_EQ(2u, sdus.size());
  EXPECT_EQ("hello", sdus[0]);
  EXPECT_EQ("how are you?", sdus[1]);
}

TEST_F(ChannelManagerMockAclChannelTest, ReceiveDataBeforeRegisteringLink) {
  constexpr size_t kPacketCount = 10;

  StaticByteBuffer<255> buffer;

  // We use the ATT channel to control incoming packets and the SMP channel to
  // quit the message loop
  size_t packet_count = 0;
  auto att_rx_cb = [&packet_count](ByteBufferPtr) { packet_count++; };

  bool smp_cb_called = false;
  auto smp_rx_cb = [&smp_cb_called](ByteBufferPtr sdu) {
    PW_DCHECK(sdu);
    EXPECT_EQ(0u, sdu->size());
    smp_cb_called = true;
  };

  // ATT channel
  for (size_t i = 0u; i < kPacketCount; i++) {
    ReceiveAclDataPacket(StaticByteBuffer(
        // ACL data header (starting fragment)
        0x01,
        0x00,
        0x04,
        0x00,
        // L2CAP B-frame
        0x00,
        0x00,
        0x04,
        0x00));
  }

  // SMP channel
  ReceiveAclDataPacket(StaticByteBuffer(
      // ACL data header (starting fragment)
      0x01,
      0x00,
      0x04,
      0x00,
      // L2CAP B-frame (empty)
      0x00,
      0x00,
      0x06,
      0x00));

  Channel::WeakPtr att_chan, smp_chan;

  // Run the loop so all packets are received.
  RunUntilIdle();

  LEFixedChannels fixed_channels =
      RegisterLE(kTestHandle1, pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  ASSERT_TRUE(fixed_channels.att->Activate(att_rx_cb, DoNothing));
  ASSERT_TRUE(fixed_channels.smp->Activate(smp_rx_cb, DoNothing));

  RunUntilIdle();
  EXPECT_TRUE(smp_cb_called);
  EXPECT_EQ(kPacketCount, packet_count);
}

// Receive data after registering the link but before creating a fixed channel.
TEST_F(ChannelManagerRealAclChannelTest,
       ReceiveDataBeforeCreatingFixedChannel) {
  constexpr size_t kPacketCount = 10;

  // Register an ACL connection because LE connections create fixed channels
  // immediately.
  QueueAclConnection(kTestHandle1,
                     pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  RunUntilIdle();

  StaticByteBuffer<255> buffer;

  size_t packet_count = 0;
  auto rx_cb = [&packet_count](ByteBufferPtr) { packet_count++; };
  for (size_t i = 0u; i < kPacketCount; i++) {
    test_device()->SendACLDataChannelPacket(StaticByteBuffer(
        // ACL data header (starting fragment)
        LowerBits(kTestHandle1),
        UpperBits(kTestHandle1),
        0x04,
        0x00,
        // L2CAP B-frame (empty)
        0x00,
        0x00,
        LowerBits(kConnectionlessChannelId),
        UpperBits(kConnectionlessChannelId)));
  }
  // Run the loop so all packets are received.
  RunUntilIdle();

  auto chan = ActivateNewFixedChannel(
      kConnectionlessChannelId, kTestHandle1, DoNothing, std::move(rx_cb));

  RunUntilIdle();
  EXPECT_EQ(kPacketCount, packet_count);
}

// Receive data after registering the link and creating the channel but before
// setting the rx handler.
TEST_F(ChannelManagerRealAclChannelTest, ReceiveDataBeforeSettingRxHandler) {
  constexpr size_t kPacketCount = 10;

  LEFixedChannels fixed_channels = QueueLEConnection(
      kTestHandle1, pw::bluetooth::emboss::ConnectionRole::CENTRAL);

  StaticByteBuffer<255> buffer;

  // We use the ATT channel to control incoming packets and the SMP channel to
  // quit the message loop
  size_t packet_count = 0;
  auto att_rx_cb = [&packet_count](ByteBufferPtr) { packet_count++; };

  bool smp_cb_called = false;
  auto smp_rx_cb = [&smp_cb_called](ByteBufferPtr sdu) {
    PW_DCHECK(sdu);
    EXPECT_EQ(0u, sdu->size());
    smp_cb_called = true;
  };

  // ATT channel
  for (size_t i = 0u; i < kPacketCount; i++) {
    test_device()->SendACLDataChannelPacket(StaticByteBuffer(
        // ACL data header (starting fragment)
        0x01,
        0x00,
        0x04,
        0x00,
        // L2CAP B-frame
        0x00,
        0x00,
        LowerBits(kATTChannelId),
        UpperBits(kATTChannelId)));
  }

  // SMP channel
  test_device()->SendACLDataChannelPacket(StaticByteBuffer(
      // ACL data header (starting fragment)
      0x01,
      0x00,
      0x04,
      0x00,
      // L2CAP B-frame (empty)
      0x00,
      0x00,
      LowerBits(kLESMPChannelId),
      UpperBits(kLESMPChannelId)));

  // Run the loop so all packets are received.
  RunUntilIdle();

  fixed_channels.att->Activate(att_rx_cb, DoNothing);
  fixed_channels.smp->Activate(smp_rx_cb, DoNothing);

  RunUntilIdle();

  EXPECT_TRUE(smp_cb_called);
  EXPECT_EQ(kPacketCount, packet_count);
}

TEST_F(ChannelManagerMockAclChannelTest,
       ActivateChannelProcessesCallbacksSynchronously) {
  // LE-U link
  LEFixedChannels fixed_channels =
      RegisterLE(kTestHandle1, pw::bluetooth::emboss::ConnectionRole::CENTRAL);

  int att_rx_cb_count = 0;
  int smp_rx_cb_count = 0;

  auto att_rx_cb = [&att_rx_cb_count](ByteBufferPtr sdu) {
    EXPECT_EQ("hello", sdu->AsString());
    att_rx_cb_count++;
  };
  bool att_closed_called = false;
  auto att_closed_cb = [&att_closed_called] { att_closed_called = true; };

  ASSERT_TRUE(fixed_channels.att->Activate(std::move(att_rx_cb),
                                           std::move(att_closed_cb)));

  auto smp_rx_cb = [&smp_rx_cb_count](ByteBufferPtr sdu) {
    EXPECT_EQ("🤨", sdu->AsString());
    smp_rx_cb_count++;
  };
  bool smp_closed_called = false;
  auto smp_closed_cb = [&smp_closed_called] { smp_closed_called = true; };

  ASSERT_TRUE(fixed_channels.smp->Activate(std::move(smp_rx_cb),
                                           std::move(smp_closed_cb)));

  ReceiveAclDataPacket(StaticByteBuffer(
      // ACL data header (starting fragment)
      0x01,
      0x00,
      0x08,
      0x00,
      // L2CAP B-frame for SMP fixed channel (4-byte payload: U+1F928 in UTF-8)
      0x04,
      0x00,
      0x06,
      0x00,
      0xf0,
      0x9f,
      0xa4,
      0xa8));

  ReceiveAclDataPacket(StaticByteBuffer(
      // ACL data header (starting fragment)
      0x01,
      0x00,
      0x09,
      0x00,
      // L2CAP B-frame for ATT fixed channel
      0x05,
      0x00,
      0x04,
      0x00,
      'h',
      'e',
      'l',
      'l',
      'o'));

  // Receiving data in ChannelManager processes the ATT and SMP packets
  // synchronously so it has already routed the data to the Channels.
  EXPECT_EQ(1, att_rx_cb_count);
  EXPECT_EQ(1, smp_rx_cb_count);
  RunUntilIdle();
  EXPECT_EQ(1, att_rx_cb_count);
  EXPECT_EQ(1, smp_rx_cb_count);

  // Link closure synchronously calls the ATT and SMP channel close callbacks.
  chanmgr()->RemoveConnection(kTestHandle1);
  EXPECT_TRUE(att_closed_called);
  EXPECT_TRUE(smp_closed_called);
}

TEST_F(ChannelManagerRealAclChannelTest,
       RemovingLinkInvalidatesChannelPointer) {
  LEFixedChannels fixed_channels = QueueLEConnection(
      kTestHandle1, pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  PW_CHECK(fixed_channels.att->Activate(NopRxCallback, DoNothing));
  chanmgr()->RemoveConnection(kTestHandle1);
  EXPECT_FALSE(fixed_channels.att.is_alive());
}

TEST_F(ChannelManagerRealAclChannelTest, SendBasicSDU) {
  LEFixedChannels fixed_channels = QueueLEConnection(
      kTestHandle1, pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  PW_CHECK(fixed_channels.att->Activate(NopRxCallback, DoNothing));

  EXPECT_ACL_PACKET_OUT(test_device(),
                        StaticByteBuffer(
                            // ACL data header (handle: 1, length 7)
                            0x01,
                            0x00,
                            0x08,
                            0x00,
                            // L2CAP B-frame: (length: 3, channel-id: 4)
                            0x04,
                            0x00,
                            0x04,
                            0x00,
                            'T',
                            'e',
                            's',
                            't'));

  EXPECT_TRUE(fixed_channels.att->Send(NewBuffer('T', 'e', 's', 't')));
  RunUntilIdle();
}

// Tests that fragmentation of BR/EDR packets uses the BR/EDR buffer size
TEST_F(ChannelManagerRealAclChannelTest, SendBREDRFragmentedSDUs) {
  // Customize setup so that ACL payload size is 6 instead of default 1024
  TearDown();
  SetUp(/*max_acl_payload_size=*/6, /*max_le_payload_size=*/0);

  // Send fragmented Extended Features Information Request
  l2cap::CommandId features_command_id = NextCommandId();
  EXPECT_ACL_PACKET_OUT(test_device(),
                        StaticByteBuffer(
                            // ACL data header (handle: 1, length: 6)
                            0x01,
                            0x00,
                            0x06,
                            0x00,
                            // L2CAP B-frame (length: 6, channel-id: 1)
                            0x06,
                            0x00,
                            0x01,
                            0x00,
                            // Extended Features Information Request
                            // (code = 0x0A, ID)
                            0x0A,
                            features_command_id));
  EXPECT_ACL_PACKET_OUT(
      test_device(),
      StaticByteBuffer(
          // ACL data header (handle: 1, pbf: continuing fr., length: 4)
          0x01,
          0x10,
          0x04,
          0x00,
          // Extended Features Information Request cont. (Length: 2, type)
          0x02,
          0x00,
          LowerBits(static_cast<uint16_t>(
              InformationType::kExtendedFeaturesSupported)),
          UpperBits(static_cast<uint16_t>(
              InformationType::kExtendedFeaturesSupported))));

  // Send fragmented Fixed Channels Supported Information Request
  l2cap::CommandId fixed_channels_command_id = NextCommandId();
  EXPECT_ACL_PACKET_OUT(test_device(),
                        StaticByteBuffer(
                            // ACL data header (handle: 1, length: 6)
                            0x01,
                            0x00,
                            0x06,
                            0x00,
                            // L2CAP B-frame (length: 6, channel-id: 1)
                            0x06,
                            0x00,
                            0x01,
                            0x00,
                            // Fixed Channels Supported Information Request
                            // (command code, command ID)
                            l2cap::kInformationRequest,
                            fixed_channels_command_id));
  auto fixed_channels_rsp = testing::AclFixedChannelsSupportedInfoRsp(
      fixed_channels_command_id,
      kTestHandle1,
      kFixedChannelsSupportedBitSignaling | kFixedChannelsSupportedBitSM);
  EXPECT_ACL_PACKET_OUT(
      test_device(),
      StaticByteBuffer(
          // ACL data header (handle: 1, pbf: continuing fr., length: 4)
          0x01,
          0x10,
          0x04,
          0x00,
          // Fixed Channels Supported Information Request cont.
          // (length: 2, type)
          0x02,
          0x00,
          LowerBits(
              static_cast<uint16_t>(InformationType::kFixedChannelsSupported)),
          UpperBits(
              static_cast<uint16_t>(InformationType::kFixedChannelsSupported))),
      &fixed_channels_rsp);
  std::optional<BrEdrFixedChannels> fixed_channels;
  chanmgr()->AddACLConnection(
      kTestHandle1,
      pw::bluetooth::emboss::ConnectionRole::CENTRAL,
      /*link_error_callback=*/[]() {},
      /*security_callback=*/[](auto, auto, auto) {},
      /*fixed_channels_callback=*/
      [&fixed_channels](BrEdrFixedChannels channels) {
        fixed_channels = channels;
      });
  RunUntilIdle();
  ASSERT_TRUE(fixed_channels.has_value());
  Channel::WeakPtr sm_chan = std::move(fixed_channels->smp);
  sm_chan->Activate(NopRxCallback, DoNothing);
  ASSERT_TRUE(sm_chan.is_alive());

  EXPECT_ACL_PACKET_OUT(
      test_device(),
      StaticByteBuffer(
          // ACL data header (handle: 1, length: 6)
          0x01,
          0x00,
          0x06,
          0x00,
          // L2CAP B-frame: (length: 7, channel-id: 7 (SMP), partial payload)
          0x07,
          0x00,
          LowerBits(kSMPChannelId),
          UpperBits(kSMPChannelId),
          'G',
          'o'));

  EXPECT_ACL_PACKET_OUT(
      test_device(),
      StaticByteBuffer(
          // ACL data header (handle: 1, pbf: continuing fr., length: 5)
          0x01,
          0x10,
          0x05,
          0x00,
          // continuing payload
          'o',
          'd',
          'b',
          'y',
          'e'));

  // SDU of length 7 corresponds to a 11-octet B-frame
  // Due to the BR/EDR buffer size, this should be sent over a 6-byte then a
  // 5-byte fragment.
  EXPECT_TRUE(sm_chan->Send(NewBuffer('G', 'o', 'o', 'd', 'b', 'y', 'e')));

  RunUntilIdle();
}

TEST_F(ChannelManagerRealAclChannelTest,
       SendBREDRFragmentedSDUsOverTwoDynamicChannels) {
  // Customize setup so that ACL payload size is 18 instead of default 1024
  TearDown();
  SetUp(/*max_acl_payload_size=*/18, /*max_le_payload_size=*/0);

  constexpr l2cap::Psm kPsm0 = l2cap::kAVCTP;
  constexpr l2cap::ChannelId kLocalId0 = 0x0040;
  constexpr l2cap::ChannelId kRemoteId0 = 0x9042;

  constexpr l2cap::Psm kPsm1 = l2cap::kAVDTP;
  constexpr l2cap::ChannelId kLocalId1 = 0x0041;
  constexpr l2cap::ChannelId kRemoteId1 = 0x9043;

  // L2CAP connection request/response, config request, config response
  constexpr size_t kChannelCreationPacketCount = 3;

  QueueAclConnection(kTestHandle1);
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  l2cap::Channel::WeakPtr channel0;
  auto chan_cb0 = [&](auto activated_chan) {
    EXPECT_EQ(kTestHandle1, activated_chan->link_handle());
    channel0 = std::move(activated_chan);
  };
  QueueOutboundL2capConnection(
      kTestHandle1, kPsm0, kLocalId0, kRemoteId0, std::move(chan_cb0));

  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
  ASSERT_TRUE(channel0.is_alive());
  ASSERT_TRUE(channel0->Activate(NopRxCallback, DoNothing));

  l2cap::Channel::WeakPtr channel1;
  auto chan_cb1 = [&](auto activated_chan) {
    EXPECT_EQ(kTestHandle1, activated_chan->link_handle());
    channel1 = std::move(activated_chan);
  };
  QueueOutboundL2capConnection(
      kTestHandle1, kPsm1, kLocalId1, kRemoteId1, std::move(chan_cb1));

  // Free up the buffer space from packets sent while creating |channel0|
  test_device()->SendCommandChannelPacket(NumberOfCompletedPacketsPacket(
      kTestHandle1, kChannelCreationPacketCount));
  RunUntilIdle();

  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
  EXPECT_TRUE(channel1.is_alive());
  ASSERT_TRUE(channel1->Activate(NopRxCallback, DoNothing));

  // Free up the buffer space from packets sent while creating |channel1|
  test_device()->SendCommandChannelPacket(NumberOfCompletedPacketsPacket(
      kTestHandle1,
      kConnectionCreationPacketCount + kChannelCreationPacketCount));
  RunUntilIdle();

  // Queue size should be equal to or larger than |num_queued_packets| to ensure
  // that all packets get queued and sent
  uint16_t num_queued_packets = 3;
  EXPECT_TRUE(num_queued_packets <= kDefaultTxMaxQueuedCount);

  // Queue 14 packets in total, distributed between the two channels
  // Fill up BR/EDR controller buffer then queue 3 additional packets (which
  // will be later fragmented to form 6 packets)
  for (size_t i = 0; i < kBufferMaxNumPackets / 2 + num_queued_packets; i++) {
    Channel::WeakPtr channel = (i % 2) ? channel1 : channel0;
    ChannelId channel_id = (i % 2) ? kRemoteId1 : kRemoteId0;

    const StaticByteBuffer kPacket0(
        // ACL data header (handle: 1, length: 18)
        0x01,
        0x00,
        0x12,
        0x00,
        // L2CAP B-frame: (length: 14, channel-id, partial payload)
        0x14,
        0x00,
        LowerBits(channel_id),
        UpperBits(channel_id),
        'G',
        'o',
        'o',
        'o',
        'o',
        'o',
        'o',
        'o',
        'o',
        'o',
        'o',
        'o',
        'o',
        'o');
    EXPECT_ACL_PACKET_OUT(test_device(), kPacket0);

    const StaticByteBuffer kPacket1(
        // ACL data header (handle: 1, pbf: continuing fr., length: 6)
        0x01,
        0x10,
        0x06,
        0x00,
        // continuing payload
        'o',
        'o',
        'd',
        'b',
        'y',
        'e');
    EXPECT_ACL_PACKET_OUT(test_device(), kPacket1);

    // SDU of length 20 corresponds to a 24-octet B-frame
    // Due to the BR/EDR buffer size, this should be sent over a 18-byte then a
    // 6-byte fragment.
    EXPECT_TRUE(channel->Send(NewBuffer('G',
                                        'o',
                                        'o',
                                        'o',
                                        'o',
                                        'o',
                                        'o',
                                        'o',
                                        'o',
                                        'o',
                                        'o',
                                        'o',
                                        'o',
                                        'o',
                                        'o',
                                        'o',
                                        'd',
                                        'b',
                                        'y',
                                        'e')));
    RunUntilIdle();
  }
  EXPECT_FALSE(test_device()->AllExpectedDataPacketsSent());

  // Notify the processed packets with a Number Of Completed Packet HCI event
  // This should cause the remaining 6 fragmented packets to be sent
  test_device()->SendCommandChannelPacket(
      NumberOfCompletedPacketsPacket(kTestHandle1, 6));
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
}

// Tests that fragmentation of LE packets uses the LE buffer size
TEST_F(ChannelManagerRealAclChannelTest, SendLEFragmentedSDUs) {
  TearDown();
  SetUp(/*max_acl_payload_size=*/6, /*max_le_payload_size=*/5);

  LEFixedChannels fixed_channels = QueueLEConnection(
      kTestHandle1, pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  ASSERT_TRUE(fixed_channels.att->Activate(NopRxCallback, DoNothing));

  EXPECT_ACL_PACKET_OUT(
      test_device(),
      StaticByteBuffer(
          // ACL data header (handle: 1, length: 5)
          0x01,
          0x00,
          0x05,
          0x00,
          // L2CAP B-frame: (length: 5, channel-id: 4, partial payload)
          0x05,
          0x00,
          0x04,
          0x00,
          'H'));

  EXPECT_ACL_PACKET_OUT(
      test_device(),
      StaticByteBuffer(
          // ACL data header (handle: 1, pbf: continuing fr., length: 4)
          0x01,
          0x10,
          0x04,
          0x00,
          // Continuing payload
          'e',
          'l',
          'l',
          'o'));

  // SDU of length 5 corresponds to a 9-octet B-frame which should be sent over
  // a 5-byte and a 4-byte fragment.
  EXPECT_TRUE(fixed_channels.att->Send(NewBuffer('H', 'e', 'l', 'l', 'o')));

  RunUntilIdle();
}

TEST_F(ChannelManagerMockAclChannelTest, ACLChannelSignalLinkError) {
  bool link_error = false;
  auto link_error_cb = [&link_error] { link_error = true; };
  BrEdrFixedChannels fixed_channels;
  auto fixed_channels_cb = [&fixed_channels](BrEdrFixedChannels channels) {
    fixed_channels = channels;
  };
  QueueRegisterACLRetVal return_val =
      QueueRegisterACL(kTestHandle1,
                       pw::bluetooth::emboss::ConnectionRole::CENTRAL,
                       link_error_cb,
                       NopSecurityCallback,
                       fixed_channels_cb);
  RunUntilIdle();
  ReceiveL2capInformationResponses(
      return_val.extended_features_id,
      return_val.fixed_channels_supported_id,
      /*features=*/0,
      kFixedChannelsSupportedBitSignaling | kFixedChannelsSupportedBitSM);
  RunUntilIdle();
  ASSERT_TRUE(fixed_channels.smp.is_alive());
  fixed_channels.smp->Activate(NopRxCallback, DoNothing);
  fixed_channels.smp->SignalLinkError();
  RunUntilIdle();
  EXPECT_TRUE(link_error);
}

TEST_F(ChannelManagerMockAclChannelTest, LEChannelSignalLinkError) {
  bool link_error = false;
  auto link_error_cb = [&link_error] { link_error = true; };
  LEFixedChannels fixed_channels =
      RegisterLE(kTestHandle1,
                 pw::bluetooth::emboss::ConnectionRole::CENTRAL,
                 link_error_cb);

  // Activate a new Attribute channel to signal the error.
  fixed_channels.att->Activate(NopRxCallback, DoNothing);
  fixed_channels.att->SignalLinkError();

  RunUntilIdle();

  EXPECT_TRUE(link_error);
}

TEST_F(ChannelManagerMockAclChannelTest, SignalLinkErrorDisconnectsChannels) {
  bool link_error = false;
  auto link_error_cb = [this, &link_error] {
    // This callback is run after the expectation for
    // OutboundDisconnectionRequest is set, so this tests that L2CAP-level
    // teardown happens before ChannelManager requests a link teardown.
    ASSERT_TRUE(AllExpectedPacketsSent());
    link_error = true;

    // Simulate closing the link.
    chanmgr()->RemoveConnection(kTestHandle1);
  };
  BrEdrFixedChannels fixed_channels;
  auto fixed_channels_cb = [&fixed_channels](BrEdrFixedChannels channels) {
    fixed_channels = std::move(channels);
  };
  QueueRegisterACLRetVal acl =
      QueueRegisterACL(kTestHandle1,
                       pw::bluetooth::emboss::ConnectionRole::CENTRAL,
                       link_error_cb,
                       NopSecurityCallback,
                       std::move(fixed_channels_cb));
  RunUntilIdle();
  ReceiveL2capInformationResponses(
      acl.extended_features_id,
      acl.fixed_channels_supported_id,
      /*features=*/0,
      kFixedChannelsSupportedBitSignaling | kFixedChannelsSupportedBitSM);

  const auto conn_req_id = NextCommandId();
  const auto config_req_id = NextCommandId();
  EXPECT_ACL_PACKET_OUT_(OutboundConnectionRequest(conn_req_id), kHighPriority);
  EXPECT_ACL_PACKET_OUT_(OutboundConfigurationRequest(config_req_id),
                         kHighPriority);
  EXPECT_ACL_PACKET_OUT_(OutboundConfigurationResponse(kPeerConfigRequestId),
                         kHighPriority);

  Channel::WeakPtr dynamic_channel;
  auto channel_cb = [&dynamic_channel](l2cap::Channel::WeakPtr activated_chan) {
    dynamic_channel = std::move(activated_chan);
  };

  int dynamic_channel_closed = 0;
  ActivateOutboundChannel(
      kTestPsm,
      kChannelParams,
      std::move(channel_cb),
      kTestHandle1,
      /*closed_cb=*/[&dynamic_channel_closed] { dynamic_channel_closed++; });

  ReceiveAclDataPacket(InboundConnectionResponse(conn_req_id));
  ReceiveAclDataPacket(InboundConfigurationRequest(kPeerConfigRequestId));
  ReceiveAclDataPacket(InboundConfigurationResponse(config_req_id));

  RETURN_IF_FATAL(RunUntilIdle());
  EXPECT_TRUE(AllExpectedPacketsSent());

  // The channel on kTestHandle1 should be open.
  EXPECT_TRUE(dynamic_channel.is_alive());
  EXPECT_EQ(0, dynamic_channel_closed);

  EXPECT_TRUE(AllExpectedPacketsSent());
  const auto disconn_req_id = NextCommandId();
  EXPECT_ACL_PACKET_OUT_(OutboundDisconnectionRequest(disconn_req_id),
                         kHighPriority);

  // Activate a new Security Manager channel to signal the error on
  // kTestHandle1.
  ASSERT_TRUE(fixed_channels.smp.is_alive());
  int fixed_channel_closed = 0;
  fixed_channels.smp->Activate(
      NopRxCallback,
      /*closed_callback=*/[&fixed_channel_closed] { fixed_channel_closed++; });
  ASSERT_FALSE(link_error);
  fixed_channels.smp->SignalLinkError();

  RETURN_IF_FATAL(RunUntilIdle());

  // link_error_cb is not called until Disconnection Response is received for
  // each dynamic channel
  EXPECT_FALSE(link_error);

  // But channels should be deactivated to prevent any activity.
  EXPECT_EQ(1, fixed_channel_closed);
  EXPECT_EQ(1, dynamic_channel_closed);

  ASSERT_TRUE(AllExpectedPacketsSent());
  const auto disconnection_rsp = testing::AclDisconnectionRsp(
      disconn_req_id, kTestHandle1, kLocalId, kRemoteId);
  ReceiveAclDataPacket(disconnection_rsp);

  RETURN_IF_FATAL(RunUntilIdle());

  EXPECT_TRUE(link_error);
}

TEST_F(ChannelManagerRealAclChannelTest, LEConnectionParameterUpdateRequest) {
  EXPECT_ACL_PACKET_OUT(
      test_device(),
      StaticByteBuffer(
          // ACL data header (handle: 0x0001, length: 10 bytes)
          0x01,
          0x00,
          0x0a,
          0x00,
          // L2CAP B-frame header (length: 6 bytes, channel-id: 0x0005 (LEsig))
          0x06,
          0x00,
          0x05,
          0x00,
          // L2CAP C-frame header
          // (LE conn. param. update response, id: 1, length: 2 bytes)
          0x13,
          0x01,
          0x02,
          0x00,
          // res: accepted
          0x00,
          0x00));

  LEFixedChannels fixed_channels = QueueLEConnection(
      kTestHandle1, pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  ASSERT_TRUE(fixed_channels.att.is_alive());
  ASSERT_TRUE(fixed_channels.smp.is_alive());

  ASSERT_TRUE(fixed_channels.att->Activate(NopRxCallback, DoNothing));
  ASSERT_TRUE(fixed_channels.smp->Activate(NopRxCallback, DoNothing));

  // clang-format off
  test_device()->SendACLDataChannelPacket(StaticByteBuffer(
      // ACL data header (handle: 0x0001, length: 16 bytes)
      0x01, 0x00, 0x10, 0x00,
      // L2CAP B-frame header (length: 12 bytes, channel-id: 0x0005 (LE sig))
      0x0C, 0x00, 0x05, 0x00,
      // L2CAP C-frame header (LE conn. param. update request, id: 1, length: 8 bytes)
      0x12, 0x01, 0x08, 0x00,
      // Connection parameters (hardcoded to match the expectations in |conn_param_cb|).
      0x06, 0x00,
      0x80, 0x0C,
      0xF3, 0x01,
      0x80, 0x0C));
  // clang-format on

  RunUntilIdle();
}

TEST_F(ChannelManagerMockAclChannelTest,
       ACLOutboundDynamicChannelLocalDisconnect) {
  QueueRegisterACL(kTestHandle1,
                   pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  RunUntilIdle();

  Channel::WeakPtr channel;
  auto channel_cb = [&channel](l2cap::Channel::WeakPtr activated_chan) {
    channel = std::move(activated_chan);
  };

  bool closed_cb_called = false;
  auto closed_cb = [&closed_cb_called] { closed_cb_called = true; };

  const auto conn_req_id = NextCommandId();
  const auto config_req_id = NextCommandId();
  EXPECT_ACL_PACKET_OUT_(OutboundConnectionRequest(conn_req_id), kHighPriority);
  EXPECT_ACL_PACKET_OUT_(OutboundConfigurationRequest(config_req_id),
                         kHighPriority);
  EXPECT_ACL_PACKET_OUT_(OutboundConfigurationResponse(kPeerConfigRequestId),
                         kHighPriority);

  ActivateOutboundChannel(kTestPsm,
                          kChannelParams,
                          std::move(channel_cb),
                          kTestHandle1,
                          std::move(closed_cb));
  RunUntilIdle();

  ReceiveAclDataPacket(InboundConnectionResponse(conn_req_id));
  ReceiveAclDataPacket(InboundConfigurationRequest(kPeerConfigRequestId));
  ReceiveAclDataPacket(InboundConfigurationResponse(config_req_id));

  RunUntilIdle();

  EXPECT_TRUE(AllExpectedPacketsSent());
  ASSERT_TRUE(channel.is_alive());
  EXPECT_FALSE(closed_cb_called);
  EXPECT_EQ(kLocalId, channel->id());
  EXPECT_EQ(kRemoteId, channel->remote_id());
  EXPECT_EQ(RetransmissionAndFlowControlMode::kBasic, channel->mode());

  // Test SDU transmission.
  // SDU must have remote channel ID (unlike for fixed channels).
  EXPECT_ACL_PACKET_OUT_(StaticByteBuffer(
                             // ACL data header (handle: 1, length 8)
                             0x01,
                             0x00,
                             0x08,
                             0x00,
                             // L2CAP B-frame: (length: 4, channel-id)
                             0x04,
                             0x00,
                             LowerBits(kRemoteId),
                             UpperBits(kRemoteId),
                             'T',
                             'e',
                             's',
                             't'),
                         kLowPriority);

  EXPECT_TRUE(channel->Send(NewBuffer('T', 'e', 's', 't')));

  RunUntilIdle();
  EXPECT_TRUE(AllExpectedPacketsSent());

  const auto disconn_req_id = NextCommandId();
  EXPECT_ACL_PACKET_OUT_(OutboundDisconnectionRequest(disconn_req_id),
                         kHighPriority);

  // Explicit deactivation should not res in |closed_cb| being called.
  channel->Deactivate();

  RunUntilIdle();
  EXPECT_TRUE(AllExpectedPacketsSent());

  // Ensure callback is not called after the channel has disconnected
  acl_data_channel()->set_drop_queued_packets_cb(nullptr);

  // clang-format off
  ReceiveAclDataPacket(StaticByteBuffer(
      // ACL data header (handle: 0x0001, length: 12 bytes)
      0x01, 0x00, 0x0c, 0x00,
      // L2CAP B-frame header (length: 8 bytes, channel-id: 0x0001 (ACL sig))
      0x08, 0x00, 0x01, 0x00,
      // Disconnection Response
      // (ID, length: 4, dst cid, src cid)
      0x07, disconn_req_id, 0x04, 0x00,
      LowerBits(kRemoteId), UpperBits(kRemoteId), LowerBits(kLocalId), UpperBits(kLocalId)));
  // clang-format on

  RunUntilIdle();

  EXPECT_FALSE(closed_cb_called);
}

TEST_F(ChannelManagerRealAclChannelTest,
       ACLOutboundDynamicChannelRemoteDisconnect) {
  QueueAclConnection(kTestHandle1,
                     pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  Channel::WeakPtr channel;
  auto chan_cb = [&](l2cap::Channel::WeakPtr activated_chan) {
    EXPECT_EQ(kTestHandle1, activated_chan->link_handle());
    channel = std::move(activated_chan);
  };
  QueueOutboundL2capConnection(
      kTestHandle1, kTestPsm, kLocalId, kRemoteId, std::move(chan_cb));

  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
  EXPECT_TRUE(channel.is_alive());
  bool dynamic_channel_closed = false;
  ASSERT_TRUE(channel->Activate(NopRxCallback,
                                /*closed_callback=*/[&dynamic_channel_closed] {
                                  dynamic_channel_closed = true;
                                }));
  EXPECT_FALSE(dynamic_channel_closed);
  EXPECT_EQ(kLocalId, channel->id());
  EXPECT_EQ(kRemoteId, channel->remote_id());
  EXPECT_EQ(RetransmissionAndFlowControlMode::kBasic, channel->mode());

  // Test SDU reception.
  test_device()->SendACLDataChannelPacket(StaticByteBuffer(
      // ACL data header (handle: 1, length 8)
      0x01,
      0x00,
      0x08,
      0x00,
      // L2CAP B-frame: (length: 4, channel-id)
      0x04,
      0x00,
      LowerBits(kLocalId),
      UpperBits(kLocalId),
      'T',
      'e',
      's',
      't'));

  RunUntilIdle();

  EXPECT_ACL_PACKET_OUT(test_device(), OutboundDisconnectionResponse(7));

  // clang-format off
  test_device()->SendACLDataChannelPacket(StaticByteBuffer(
      // ACL data header (handle: 0x0001, length: 12 bytes)
      0x01, 0x00, 0x0c, 0x00,
      // L2CAP B-frame header (length: 8 bytes, channel-id: 0x0001 (ACL sig))
      0x08, 0x00, 0x01, 0x00,
      // Disconnection Request (ID: 7, length: 4, dst cid, src cid)
      0x06, 0x07, 0x04, 0x00,
      LowerBits(kLocalId), UpperBits(kLocalId), LowerBits(kRemoteId), UpperBits(kRemoteId)));
  // clang-format on

  // The preceding peer disconnection should have immediately destroyed the
  // route to the channel. L2CAP will process it and this following SDU
  // back-to-back. The latter should be dropped.
  test_device()->SendACLDataChannelPacket(StaticByteBuffer(
      // ACL data header (handle: 1, length 5)
      0x01,
      0x00,
      0x05,
      0x00,
      // L2CAP B-frame: (length: 1, channel-id: 0x0040)
      0x01,
      0x00,
      0x40,
      0x00,
      '!'));

  RunUntilIdle();

  EXPECT_TRUE(dynamic_channel_closed);
}

TEST_F(ChannelManagerMockAclChannelTest,
       ACLOutboundDynamicChannelDataNotBuffered) {
  QueueRegisterACL(kTestHandle1,
                   pw::bluetooth::emboss::ConnectionRole::CENTRAL);

  Channel::WeakPtr channel;
  auto channel_cb = [&channel](l2cap::Channel::WeakPtr activated_chan) {
    channel = std::move(activated_chan);
  };

  bool channel_closed = false;
  auto closed_cb = [&channel_closed] { channel_closed = true; };

  auto data_rx_cb = [](ByteBufferPtr) {
    FAIL() << "Unexpected data reception";
  };

  // Receive SDU for the channel about to be opened. It should be ignored.
  ReceiveAclDataPacket(StaticByteBuffer(
      // ACL data header (handle: 1, length 8)
      0x01,
      0x00,
      0x08,
      0x00,
      // L2CAP B-frame: (length: 4, channel-id)
      0x04,
      0x00,
      LowerBits(kLocalId),
      UpperBits(kLocalId),
      'T',
      'e',
      's',
      't'));

  const auto conn_req_id = NextCommandId();
  const auto config_req_id = NextCommandId();
  EXPECT_ACL_PACKET_OUT_(OutboundConnectionRequest(conn_req_id), kHighPriority);
  EXPECT_ACL_PACKET_OUT_(OutboundConfigurationRequest(config_req_id),
                         kHighPriority);
  EXPECT_ACL_PACKET_OUT_(OutboundConfigurationResponse(kPeerConfigRequestId),
                         kHighPriority);

  ActivateOutboundChannel(kTestPsm,
                          kChannelParams,
                          std::move(channel_cb),
                          kTestHandle1,
                          std::move(closed_cb),
                          std::move(data_rx_cb));
  RunUntilIdle();

  ReceiveAclDataPacket(InboundConnectionResponse(conn_req_id));

  // The channel is connected but not configured, so no data should flow on the
  // channel. Test that this received data is also ignored.
  // clang-format off
  ReceiveAclDataPacket(StaticByteBuffer(
      // ACL data header (handle: 1, length 8)
      0x01, 0x00, 0x08, 0x00,
      // L2CAP B-frame: (length: 4, channel-id)
      0x04, 0x00, LowerBits(kLocalId), UpperBits(kLocalId), 'T', 'e', 's', 't'));
  // clang-format on

  ReceiveAclDataPacket(InboundConfigurationRequest(kPeerConfigRequestId));
  ReceiveAclDataPacket(InboundConfigurationResponse(config_req_id));

  RunUntilIdle();

  EXPECT_TRUE(AllExpectedPacketsSent());
  EXPECT_TRUE(channel.is_alive());
  EXPECT_FALSE(channel_closed);

  EXPECT_ACL_PACKET_OUT_(OutboundDisconnectionResponse(7), kHighPriority);

  // clang-format off
  ReceiveAclDataPacket(StaticByteBuffer(
      // ACL data header (handle: 0x0001, length: 12 bytes)
      0x01, 0x00, 0x0c, 0x00,
      // L2CAP B-frame header (length: 8 bytes, channel-id: 0x0001 (ACL sig))
      0x08, 0x00, 0x01, 0x00,
      // Disconnection Request
      // (ID: 7, length: 4, dst cid, src cid)
      0x06, 0x07, 0x04, 0x00,
      LowerBits(kLocalId), UpperBits(kLocalId), LowerBits(kRemoteId), UpperBits(kRemoteId)));
  // clang-format on

  RunUntilIdle();
}

TEST_F(ChannelManagerMockAclChannelTest,
       ACLOutboundDynamicChannelRemoteRefused) {
  QueueRegisterACL(kTestHandle1,
                   pw::bluetooth::emboss::ConnectionRole::CENTRAL);

  bool channel_cb_called = false;
  auto channel_cb = [&channel_cb_called](l2cap::Channel::WeakPtr channel) {
    channel_cb_called = true;
    EXPECT_FALSE(channel.is_alive());
  };

  const CommandId conn_req_id = NextCommandId();
  EXPECT_ACL_PACKET_OUT_(OutboundConnectionRequest(conn_req_id), kHighPriority);

  ActivateOutboundChannel(kTestPsm, kChannelParams, std::move(channel_cb));

  // clang-format off
  ReceiveAclDataPacket(StaticByteBuffer(
      // ACL data header (handle: 0x0001, length: 16 bytes)
      0x01, 0x00, 0x10, 0x00,
      // L2CAP B-frame header (length: 12 bytes, channel-id: 0x0001 (ACL sig))
      0x0c, 0x00, 0x01, 0x00,
      // Connection Response (ID, length: 8, dst cid: 0x0000 (invalid),
      // src cid, res: 0x0004 (Refused; no resources available), status: none)
      0x03, conn_req_id, 0x08, 0x00,
      0x00, 0x00, LowerBits(kLocalId), UpperBits(kLocalId),
      0x04, 0x00, 0x00, 0x00));
  // clang-format on

  RunUntilIdle();
  EXPECT_TRUE(AllExpectedPacketsSent());
  EXPECT_TRUE(channel_cb_called);
}

TEST_F(ChannelManagerMockAclChannelTest,
       ACLOutboundDynamicChannelFailedConfiguration) {
  QueueRegisterACL(kTestHandle1,
                   pw::bluetooth::emboss::ConnectionRole::CENTRAL);

  bool channel_cb_called = false;
  auto channel_cb = [&channel_cb_called](l2cap::Channel::WeakPtr channel) {
    channel_cb_called = true;
    EXPECT_FALSE(channel.is_alive());
  };

  const auto conn_req_id = NextCommandId();
  const auto config_req_id = NextCommandId();
  const auto disconn_req_id = NextCommandId();
  EXPECT_ACL_PACKET_OUT_(OutboundConnectionRequest(conn_req_id), kHighPriority);
  EXPECT_ACL_PACKET_OUT_(OutboundConfigurationRequest(config_req_id),
                         kHighPriority);
  EXPECT_ACL_PACKET_OUT_(OutboundConfigurationResponse(kPeerConfigRequestId),
                         kHighPriority);
  EXPECT_ACL_PACKET_OUT_(OutboundDisconnectionRequest(disconn_req_id),
                         kHighPriority);

  ActivateOutboundChannel(kTestPsm, kChannelParams, std::move(channel_cb));

  ReceiveAclDataPacket(InboundConnectionResponse(conn_req_id));
  ReceiveAclDataPacket(InboundConfigurationRequest(kPeerConfigRequestId));

  // clang-format off
  ReceiveAclDataPacket(StaticByteBuffer(
      // ACL data header (handle: 0x0001, length: 14 bytes)
      0x01, 0x00, 0x0e, 0x00,
      // L2CAP B-frame header (length: 10 bytes, channel-id: 0x0001 (ACL sig))
      0x0a, 0x00, 0x01, 0x00,
      // Configuration Response (ID, length: 6, src cid, flags: 0,
      // res: 0x0002 (Rejected; no reason provided))
      0x05, config_req_id, 0x06, 0x00,
      LowerBits(kLocalId), UpperBits(kLocalId), 0x00, 0x00,
      0x02, 0x00));

  ReceiveAclDataPacket(StaticByteBuffer(
      // ACL data header (handle: 0x0001, length: 12 bytes)
      0x01, 0x00, 0x0c, 0x00,
      // L2CAP B-frame header (length: 8 bytes, channel-id: 0x0001 (ACL sig))
      0x08, 0x00, 0x01, 0x00,
      // Disconnection Response (ID, length: 4, dst cid, src cid)
      0x07, disconn_req_id, 0x04, 0x00,
      LowerBits(kRemoteId), UpperBits(kRemoteId), LowerBits(kLocalId), UpperBits(kLocalId)));
  // clang-format on

  RunUntilIdle();
  EXPECT_TRUE(AllExpectedPacketsSent());
  EXPECT_TRUE(channel_cb_called);
}

TEST_F(ChannelManagerMockAclChannelTest,
       ACLInboundDynamicChannelLocalDisconnect) {
  constexpr Psm kBadPsm0 = 0x0004;
  constexpr Psm kBadPsm1 = 0x0103;

  QueueRegisterACL(kTestHandle1,
                   pw::bluetooth::emboss::ConnectionRole::CENTRAL);

  bool dynamic_channel_closed = false;
  auto closed_cb = [&dynamic_channel_closed] { dynamic_channel_closed = true; };

  Channel::WeakPtr channel;
  auto channel_cb = [&channel, closed_cb = std::move(closed_cb)](
                        l2cap::Channel::WeakPtr opened_chan) {
    channel = std::move(opened_chan);
    EXPECT_TRUE(channel->Activate(NopRxCallback, std::move(closed_cb)));
  };

  EXPECT_FALSE(
      chanmgr()->RegisterService(kBadPsm0, ChannelParameters(), channel_cb));
  EXPECT_FALSE(
      chanmgr()->RegisterService(kBadPsm1, ChannelParameters(), channel_cb));
  EXPECT_TRUE(chanmgr()->RegisterService(
      kTestPsm, ChannelParameters(), std::move(channel_cb)));

  const auto config_req_id = NextCommandId();
  EXPECT_ACL_PACKET_OUT_(OutboundConnectionResponse(1), kHighPriority);
  EXPECT_ACL_PACKET_OUT_(OutboundConfigurationRequest(config_req_id),
                         kHighPriority);
  EXPECT_ACL_PACKET_OUT_(OutboundConfigurationResponse(kPeerConfigRequestId),
                         kHighPriority);

  ReceiveAclDataPacket(InboundConnectionRequest(1));
  ReceiveAclDataPacket(InboundConfigurationRequest(kPeerConfigRequestId));
  ReceiveAclDataPacket(InboundConfigurationResponse(config_req_id));

  RunUntilIdle();

  EXPECT_TRUE(AllExpectedPacketsSent());
  ASSERT_TRUE(channel.is_alive());
  EXPECT_FALSE(dynamic_channel_closed);
  EXPECT_EQ(kLocalId, channel->id());
  EXPECT_EQ(kRemoteId, channel->remote_id());

  // Test SDU transmission.
  // SDU must have remote channel ID (unlike for fixed channels).
  EXPECT_ACL_PACKET_OUT_(StaticByteBuffer(
                             // ACL data header (handle: 1, length 7)
                             0x01,
                             0x00,
                             0x08,
                             0x00,
                             // L2CAP B-frame: (length: 3, channel-id)
                             0x04,
                             0x00,
                             LowerBits(kRemoteId),
                             UpperBits(kRemoteId),
                             'T',
                             'e',
                             's',
                             't'),
                         kLowPriority);

  EXPECT_TRUE(channel->Send(NewBuffer('T', 'e', 's', 't')));

  RunUntilIdle();
  EXPECT_TRUE(AllExpectedPacketsSent());

  const auto disconn_req_id = NextCommandId();
  EXPECT_ACL_PACKET_OUT_(OutboundDisconnectionRequest(disconn_req_id),
                         kHighPriority);

  // Explicit deactivation should not res in |closed_cb| being called.
  channel->Deactivate();

  RunUntilIdle();
  EXPECT_TRUE(AllExpectedPacketsSent());

  // clang-format off
  ReceiveAclDataPacket(StaticByteBuffer(
      // ACL data header (handle: 0x0001, length: 12 bytes)
      0x01, 0x00, 0x0c, 0x00,
      // L2CAP B-frame header (length: 8 bytes, channel-id: 0x0001 (ACL sig))
      0x08, 0x00, 0x01, 0x00,
      // Disconnection Response (ID, length: 4, dst cid, src cid)
      0x07, disconn_req_id, 0x04, 0x00,
      LowerBits(kRemoteId), UpperBits(kRemoteId), LowerBits(kLocalId), UpperBits(kLocalId)));
  // clang-format on

  RunUntilIdle();

  EXPECT_FALSE(dynamic_channel_closed);
}

TEST_F(ChannelManagerRealAclChannelTest, LinkSecurityProperties) {
  sm::SecurityProperties security(sm::SecurityLevel::kEncrypted,
                                  16,
                                  /*secure_connections=*/false);

  // Has no effect.
  chanmgr()->AssignLinkSecurityProperties(kTestHandle1, security);

  // Register a link and open a channel.
  // The security properties should be accessible using the channel.
  LEFixedChannels fixed_channels = QueueLEConnection(
      kTestHandle1, pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  ASSERT_TRUE(fixed_channels.att->Activate(NopRxCallback, DoNothing));

  // The channel should start out at the lowest level of security.
  EXPECT_EQ(sm::SecurityProperties(), fixed_channels.att->security());

  // Assign a new security level.
  chanmgr()->AssignLinkSecurityProperties(kTestHandle1, security);

  // Channel should return the new security level.
  EXPECT_EQ(security, fixed_channels.att->security());
}

TEST_F(ChannelManagerRealAclChannelTest,
       AssignLinkSecurityPropertiesOnClosedLinkDoesNothing) {
  // Register a link and open a channel.
  // The security properties should be accessible using the channel.
  LEFixedChannels fixed_channels = QueueLEConnection(
      kTestHandle1, pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  ASSERT_TRUE(fixed_channels.att->Activate(NopRxCallback, DoNothing));

  chanmgr()->RemoveConnection(kTestHandle1);
  RunUntilIdle();
  EXPECT_FALSE(fixed_channels.att.is_alive());

  // Assign a new security level.
  sm::SecurityProperties security(sm::SecurityLevel::kEncrypted,
                                  16,
                                  /*secure_connections=*/false);
  chanmgr()->AssignLinkSecurityProperties(kTestHandle1, security);
}

TEST_F(ChannelManagerMockAclChannelTest, UpgradeSecurity) {
  // The callback passed to to Channel::UpgradeSecurity().
  sm::Result<> received_status = fit::ok();
  int security_status_count = 0;
  auto status_callback = [&](sm::Result<> status) {
    received_status = status;
    security_status_count++;
  };

  // The security handler callback assigned when registering a link.
  sm::Result<> delivered_status = fit::ok();
  sm::SecurityLevel last_requested_level = sm::SecurityLevel::kNoSecurity;
  int security_request_count = 0;
  auto security_handler = [&](hci_spec::ConnectionHandle handle,
                              sm::SecurityLevel level,
                              auto callback) {
    EXPECT_EQ(kTestHandle1, handle);
    last_requested_level = level;
    security_request_count++;

    callback(delivered_status);
  };

  LEFixedChannels fixed_channels =
      RegisterLE(kTestHandle1,
                 pw::bluetooth::emboss::ConnectionRole::CENTRAL,
                 DoNothing,
                 NopLeConnParamCallback,
                 std::move(security_handler));
  l2cap::Channel::WeakPtr att = std::move(fixed_channels.att);
  ASSERT_TRUE(att->Activate(NopRxCallback, DoNothing));

  // Requesting security at or below the current level should succeed without
  // doing anything.
  att->UpgradeSecurity(sm::SecurityLevel::kNoSecurity, status_callback);
  RunUntilIdle();
  EXPECT_EQ(0, security_request_count);
  EXPECT_EQ(1, security_status_count);
  EXPECT_EQ(fit::ok(), received_status);

  // Test reporting an error.
  delivered_status = ToResult(HostError::kNotSupported);
  att->UpgradeSecurity(sm::SecurityLevel::kEncrypted, status_callback);
  RunUntilIdle();
  EXPECT_EQ(1, security_request_count);
  EXPECT_EQ(2, security_status_count);
  EXPECT_EQ(delivered_status, received_status);
  EXPECT_EQ(sm::SecurityLevel::kEncrypted, last_requested_level);

  chanmgr()->RemoveConnection(kTestHandle1);
  RunUntilIdle();
  EXPECT_FALSE(att.is_alive());
  EXPECT_EQ(1, security_request_count);
  EXPECT_EQ(2, security_status_count);
}

TEST_F(ChannelManagerMockAclChannelTest, MtuOutboundChannelConfiguration) {
  constexpr uint16_t kRemoteMtu = kDefaultMTU - 1;
  constexpr uint16_t kLocalMtu = kMaxMTU;

  QueueRegisterACL(kTestHandle1,
                   pw::bluetooth::emboss::ConnectionRole::CENTRAL);

  Channel::WeakPtr channel;
  auto channel_cb = [&channel](l2cap::Channel::WeakPtr activated_chan) {
    channel = std::move(activated_chan);
  };

  const auto conn_req_id = NextCommandId();
  const auto config_req_id = NextCommandId();

  // Signaling channel packets should be sent with high priority.
  EXPECT_ACL_PACKET_OUT_(OutboundConnectionRequest(conn_req_id), kHighPriority);
  EXPECT_ACL_PACKET_OUT_(OutboundConfigurationRequest(config_req_id),
                         kHighPriority);
  EXPECT_ACL_PACKET_OUT_(
      OutboundConfigurationResponse(kPeerConfigRequestId, kRemoteMtu),
      kHighPriority);

  ActivateOutboundChannel(
      kTestPsm, kChannelParams, std::move(channel_cb), kTestHandle1);

  ReceiveAclDataPacket(InboundConnectionResponse(conn_req_id));
  ReceiveAclDataPacket(
      InboundConfigurationRequest(kPeerConfigRequestId, kRemoteMtu));
  ReceiveAclDataPacket(InboundConfigurationResponse(config_req_id));

  RunUntilIdle();

  EXPECT_TRUE(AllExpectedPacketsSent());
  EXPECT_TRUE(channel.is_alive());
  EXPECT_EQ(kRemoteMtu, channel->max_tx_sdu_size());
  EXPECT_EQ(kLocalMtu, channel->max_rx_sdu_size());
}

TEST_F(ChannelManagerMockAclChannelTest, MtuInboundChannelConfiguration) {
  constexpr uint16_t kRemoteMtu = kDefaultMTU - 1;
  constexpr uint16_t kLocalMtu = kMaxMTU;

  QueueRegisterACL(kTestHandle1,
                   pw::bluetooth::emboss::ConnectionRole::CENTRAL);

  Channel::WeakPtr channel;
  auto channel_cb = [&channel](l2cap::Channel::WeakPtr opened_chan) {
    channel = std::move(opened_chan);
    EXPECT_TRUE(channel->Activate(NopRxCallback, DoNothing));
  };

  EXPECT_TRUE(chanmgr()->RegisterService(
      kTestPsm, kChannelParams, std::move(channel_cb)));

  CommandId kPeerConnectionRequestId = 3;
  const auto config_req_id = NextCommandId();

  EXPECT_ACL_PACKET_OUT_(OutboundConnectionResponse(kPeerConnectionRequestId),
                         kHighPriority);
  EXPECT_ACL_PACKET_OUT_(OutboundConfigurationRequest(config_req_id),
                         kHighPriority);
  EXPECT_ACL_PACKET_OUT_(
      OutboundConfigurationResponse(kPeerConfigRequestId, kRemoteMtu),
      kHighPriority);

  ReceiveAclDataPacket(InboundConnectionRequest(kPeerConnectionRequestId));
  ReceiveAclDataPacket(
      InboundConfigurationRequest(kPeerConfigRequestId, kRemoteMtu));
  ReceiveAclDataPacket(InboundConfigurationResponse(config_req_id));

  RunUntilIdle();
  EXPECT_TRUE(AllExpectedPacketsSent());
  EXPECT_TRUE(channel.is_alive());
  EXPECT_EQ(kRemoteMtu, channel->max_tx_sdu_size());
  EXPECT_EQ(kLocalMtu, channel->max_rx_sdu_size());
}

TEST_F(ChannelManagerMockAclChannelTest,
       OutboundChannelConfigurationUsesChannelParameters) {
  l2cap::ChannelParameters chan_params;
  auto config_mode =
      l2cap::RetransmissionAndFlowControlMode::kEnhancedRetransmission;
  chan_params.mode = config_mode;
  chan_params.max_rx_sdu_size = l2cap::kMinACLMTU;

  const auto cmd_ids = QueueRegisterACL(
      kTestHandle1, pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  ReceiveAclDataPacket(testing::AclExtFeaturesInfoRsp(
      cmd_ids.extended_features_id,
      kTestHandle1,
      kExtendedFeaturesBitEnhancedRetransmission));

  Channel::WeakPtr channel;
  auto channel_cb = [&channel](Channel::WeakPtr activated_chan) {
    channel = std::move(activated_chan);
  };

  const auto conn_req_id = NextCommandId();
  const auto config_req_id = NextCommandId();
  EXPECT_ACL_PACKET_OUT_(OutboundConnectionRequest(conn_req_id), kHighPriority);
  EXPECT_ACL_PACKET_OUT_(
      OutboundConfigurationRequest(
          config_req_id, *chan_params.max_rx_sdu_size, config_mode),
      kHighPriority);
  const auto kInboundMtu = kDefaultMTU;
  EXPECT_ACL_PACKET_OUT_(OutboundConfigurationResponse(
                             kPeerConfigRequestId, kInboundMtu, config_mode),
                         kHighPriority);

  ActivateOutboundChannel(
      kTestPsm, chan_params, std::move(channel_cb), kTestHandle1);

  ReceiveAclDataPacket(InboundConnectionResponse(conn_req_id));
  ReceiveAclDataPacket(InboundConfigurationRequest(
      kPeerConfigRequestId, kInboundMtu, config_mode));
  ReceiveAclDataPacket(InboundConfigurationResponse(config_req_id));

  RunUntilIdle();

  EXPECT_TRUE(AllExpectedPacketsSent());
  EXPECT_TRUE(channel.is_alive());
  EXPECT_EQ(*chan_params.max_rx_sdu_size, channel->max_rx_sdu_size());
  EXPECT_EQ(config_mode, channel->mode());

  // Receiver Ready poll request should elicit a response if ERTM has been set
  // up.
  EXPECT_ACL_PACKET_OUT_(
      testing::AclSFrameReceiverReady(kTestHandle1,
                                      kRemoteId,
                                      /*receive_seq_num=*/0,
                                      /*is_poll_request=*/false,
                                      /*is_poll_response=*/true),
      kLowPriority);
  ReceiveAclDataPacket(
      testing::AclSFrameReceiverReady(kTestHandle1,
                                      kLocalId,
                                      /*receive_seq_num=*/0,
                                      /*is_poll_request=*/true,
                                      /*is_poll_response=*/false));

  RunUntilIdle();
  EXPECT_TRUE(AllExpectedPacketsSent());
}

TEST_F(ChannelManagerMockAclChannelTest,
       InboundChannelConfigurationUsesChannelParameters) {
  CommandId kPeerConnReqId = 3;

  l2cap::ChannelParameters chan_params;
  auto config_mode =
      l2cap::RetransmissionAndFlowControlMode::kEnhancedRetransmission;
  chan_params.mode = config_mode;
  chan_params.max_rx_sdu_size = l2cap::kMinACLMTU;

  const auto cmd_ids = QueueRegisterACL(
      kTestHandle1, pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  ReceiveAclDataPacket(testing::AclExtFeaturesInfoRsp(
      cmd_ids.extended_features_id,
      kTestHandle1,
      kExtendedFeaturesBitEnhancedRetransmission));
  Channel::WeakPtr channel;
  auto channel_cb = [&channel](l2cap::Channel::WeakPtr opened_chan) {
    channel = std::move(opened_chan);
    EXPECT_TRUE(channel->Activate(NopRxCallback, DoNothing));
  };

  EXPECT_TRUE(
      chanmgr()->RegisterService(kTestPsm, chan_params, std::move(channel_cb)));

  const auto config_req_id = NextCommandId();
  EXPECT_ACL_PACKET_OUT_(OutboundConnectionResponse(kPeerConnReqId),
                         kHighPriority);
  EXPECT_ACL_PACKET_OUT_(
      OutboundConfigurationRequest(
          config_req_id, *chan_params.max_rx_sdu_size, config_mode),
      kHighPriority);
  const auto kInboundMtu = kDefaultMTU;
  EXPECT_ACL_PACKET_OUT_(OutboundConfigurationResponse(
                             kPeerConfigRequestId, kInboundMtu, config_mode),
                         kHighPriority);

  ReceiveAclDataPacket(InboundConnectionRequest(kPeerConnReqId));
  ReceiveAclDataPacket(InboundConfigurationRequest(
      kPeerConfigRequestId, kInboundMtu, config_mode));
  ReceiveAclDataPacket(InboundConfigurationResponse(config_req_id));

  RunUntilIdle();
  EXPECT_TRUE(AllExpectedPacketsSent());
  EXPECT_TRUE(channel.is_alive());
  EXPECT_EQ(*chan_params.max_rx_sdu_size, channel->max_rx_sdu_size());
  EXPECT_EQ(config_mode, channel->mode());

  // Receiver Ready poll request should elicit a response if ERTM has been set
  // up.
  EXPECT_ACL_PACKET_OUT_(
      testing::AclSFrameReceiverReady(kTestHandle1,
                                      kRemoteId,
                                      /*receive_seq_num=*/0,
                                      /*is_poll_request=*/false,
                                      /*is_poll_response=*/true),
      kLowPriority);
  ReceiveAclDataPacket(
      testing::AclSFrameReceiverReady(kTestHandle1,
                                      kLocalId,
                                      /*receive_seq_num=*/0,
                                      /*is_poll_request=*/true,
                                      /*is_poll_response=*/false));

  RunUntilIdle();
  EXPECT_TRUE(AllExpectedPacketsSent());
}

TEST_F(ChannelManagerMockAclChannelTest,
       UnregisteringUnknownHandleClearsPendingPacketsAndDoesNotCrash) {
  // Packet for unregistered handle should be queued.
  ReceiveAclDataPacket(
      testing::AclConnectionReq(1, kTestHandle1, kRemoteId, kTestPsm));
  chanmgr()->RemoveConnection(kTestHandle1);

  QueueRegisterACL(kTestHandle1,
                   pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  // Since pending connection request packet was cleared, no response should be
  // sent.
  RunUntilIdle();
}

TEST_F(
    ChannelManagerMockAclChannelTest,
    PacketsReceivedAfterChannelDeactivatedAndBeforeRemoveChannelCalledAreDropped) {
  QueueRegisterACL(kTestHandle1,
                   pw::bluetooth::emboss::ConnectionRole::CENTRAL);

  Channel::WeakPtr channel;
  auto channel_cb = [&channel](l2cap::Channel::WeakPtr opened_chan) {
    channel = std::move(opened_chan);
    EXPECT_TRUE(channel->Activate(NopRxCallback, DoNothing));
  };

  EXPECT_TRUE(chanmgr()->RegisterService(
      kTestPsm, kChannelParams, std::move(channel_cb)));

  CommandId kPeerConnectionRequestId = 3;
  CommandId kLocalConfigRequestId = NextCommandId();

  EXPECT_ACL_PACKET_OUT_(OutboundConnectionResponse(kPeerConnectionRequestId),
                         kHighPriority);
  EXPECT_ACL_PACKET_OUT_(OutboundConfigurationRequest(kLocalConfigRequestId),
                         kHighPriority);
  EXPECT_ACL_PACKET_OUT_(OutboundConfigurationResponse(kPeerConfigRequestId),
                         kHighPriority);

  ReceiveAclDataPacket(InboundConnectionRequest(kPeerConnectionRequestId));
  ReceiveAclDataPacket(InboundConfigurationRequest(kPeerConfigRequestId));
  ReceiveAclDataPacket(InboundConfigurationResponse(kLocalConfigRequestId));

  EXPECT_TRUE(AllExpectedPacketsSent());
  EXPECT_TRUE(channel.is_alive());

  EXPECT_ACL_PACKET_OUT_(OutboundDisconnectionRequest(NextCommandId()),
                         kHighPriority);

  // channel marked inactive & LogicalLink::RemoveChannel called.
  channel->Deactivate();
  EXPECT_TRUE(AllExpectedPacketsSent());

  StaticByteBuffer kPacket(
      // ACL data header (handle: 0x0001, length: 4 bytes)
      0x01,
      0x00,
      0x04,
      0x00,
      // L2CAP B-frame header (length: 0 bytes, channel-id)
      0x00,
      0x00,
      LowerBits(kLocalId),
      UpperBits(kLocalId));
  // Packet for removed channel should be dropped by LogicalLink.
  ReceiveAclDataPacket(kPacket);
}

TEST_F(ChannelManagerMockAclChannelTest,
       ReceiveFixedChannelsInformationResponseWithNotSupportedResult) {
  const auto cmd_ids = QueueRegisterACL(
      kTestHandle1, pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  // Handler should check for res and not crash from reading mask or type.
  ReceiveAclDataPacket(testing::AclNotSupportedInformationResponse(
      cmd_ids.fixed_channels_supported_id, kTestHandle1));
  RunUntilIdle();
}

TEST_F(ChannelManagerMockAclChannelTest,
       ReceiveFixedChannelsInformationResponseWithInvalidResult) {
  const auto cmd_ids = QueueRegisterACL(
      kTestHandle1, pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  // Handler should check for res and not crash from reading mask or type.
  StaticByteBuffer kPacket(
      // ACL data header (handle: |link_handle|, length: 12 bytes)
      LowerBits(kTestHandle1),
      UpperBits(kTestHandle1),
      0x0c,
      0x00,
      // L2CAP B-frame header (length: 8 bytes, channel-id: 0x0001 (ACL sig))
      0x08,
      0x00,
      0x01,
      0x00,
      // Information Response (type, ID, length: 4)
      l2cap::kInformationResponse,
      cmd_ids.fixed_channels_supported_id,
      0x04,
      0x00,
      // Type = Fixed Channels Supported
      LowerBits(
          static_cast<uint16_t>(InformationType::kFixedChannelsSupported)),
      UpperBits(
          static_cast<uint16_t>(InformationType::kFixedChannelsSupported)),
      // Invalid Result
      0xFF,
      0xFF);
  ReceiveAclDataPacket(kPacket);
  RunUntilIdle();
}

TEST_F(ChannelManagerMockAclChannelTest,
       ReceiveFixedChannelsInformationResponseWithIncorrectType) {
  const auto cmd_ids = QueueRegisterACL(
      kTestHandle1, pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  // Handler should check type and not attempt to read fixed channel mask.
  ReceiveAclDataPacket(testing::AclExtFeaturesInfoRsp(
      cmd_ids.fixed_channels_supported_id, kTestHandle1, 0));
  RunUntilIdle();
}

TEST_F(ChannelManagerMockAclChannelTest,
       ReceiveFixedChannelsInformationResponseWithRejectStatus) {
  const auto cmd_ids = QueueRegisterACL(
      kTestHandle1, pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  // Handler should check status and not attempt to read fields.
  ReceiveAclDataPacket(testing::AclCommandRejectNotUnderstoodRsp(
      cmd_ids.fixed_channels_supported_id, kTestHandle1));
  RunUntilIdle();
}

TEST_F(
    ChannelManagerMockAclChannelTest,
    ReceiveValidConnectionParameterUpdateRequestAsCentralAndRespondWithAcceptedResult) {
  // Valid parameter values
  constexpr uint16_t kIntervalMin = 6;
  constexpr uint16_t kIntervalMax = 7;
  constexpr uint16_t kPeripheralLatency = 1;
  constexpr uint16_t kTimeoutMult = 10;

  std::optional<hci_spec::LEPreferredConnectionParameters> params;
  LEConnectionParameterUpdateCallback param_cb =
      [&params](const hci_spec::LEPreferredConnectionParameters& cb_params) {
        params = cb_params;
      };

  LEFixedChannels fixed_channels =
      RegisterLE(kTestHandle1,
                 pw::bluetooth::emboss::ConnectionRole::CENTRAL,
                 /*link_error_cb=*/DoNothing,
                 std::move(param_cb));

  constexpr CommandId kParamReqId = 4;  // random

  EXPECT_LE_PACKET_OUT(testing::AclConnectionParameterUpdateRsp(
                           kParamReqId,
                           kTestHandle1,
                           ConnectionParameterUpdateResult::kAccepted),
                       kHighPriority);

  ReceiveAclDataPacket(
      testing::AclConnectionParameterUpdateReq(kParamReqId,
                                               kTestHandle1,
                                               kIntervalMin,
                                               kIntervalMax,
                                               kPeripheralLatency,
                                               kTimeoutMult));
  RunUntilIdle();

  ASSERT_TRUE(params.has_value());
  EXPECT_EQ(kIntervalMin, params->min_interval());
  EXPECT_EQ(kIntervalMax, params->max_interval());
  EXPECT_EQ(kPeripheralLatency, params->max_latency());
  EXPECT_EQ(kTimeoutMult, params->supervision_timeout());
}

// If an LE Peripheral host receives a Connection Parameter Update Request, it
// should reject it.
TEST_F(
    ChannelManagerMockAclChannelTest,
    ReceiveValidConnectionParameterUpdateRequestAsPeripheralAndRespondWithReject) {
  // Valid parameter values
  constexpr uint16_t kIntervalMin = 6;
  constexpr uint16_t kIntervalMax = 7;
  constexpr uint16_t kPeripheralLatency = 1;
  constexpr uint16_t kTimeoutMult = 10;

  std::optional<hci_spec::LEPreferredConnectionParameters> params;
  LEConnectionParameterUpdateCallback param_cb =
      [&params](const hci_spec::LEPreferredConnectionParameters& cb_params) {
        params = cb_params;
      };

  LEFixedChannels fixed_channels =
      RegisterLE(kTestHandle1,
                 pw::bluetooth::emboss::ConnectionRole::PERIPHERAL,
                 /*link_error_cb=*/DoNothing,
                 std::move(param_cb));

  constexpr CommandId kParamReqId = 4;  // random

  EXPECT_LE_PACKET_OUT(testing::AclCommandRejectNotUnderstoodRsp(
                           kParamReqId, kTestHandle1, kLESignalingChannelId),
                       kHighPriority);

  ReceiveAclDataPacket(
      testing::AclConnectionParameterUpdateReq(kParamReqId,
                                               kTestHandle1,
                                               kIntervalMin,
                                               kIntervalMax,
                                               kPeripheralLatency,
                                               kTimeoutMult));
  RunUntilIdle();

  ASSERT_FALSE(params.has_value());
}

TEST_F(
    ChannelManagerMockAclChannelTest,
    ReceiveInvalidConnectionParameterUpdateRequestsAndRespondWithRejectedResult) {
  // Valid parameter values
  constexpr uint16_t kIntervalMin = 6;
  constexpr uint16_t kIntervalMax = 7;
  constexpr uint16_t kPeripheralLatency = 1;
  constexpr uint16_t kTimeoutMult = 10;

  // Callback should not be called for request with invalid parameters.
  LEConnectionParameterUpdateCallback param_cb = [](auto /*params*/) {
    ADD_FAILURE();
  };
  LEFixedChannels fixed_channels =
      RegisterLE(kTestHandle1,
                 pw::bluetooth::emboss::ConnectionRole::CENTRAL,
                 /*link_error_cb=*/DoNothing,
                 std::move(param_cb));

  constexpr CommandId kParamReqId = 4;  // random

  std::array invalid_requests = {
      // interval min > interval max
      testing::AclConnectionParameterUpdateReq(kParamReqId,
                                               kTestHandle1,
                                               /*interval_min=*/7,
                                               /*interval_max=*/6,
                                               kPeripheralLatency,
                                               kTimeoutMult),
      // interval_min too small
      testing::AclConnectionParameterUpdateReq(
          kParamReqId,
          kTestHandle1,
          hci_spec::kLEConnectionIntervalMin - 1,
          kIntervalMax,
          kPeripheralLatency,
          kTimeoutMult),
      // interval max too large
      testing::AclConnectionParameterUpdateReq(
          kParamReqId,
          kTestHandle1,
          kIntervalMin,
          hci_spec::kLEConnectionIntervalMax + 1,
          kPeripheralLatency,
          kTimeoutMult),
      // latency too large
      testing::AclConnectionParameterUpdateReq(
          kParamReqId,
          kTestHandle1,
          kIntervalMin,
          kIntervalMax,
          hci_spec::kLEConnectionLatencyMax + 1,
          kTimeoutMult),
      // timeout multiplier too small
      testing::AclConnectionParameterUpdateReq(
          kParamReqId,
          kTestHandle1,
          kIntervalMin,
          kIntervalMax,
          kPeripheralLatency,
          hci_spec::kLEConnectionSupervisionTimeoutMin - 1),
      // timeout multiplier too large
      testing::AclConnectionParameterUpdateReq(
          kParamReqId,
          kTestHandle1,
          kIntervalMin,
          kIntervalMax,
          kPeripheralLatency,
          hci_spec::kLEConnectionSupervisionTimeoutMax + 1)};

  for (auto& req : invalid_requests) {
    EXPECT_LE_PACKET_OUT(testing::AclConnectionParameterUpdateRsp(
                             kParamReqId,
                             kTestHandle1,
                             ConnectionParameterUpdateResult::kRejected),
                         kHighPriority);
    ReceiveAclDataPacket(req);
  }
  RunUntilIdle();
}

TEST_F(ChannelManagerMockAclChannelTest,
       RequestConnParamUpdateForUnknownLinkIsNoOp) {
  auto update_cb = [](auto) { ADD_FAILURE(); };
  chanmgr()->RequestConnectionParameterUpdate(
      kTestHandle1,
      hci_spec::LEPreferredConnectionParameters(),
      std::move(update_cb));
  RunUntilIdle();
}

TEST_F(
    ChannelManagerMockAclChannelTest,
    RequestConnParamUpdateAsPeripheralAndReceiveAcceptedAndRejectedResponses) {
  LEFixedChannels fixed_channels = RegisterLE(
      kTestHandle1, pw::bluetooth::emboss::ConnectionRole::PERIPHERAL);

  // Valid parameter values
  constexpr uint16_t kIntervalMin = 6;
  constexpr uint16_t kIntervalMax = 7;
  constexpr uint16_t kPeripheralLatency = 1;
  constexpr uint16_t kTimeoutMult = 10;
  const hci_spec::LEPreferredConnectionParameters kParams(
      kIntervalMin, kIntervalMax, kPeripheralLatency, kTimeoutMult);

  std::optional<bool> accepted;
  auto request_cb = [&accepted](bool cb_accepted) { accepted = cb_accepted; };

  // Receive "Accepted" Response:

  CommandId param_update_req_id = NextCommandId();
  EXPECT_LE_PACKET_OUT(
      testing::AclConnectionParameterUpdateReq(param_update_req_id,
                                               kTestHandle1,
                                               kIntervalMin,
                                               kIntervalMax,
                                               kPeripheralLatency,
                                               kTimeoutMult),
      kHighPriority);
  chanmgr()->RequestConnectionParameterUpdate(
      kTestHandle1, kParams, request_cb);
  RunUntilIdle();
  EXPECT_FALSE(accepted.has_value());

  ReceiveAclDataPacket(testing::AclConnectionParameterUpdateRsp(
      param_update_req_id,
      kTestHandle1,
      ConnectionParameterUpdateResult::kAccepted));
  RunUntilIdle();
  ASSERT_TRUE(accepted.has_value());
  EXPECT_TRUE(accepted.value());
  accepted.reset();

  // Receive "Rejected" Response:

  param_update_req_id = NextCommandId();
  EXPECT_LE_PACKET_OUT(
      testing::AclConnectionParameterUpdateReq(param_update_req_id,
                                               kTestHandle1,
                                               kIntervalMin,
                                               kIntervalMax,
                                               kPeripheralLatency,
                                               kTimeoutMult),
      kHighPriority);
  chanmgr()->RequestConnectionParameterUpdate(
      kTestHandle1, kParams, std::move(request_cb));
  RunUntilIdle();
  EXPECT_FALSE(accepted.has_value());

  ReceiveAclDataPacket(testing::AclConnectionParameterUpdateRsp(
      param_update_req_id,
      kTestHandle1,
      ConnectionParameterUpdateResult::kRejected));
  RunUntilIdle();
  ASSERT_TRUE(accepted.has_value());
  EXPECT_FALSE(accepted.value());
}

TEST_F(ChannelManagerMockAclChannelTest, ConnParamUpdateRequestRejected) {
  LEFixedChannels fixed_channels = RegisterLE(
      kTestHandle1, pw::bluetooth::emboss::ConnectionRole::PERIPHERAL);

  // Valid parameter values
  constexpr uint16_t kIntervalMin = 6;
  constexpr uint16_t kIntervalMax = 7;
  constexpr uint16_t kPeripheralLatency = 1;
  constexpr uint16_t kTimeoutMult = 10;
  const hci_spec::LEPreferredConnectionParameters kParams(
      kIntervalMin, kIntervalMax, kPeripheralLatency, kTimeoutMult);

  std::optional<bool> accepted;
  auto request_cb = [&accepted](bool cb_accepted) { accepted = cb_accepted; };

  const CommandId kParamUpdateReqId = NextCommandId();
  EXPECT_LE_PACKET_OUT(
      testing::AclConnectionParameterUpdateReq(kParamUpdateReqId,
                                               kTestHandle1,
                                               kIntervalMin,
                                               kIntervalMax,
                                               kPeripheralLatency,
                                               kTimeoutMult),
      kHighPriority);
  chanmgr()->RequestConnectionParameterUpdate(
      kTestHandle1, kParams, request_cb);
  RunUntilIdle();
  EXPECT_FALSE(accepted.has_value());

  ReceiveAclDataPacket(testing::AclCommandRejectNotUnderstoodRsp(
      kParamUpdateReqId, kTestHandle1, kLESignalingChannelId));
  RunUntilIdle();
  ASSERT_TRUE(accepted.has_value());
  EXPECT_FALSE(accepted.value());
}

TEST_F(ChannelManagerRealAclChannelTest,
       DestroyingChannelManagerReleasesLogicalLinkAndClosesChannels) {
  QueueAclConnection(kTestHandle1);
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  auto link = chanmgr()->LogicalLinkForTesting(kTestHandle1);
  ASSERT_TRUE(link.is_alive());

  bool closed = false;
  auto closed_cb = [&] { closed = true; };

  auto chan = ActivateNewFixedChannel(
      kConnectionlessChannelId, kTestHandle1, closed_cb);
  ASSERT_TRUE(chan.is_alive());
  ASSERT_FALSE(closed);

  TearDown();  // Destroys channel manager
  RunUntilIdle();
  EXPECT_TRUE(closed);
  // If link is still valid, there may be a memory leak.
  EXPECT_FALSE(link.is_alive());

  // If the above fails, check if the channel was holding a strong reference to
  // the link.
  chan = Channel::WeakPtr();
  RunUntilIdle();
  EXPECT_TRUE(closed);
  EXPECT_FALSE(link.is_alive());
}

TEST_F(ChannelManagerRealAclChannelTest, AutosniffSuppression) {
  QueueAclConnection(kTestHandle1);
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  auto link = chanmgr()->LogicalLinkForTesting(kTestHandle1);
  ASSERT_TRUE(link.is_alive());

  ASSERT_TRUE(!link->AutosniffIsSuppressed());
  auto suppression =
      chanmgr()->SuppressAutosniff(kTestHandle1, "autosniff test");
  ASSERT_TRUE(link->AutosniffIsSuppressed());
}

TEST_F(ChannelManagerMockAclChannelTest, RequestAclPriorityNormal) {
  QueueRegisterACL(kTestHandle1,
                   pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  RunUntilIdle();

  auto channel = SetUpOutboundChannel();

  std::optional<AclPriority> requested_priority;
  acl_data_channel()->set_request_acl_priority_cb(
      [&requested_priority](auto priority, auto handle, auto cb) {
        EXPECT_EQ(handle, kTestHandle1);
        requested_priority = priority;
        cb(fit::ok());
      });

  size_t result_cb_count = 0;
  channel->RequestAclPriority(AclPriority::kNormal, [&](auto res) {
    EXPECT_EQ(fit::ok(), res);
    result_cb_count++;
  });

  EXPECT_EQ(result_cb_count, 1u);
  EXPECT_FALSE(requested_priority.has_value());

  EXPECT_ACL_PACKET_OUT_(OutboundDisconnectionRequest(NextCommandId()),
                         kHighPriority);
  // Closing channel should not request normal priority because it is already
  // the current priority.
  channel->Deactivate();
  EXPECT_EQ(result_cb_count, 1u);
  EXPECT_FALSE(requested_priority.has_value());
}

TEST_F(ChannelManagerMockAclChannelTest, RequestAclPrioritySinkThenNormal) {
  QueueRegisterACL(kTestHandle1,
                   pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  RunUntilIdle();

  auto channel = SetUpOutboundChannel();

  std::optional<AclPriority> requested_priority;
  acl_data_channel()->set_request_acl_priority_cb(
      [&requested_priority](auto priority, auto handle, auto cb) {
        EXPECT_EQ(handle, kTestHandle1);
        requested_priority = priority;
        cb(fit::ok());
      });

  size_t result_cb_count = 0;
  channel->RequestAclPriority(AclPriority::kSink, [&](auto res) {
    EXPECT_EQ(fit::ok(), res);
    result_cb_count++;
  });

  EXPECT_EQ(result_cb_count, 1u);
  EXPECT_EQ(channel->requested_acl_priority(), AclPriority::kSink);
  ASSERT_TRUE(requested_priority.has_value());
  EXPECT_EQ(*requested_priority, AclPriority::kSink);

  channel->RequestAclPriority(AclPriority::kNormal, [&](auto res) {
    EXPECT_EQ(fit::ok(), res);
    result_cb_count++;
  });

  EXPECT_EQ(result_cb_count, 2u);
  ASSERT_TRUE(requested_priority.has_value());
  EXPECT_EQ(*requested_priority, AclPriority::kNormal);
  EXPECT_EQ(channel->requested_acl_priority(), AclPriority::kNormal);

  requested_priority.reset();

  EXPECT_ACL_PACKET_OUT_(OutboundDisconnectionRequest(NextCommandId()),
                         kHighPriority);
  // Closing channel should not request normal priority because it is already
  // the current priority.
  channel->Deactivate();
  EXPECT_FALSE(requested_priority.has_value());
}

TEST_F(ChannelManagerMockAclChannelTest,
       RequestAclPrioritySinkThenDeactivateChannelAfterResult) {
  QueueRegisterACL(kTestHandle1,
                   pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  RunUntilIdle();

  auto channel = SetUpOutboundChannel();

  std::optional<AclPriority> requested_priority;
  acl_data_channel()->set_request_acl_priority_cb(
      [&requested_priority](auto priority, auto handle, auto cb) {
        EXPECT_EQ(handle, kTestHandle1);
        requested_priority = priority;
        cb(fit::ok());
      });

  size_t result_cb_count = 0;
  channel->RequestAclPriority(AclPriority::kSink, [&](auto res) {
    EXPECT_EQ(fit::ok(), res);
    result_cb_count++;
  });

  EXPECT_EQ(result_cb_count, 1u);
  ASSERT_TRUE(requested_priority.has_value());
  EXPECT_EQ(*requested_priority, AclPriority::kSink);

  requested_priority.reset();

  EXPECT_ACL_PACKET_OUT_(OutboundDisconnectionRequest(NextCommandId()),
                         kHighPriority);
  channel->Deactivate();
  ASSERT_TRUE(requested_priority.has_value());
  EXPECT_EQ(*requested_priority, AclPriority::kNormal);
}

TEST_F(ChannelManagerMockAclChannelTest,
       RequestAclPrioritySinkThenReceiveDisconnectRequest) {
  QueueRegisterACL(kTestHandle1,
                   pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  RunUntilIdle();

  auto channel = SetUpOutboundChannel();

  std::optional<AclPriority> requested_priority;
  acl_data_channel()->set_request_acl_priority_cb(
      [&requested_priority](auto priority, auto handle, auto cb) {
        EXPECT_EQ(handle, kTestHandle1);
        requested_priority = priority;
        cb(fit::ok());
      });

  size_t result_cb_count = 0;
  channel->RequestAclPriority(AclPriority::kSink, [&](auto res) {
    EXPECT_EQ(fit::ok(), res);
    result_cb_count++;
  });

  EXPECT_EQ(result_cb_count, 1u);
  ASSERT_TRUE(requested_priority.has_value());
  EXPECT_EQ(*requested_priority, AclPriority::kSink);
  EXPECT_EQ(channel->requested_acl_priority(), AclPriority::kSink);

  requested_priority.reset();

  const auto kPeerDisconReqId = 1;
  EXPECT_ACL_PACKET_OUT_(OutboundDisconnectionResponse(kPeerDisconReqId),
                         kHighPriority);
  ReceiveAclDataPacket(testing::AclDisconnectionReq(
      kPeerDisconReqId, kTestHandle1, kRemoteId, kLocalId));
  RunUntilIdle();
  ASSERT_TRUE(requested_priority.has_value());
  EXPECT_EQ(*requested_priority, AclPriority::kNormal);
}

TEST_F(
    ChannelManagerMockAclChannelTest,
    RequestAclPrioritySinkThenDeactivateChannelBeforeResultShouldResetPriorityOnDeactivate) {
  QueueRegisterACL(kTestHandle1,
                   pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  RunUntilIdle();

  auto channel = SetUpOutboundChannel();

  std::vector<
      std::pair<AclPriority, fit::callback<void(fit::result<fit::failed>)>>>
      requests;
  acl_data_channel()->set_request_acl_priority_cb(
      [&](auto priority, auto handle, auto cb) {
        EXPECT_EQ(handle, kTestHandle1);
        requests.push_back({priority, std::move(cb)});
      });

  size_t result_cb_count = 0;
  channel->RequestAclPriority(AclPriority::kSink, [&](auto res) {
    EXPECT_EQ(fit::ok(), res);
    result_cb_count++;
  });
  EXPECT_EQ(channel->requested_acl_priority(), AclPriority::kNormal);
  EXPECT_EQ(result_cb_count, 0u);
  EXPECT_EQ(requests.size(), 1u);

  EXPECT_ACL_PACKET_OUT_(OutboundDisconnectionRequest(NextCommandId()),
                         kHighPriority);
  // Should queue kNormal ACL priority request.
  channel->Deactivate();
  ASSERT_EQ(requests.size(), 1u);

  requests[0].second(fit::ok());
  EXPECT_EQ(result_cb_count, 1u);
  ASSERT_EQ(requests.size(), 2u);
  EXPECT_EQ(requests[1].first, AclPriority::kNormal);

  requests[1].second(fit::ok());
}

TEST_F(ChannelManagerMockAclChannelTest, RequestAclPrioritySinkFails) {
  QueueRegisterACL(kTestHandle1,
                   pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  RunUntilIdle();

  auto channel = SetUpOutboundChannel();

  acl_data_channel()->set_request_acl_priority_cb(
      [](auto, auto handle, auto cb) {
        EXPECT_EQ(handle, kTestHandle1);
        cb(fit::failed());
      });

  size_t result_cb_count = 0;
  channel->RequestAclPriority(AclPriority::kSink, [&](auto res) {
    EXPECT_TRUE(res.is_error());
    result_cb_count++;
  });

  EXPECT_EQ(result_cb_count, 1u);
  EXPECT_EQ(channel->requested_acl_priority(), AclPriority::kNormal);

  EXPECT_ACL_PACKET_OUT_(OutboundDisconnectionRequest(NextCommandId()),
                         kHighPriority);
  channel->Deactivate();
}

TEST_F(ChannelManagerMockAclChannelTest,
       TwoChannelsRequestAclPrioritySinkAndDeactivate) {
  QueueRegisterACL(kTestHandle1,
                   pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  RunUntilIdle();

  const auto kChannelIds0 = std::make_pair(ChannelId(0x40), ChannelId(0x41));
  const auto kChannelIds1 = std::make_pair(ChannelId(0x41), ChannelId(0x42));

  auto channel_0 =
      SetUpOutboundChannel(kChannelIds0.first, kChannelIds0.second);
  auto channel_1 =
      SetUpOutboundChannel(kChannelIds1.first, kChannelIds1.second);

  std::optional<AclPriority> requested_priority;
  acl_data_channel()->set_request_acl_priority_cb(
      [&](auto priority, auto handle, auto cb) {
        EXPECT_EQ(handle, kTestHandle1);
        requested_priority = priority;
        cb(fit::ok());
      });

  size_t result_cb_count = 0;
  channel_0->RequestAclPriority(AclPriority::kSink, [&](auto res) {
    EXPECT_EQ(fit::ok(), res);
    result_cb_count++;
  });
  ASSERT_TRUE(requested_priority);
  EXPECT_EQ(*requested_priority, AclPriority::kSink);
  EXPECT_EQ(result_cb_count, 1u);
  EXPECT_EQ(channel_0->requested_acl_priority(), AclPriority::kSink);
  requested_priority.reset();

  channel_1->RequestAclPriority(AclPriority::kSink, [&](auto res) {
    EXPECT_EQ(fit::ok(), res);
    result_cb_count++;
  });
  // Priority is already sink. No additional request should be sent.
  EXPECT_FALSE(requested_priority);
  EXPECT_EQ(result_cb_count, 2u);
  EXPECT_EQ(channel_1->requested_acl_priority(), AclPriority::kSink);

  EXPECT_ACL_PACKET_OUT_(testing::AclDisconnectionReq(NextCommandId(),
                                                      kTestHandle1,
                                                      kChannelIds0.first,
                                                      kChannelIds0.second),
                         kHighPriority);
  channel_0->Deactivate();
  // Because channel_1 is still using sink priority, no command should be sent.
  EXPECT_FALSE(requested_priority);

  EXPECT_ACL_PACKET_OUT_(testing::AclDisconnectionReq(NextCommandId(),
                                                      kTestHandle1,
                                                      kChannelIds1.first,
                                                      kChannelIds1.second),
                         kHighPriority);
  channel_1->Deactivate();
  ASSERT_TRUE(requested_priority);
  EXPECT_EQ(*requested_priority, AclPriority::kNormal);
}

TEST_F(ChannelManagerMockAclChannelTest,
       TwoChannelsRequestConflictingAclPriorities) {
  QueueRegisterACL(kTestHandle1,
                   pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  RunUntilIdle();

  const auto kChannelIds0 = std::make_pair(ChannelId(0x40), ChannelId(0x41));
  const auto kChannelIds1 = std::make_pair(ChannelId(0x41), ChannelId(0x42));

  auto channel_0 =
      SetUpOutboundChannel(kChannelIds0.first, kChannelIds0.second);
  auto channel_1 =
      SetUpOutboundChannel(kChannelIds1.first, kChannelIds1.second);

  std::optional<AclPriority> requested_priority;
  acl_data_channel()->set_request_acl_priority_cb(
      [&](auto priority, auto handle, auto cb) {
        EXPECT_EQ(handle, kTestHandle1);
        requested_priority = priority;
        cb(fit::ok());
      });

  size_t result_cb_count = 0;
  channel_0->RequestAclPriority(AclPriority::kSink, [&](auto res) {
    EXPECT_EQ(fit::ok(), res);
    result_cb_count++;
  });
  ASSERT_TRUE(requested_priority);
  EXPECT_EQ(*requested_priority, AclPriority::kSink);
  EXPECT_EQ(result_cb_count, 1u);
  requested_priority.reset();

  channel_1->RequestAclPriority(AclPriority::kSource, [&](auto res) {
    EXPECT_TRUE(res.is_error());
    result_cb_count++;
  });
  // Priority conflict should prevent priority request.
  EXPECT_FALSE(requested_priority);
  EXPECT_EQ(result_cb_count, 2u);
  EXPECT_EQ(channel_1->requested_acl_priority(), AclPriority::kNormal);

  EXPECT_ACL_PACKET_OUT_(testing::AclDisconnectionReq(NextCommandId(),
                                                      kTestHandle1,
                                                      kChannelIds0.first,
                                                      kChannelIds0.second),
                         kHighPriority);
  channel_0->Deactivate();
  ASSERT_TRUE(requested_priority);
  EXPECT_EQ(*requested_priority, AclPriority::kNormal);
  requested_priority.reset();

  EXPECT_ACL_PACKET_OUT_(testing::AclDisconnectionReq(NextCommandId(),
                                                      kTestHandle1,
                                                      kChannelIds1.first,
                                                      kChannelIds1.second),
                         kHighPriority);
  channel_1->Deactivate();
  EXPECT_FALSE(requested_priority);
}

// If two channels request ACL priorities before the first command completes,
// they should receive responses as if they were handled strictly sequentially.
TEST_F(ChannelManagerMockAclChannelTest,
       TwoChannelsRequestAclPrioritiesAtSameTime) {
  QueueRegisterACL(kTestHandle1,
                   pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  RunUntilIdle();

  const auto kChannelIds0 = std::make_pair(ChannelId(0x40), ChannelId(0x41));
  const auto kChannelIds1 = std::make_pair(ChannelId(0x41), ChannelId(0x42));

  auto channel_0 =
      SetUpOutboundChannel(kChannelIds0.first, kChannelIds0.second);
  auto channel_1 =
      SetUpOutboundChannel(kChannelIds1.first, kChannelIds1.second);

  std::vector<fit::callback<void(fit::result<fit::failed>)>> command_callbacks;
  acl_data_channel()->set_request_acl_priority_cb(
      [&](auto, auto, auto cb) { command_callbacks.push_back(std::move(cb)); });

  size_t result_cb_count_0 = 0;
  channel_0->RequestAclPriority(AclPriority::kSink,
                                [&](auto) { result_cb_count_0++; });
  EXPECT_EQ(command_callbacks.size(), 1u);
  EXPECT_EQ(result_cb_count_0, 0u);

  size_t result_cb_count_1 = 0;
  channel_1->RequestAclPriority(AclPriority::kSource,
                                [&](auto) { result_cb_count_1++; });
  EXPECT_EQ(result_cb_count_1, 0u);
  ASSERT_EQ(command_callbacks.size(), 1u);

  command_callbacks[0](fit::ok());
  EXPECT_EQ(result_cb_count_0, 1u);
  // Second request should be notified of conflict error.
  EXPECT_EQ(result_cb_count_1, 1u);
  EXPECT_EQ(command_callbacks.size(), 1u);

  // Because requests should be handled sequentially, the second request should
  // have failed.
  EXPECT_EQ(channel_0->requested_acl_priority(), AclPriority::kSink);
  EXPECT_EQ(channel_1->requested_acl_priority(), AclPriority::kNormal);

  EXPECT_ACL_PACKET_OUT_(testing::AclDisconnectionReq(NextCommandId(),
                                                      kTestHandle1,
                                                      kChannelIds0.first,
                                                      kChannelIds0.second),
                         kHighPriority);
  channel_0->Deactivate();

  EXPECT_ACL_PACKET_OUT_(testing::AclDisconnectionReq(NextCommandId(),
                                                      kTestHandle1,
                                                      kChannelIds1.first,
                                                      kChannelIds1.second),
                         kHighPriority);
  channel_1->Deactivate();
}

TEST_F(ChannelManagerMockAclChannelTest,
       QueuedSinkAclPriorityForClosedChannelIsIgnored) {
  QueueRegisterACL(kTestHandle1,
                   pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  RunUntilIdle();

  auto channel = SetUpOutboundChannel();

  std::vector<
      std::pair<AclPriority, fit::callback<void(fit::result<fit::failed>)>>>
      requests;
  acl_data_channel()->set_request_acl_priority_cb(
      [&](auto priority, auto handle, auto cb) {
        EXPECT_EQ(handle, kTestHandle1);
        requests.push_back({priority, std::move(cb)});
      });

  size_t result_cb_count = 0;
  channel->RequestAclPriority(AclPriority::kSink, [&](auto res) {
    EXPECT_EQ(fit::ok(), res);
    result_cb_count++;
  });
  ASSERT_EQ(requests.size(), 1u);
  requests[0].second(fit::ok());
  EXPECT_EQ(channel->requested_acl_priority(), AclPriority::kSink);

  // Source request is queued and request is sent.
  channel->RequestAclPriority(AclPriority::kSource, [&](auto res) {
    EXPECT_EQ(fit::ok(), res);
    result_cb_count++;
  });
  ASSERT_EQ(requests.size(), 2u);
  EXPECT_EQ(result_cb_count, 1u);
  EXPECT_EQ(channel->requested_acl_priority(), AclPriority::kSink);

  // Sink request is queued. It should receive an error since it is handled
  // after the channel is closed.
  channel->RequestAclPriority(AclPriority::kSink, [&](auto res) {
    EXPECT_TRUE(res.is_error());
    result_cb_count++;
  });
  ASSERT_EQ(requests.size(), 2u);
  EXPECT_EQ(result_cb_count, 1u);
  EXPECT_EQ(channel->requested_acl_priority(), AclPriority::kSink);

  EXPECT_ACL_PACKET_OUT_(OutboundDisconnectionRequest(NextCommandId()),
                         kHighPriority);
  // Closing channel will queue normal request.
  channel->Deactivate();
  EXPECT_FALSE(channel.is_alive());

  // Send res to source request. Second sink request should receive error res
  // too.
  requests[1].second(fit::ok());
  EXPECT_EQ(result_cb_count, 3u);
  ASSERT_EQ(requests.size(), 3u);
  EXPECT_EQ(requests[2].first, AclPriority::kNormal);

  // Send response to kNormal request sent on Deactivate().
  requests[2].second(fit::ok());
}

#ifndef NINSPECT
TEST_F(ChannelManagerMockAclChannelTest, InspectHierarchy) {
  inspect::Inspector inspector;
  chanmgr()->AttachInspect(inspector.GetRoot(), "l2cap");

  chanmgr()->RegisterService(kSDP, kChannelParams, [](auto) {});
  auto services_matcher =
      AllOf(NodeMatches(NameMatches("services")),
            ChildrenMatch(ElementsAre(NodeMatches(
                AllOf(NameMatches("service_0x0"),
                      PropertyList(ElementsAre(StringIs("psm", "SDP"))))))));

  QueueRegisterACL(kTestHandle1,
                   pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  RunUntilIdle();

  const auto conn_req_id = NextCommandId();
  const auto config_req_id = NextCommandId();
  EXPECT_ACL_PACKET_OUT_(OutboundConnectionRequest(conn_req_id), kHighPriority);
  EXPECT_ACL_PACKET_OUT_(OutboundConfigurationRequest(config_req_id),
                         kHighPriority);
  EXPECT_ACL_PACKET_OUT_(OutboundConfigurationResponse(kPeerConfigRequestId),
                         kHighPriority);
  Channel::WeakPtr dynamic_channel;
  auto channel_cb = [&dynamic_channel](l2cap::Channel::WeakPtr activated_chan) {
    dynamic_channel = std::move(activated_chan);
  };
  ActivateOutboundChannel(
      kTestPsm, kChannelParams, std::move(channel_cb), kTestHandle1, []() {});
  ReceiveAclDataPacket(InboundConnectionResponse(conn_req_id));
  ReceiveAclDataPacket(InboundConfigurationRequest(kPeerConfigRequestId));
  ReceiveAclDataPacket(InboundConfigurationResponse(config_req_id));

  auto link = chanmgr()->LogicalLinkForTesting(kTestHandle1);
  auto suppress1 = link->SuppressAutosniff("test1");
  auto suppress2 = link->SuppressAutosniff("test2");

  auto signaling_chan_matcher = NodeMatches(AllOf(
      NameMatches("channel_0x2"),
      PropertyList(UnorderedElementsAre(StringIs("local_id", "0x0001"),
                                        StringIs("remote_id", "0x0001")))));
  auto dyn_chan_matcher = NodeMatches(
      AllOf(NameMatches("channel_0x3"),
            PropertyList(UnorderedElementsAre(StringIs("local_id", "0x0040"),
                                              StringIs("remote_id", "0x9042"),
                                              StringIs("psm", "SDP")))));
  auto channels_matcher = AllOf(NodeMatches(NameMatches("channels")),
                                ChildrenMatch(UnorderedElementsAre(
                                    signaling_chan_matcher, dyn_chan_matcher)));
  auto autosniff_matcher = NodeMatches(AllOf(
      NameMatches("autosniff"),
      PropertyList(UnorderedElementsAre(StringIs("sniff_mode", "ACTIVE"),
                                        StringIs("suppress_0x4", "test1"),
                                        StringIs("suppress_0x5", "test2")))));
  auto link_matcher = AllOf(
      NodeMatches(NameMatches("logical_links")),
      ChildrenMatch(ElementsAre(AllOf(
          NodeMatches(AllOf(
              NameMatches("logical_link_0x1"),
              PropertyList(UnorderedElementsAre(
                  StringIs("handle", "0x0001"),
                  StringIs("link_type", "ACL"),
                  UintIs("flush_timeout_ms",
                         std::chrono::duration_cast<std::chrono::milliseconds>(
                             pw::chrono::SystemClock::duration::max())
                             .count()))))),
          ChildrenMatch(
              UnorderedElementsAre(autosniff_matcher, channels_matcher))))));

  auto l2cap_node_matcher = AllOf(
      NodeMatches(NameMatches("l2cap")),
      ChildrenMatch(UnorderedElementsAre(link_matcher, services_matcher)));

  auto hierarchy = inspect::ReadFromVmo(inspector.DuplicateVmo()).take_value();
  EXPECT_THAT(hierarchy, ChildrenMatch(ElementsAre(l2cap_node_matcher)));

  // inspector must outlive ChannelManager
  chanmgr()->RemoveConnection(kTestHandle1);
}
#endif  // NINSPECT

TEST_F(
    ChannelManagerMockAclChannelTest,
    OutboundChannelWithFlushTimeoutInChannelParametersAndDelayedFlushTimeoutCallback) {
  QueueRegisterACL(kTestHandle1,
                   pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  RunUntilIdle();

  EXPECT_CMD_PACKET_OUT(test_device(),
                        WriteAutomaticFlushTimeoutPacket(
                            kTestHandle1, kExpectedFlushTimeoutParam));

  ChannelParameters chan_params;
  chan_params.flush_timeout = kFlushTimeout;

  Channel::WeakPtr channel;
  auto channel_cb = [&channel](l2cap::Channel::WeakPtr activated_chan) {
    channel = std::move(activated_chan);
  };
  SetUpOutboundChannelWithCallback(
      kLocalId, kRemoteId, /*closed_cb=*/DoNothing, chan_params, channel_cb);
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());
  // Channel should not be returned yet because setting flush timeout has not
  // completed yet.
  EXPECT_FALSE(channel.is_alive());

  // Completing the command should cause the channel to be returned.
  const auto kCommandComplete =
      CommandCompletePacket(hci_spec::kWriteAutomaticFlushTimeout,
                            pw::bluetooth::emboss::StatusCode::SUCCESS);
  test_device()->SendCommandChannelPacket(kCommandComplete);
  RunUntilIdle();
  ASSERT_TRUE(channel.is_alive());
  ASSERT_TRUE(channel->info().flush_timeout.has_value());
  EXPECT_EQ(channel->info().flush_timeout.value(), kFlushTimeout);

  EXPECT_ACL_PACKET_OUT_(StaticByteBuffer(
                             // ACL data header (handle: 1, packet boundary
                             // flag: kFirstFlushable, length: 6)
                             0x01,
                             0x20,
                             0x06,
                             0x00,
                             // L2CAP B-frame
                             0x02,
                             0x00,  // length: 2
                             LowerBits(kRemoteId),
                             UpperBits(kRemoteId),  // remote id
                             'h',
                             'i'),  // payload
                         kLowPriority);
  EXPECT_TRUE(channel->Send(NewBuffer('h', 'i')));
}

TEST_F(ChannelManagerMockAclChannelTest,
       OutboundChannelWithFlushTimeoutInChannelParametersFailure) {
  QueueRegisterACL(kTestHandle1,
                   pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  RunUntilIdle();

  const auto kCommandCompleteError = CommandCompletePacket(
      hci_spec::kWriteAutomaticFlushTimeout,
      pw::bluetooth::emboss::StatusCode::UNSPECIFIED_ERROR);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        WriteAutomaticFlushTimeoutPacket(
                            kTestHandle1, kExpectedFlushTimeoutParam),
                        &kCommandCompleteError);

  ChannelParameters chan_params;
  chan_params.flush_timeout = kFlushTimeout;

  auto channel = SetUpOutboundChannel(
      kLocalId, kRemoteId, /*closed_cb=*/DoNothing, chan_params);
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());
  // Flush timeout should not be set in channel info because setting a flush
  // timeout failed.
  EXPECT_FALSE(channel->info().flush_timeout.has_value());

  EXPECT_ACL_PACKET_OUT_(StaticByteBuffer(
                             // ACL data header (handle: 1, packet boundary
                             // flag: kFirstNonFlushable, length: 6)
                             0x01,
                             0x00,
                             0x06,
                             0x00,
                             // L2CAP B-frame
                             0x02,
                             0x00,  // length: 2
                             LowerBits(kRemoteId),
                             UpperBits(kRemoteId),  // remote id
                             'h',
                             'i'),  // payload
                         kLowPriority);
  EXPECT_TRUE(channel->Send(NewBuffer('h', 'i')));
}

TEST_F(ChannelManagerMockAclChannelTest,
       InboundChannelWithFlushTimeoutInChannelParameters) {
  QueueRegisterACL(kTestHandle1,
                   pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  RunUntilIdle();

  const auto kCommandComplete =
      CommandCompletePacket(hci_spec::kWriteAutomaticFlushTimeout,
                            pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        WriteAutomaticFlushTimeoutPacket(
                            kTestHandle1, kExpectedFlushTimeoutParam),
                        &kCommandComplete);

  ChannelParameters chan_params;
  chan_params.flush_timeout = kFlushTimeout;

  Channel::WeakPtr channel;
  auto channel_cb = [&channel](l2cap::Channel::WeakPtr opened_chan) {
    channel = std::move(opened_chan);
    EXPECT_TRUE(channel->Activate(NopRxCallback, DoNothing));
  };

  EXPECT_TRUE(
      chanmgr()->RegisterService(kTestPsm, chan_params, std::move(channel_cb)));

  CommandId kPeerConnectionRequestId = 3;
  const auto config_req_id = NextCommandId();

  EXPECT_ACL_PACKET_OUT_(OutboundConnectionResponse(kPeerConnectionRequestId),
                         kHighPriority);
  EXPECT_ACL_PACKET_OUT_(OutboundConfigurationRequest(config_req_id),
                         kHighPriority);
  EXPECT_ACL_PACKET_OUT_(OutboundConfigurationResponse(kPeerConfigRequestId),
                         kHighPriority);

  ReceiveAclDataPacket(InboundConnectionRequest(kPeerConnectionRequestId));
  ReceiveAclDataPacket(InboundConfigurationRequest(kPeerConfigRequestId));
  ReceiveAclDataPacket(InboundConfigurationResponse(config_req_id));

  RunUntilIdle();
  EXPECT_TRUE(AllExpectedPacketsSent());
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());
  ASSERT_TRUE(channel.is_alive());
  ASSERT_TRUE(channel->info().flush_timeout.has_value());
  EXPECT_EQ(channel->info().flush_timeout.value(), kFlushTimeout);

  EXPECT_ACL_PACKET_OUT_(StaticByteBuffer(
                             // ACL data header (handle: 1, packet boundary
                             // flag: kFirstFlushable, length: 6)
                             0x01,
                             0x20,
                             0x06,
                             0x00,
                             // L2CAP B-frame
                             0x02,
                             0x00,  // length: 2
                             LowerBits(kRemoteId),
                             UpperBits(kRemoteId),  // remote id
                             'h',
                             'i'),  // payload
                         kLowPriority);
  EXPECT_TRUE(channel->Send(NewBuffer('h', 'i')));
}

TEST_F(ChannelManagerMockAclChannelTest,
       FlushableChannelAndNonFlushableChannelOnSameLink) {
  QueueRegisterACL(kTestHandle1,
                   pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  RunUntilIdle();
  auto nonflushable_channel = SetUpOutboundChannel();
  auto flushable_channel = SetUpOutboundChannel(kLocalId + 1, kRemoteId + 1);

  const auto kCommandComplete =
      CommandCompletePacket(hci_spec::kWriteAutomaticFlushTimeout,
                            pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        WriteAutomaticFlushTimeoutPacket(
                            kTestHandle1, kExpectedFlushTimeoutParam),
                        &kCommandComplete);

  flushable_channel->SetBrEdrAutomaticFlushTimeout(
      kFlushTimeout, [](auto res) { EXPECT_EQ(fit::ok(), res); });
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());
  EXPECT_FALSE(nonflushable_channel->info().flush_timeout.has_value());
  ASSERT_TRUE(flushable_channel->info().flush_timeout.has_value());
  EXPECT_EQ(flushable_channel->info().flush_timeout.value(), kFlushTimeout);

  EXPECT_ACL_PACKET_OUT_(
      StaticByteBuffer(
          // ACL data header (handle: 1, packet boundary flag: kFirstFlushable,
          // length: 6)
          0x01,
          0x20,
          0x06,
          0x00,
          // L2CAP B-frame
          0x02,
          0x00,  // length: 2
          LowerBits(flushable_channel->remote_id()),
          UpperBits(flushable_channel->remote_id()),  // remote id
          'h',
          'i'),  // payload
      kLowPriority);
  EXPECT_TRUE(flushable_channel->Send(NewBuffer('h', 'i')));

  EXPECT_ACL_PACKET_OUT_(
      StaticByteBuffer(
          // ACL data header (handle: 1, packet boundary flag:
          // kFirstNonFlushable, length: 6)
          0x01,
          0x00,
          0x06,
          0x00,
          // L2CAP B-frame
          0x02,
          0x00,  // length: 2
          LowerBits(nonflushable_channel->remote_id()),
          UpperBits(nonflushable_channel->remote_id()),  // remote id
          'h',
          'i'),  // payload
      kLowPriority);
  EXPECT_TRUE(nonflushable_channel->Send(NewBuffer('h', 'i')));
}

TEST_F(ChannelManagerMockAclChannelTest, SettingFlushTimeoutFails) {
  QueueRegisterACL(kTestHandle1,
                   pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  RunUntilIdle();
  auto channel = SetUpOutboundChannel();

  const auto kCommandComplete = CommandCompletePacket(
      hci_spec::kWriteAutomaticFlushTimeout,
      pw::bluetooth::emboss::StatusCode::UNKNOWN_CONNECTION_ID);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        WriteAutomaticFlushTimeoutPacket(
                            kTestHandle1, kExpectedFlushTimeoutParam),
                        &kCommandComplete);

  channel->SetBrEdrAutomaticFlushTimeout(kFlushTimeout, [](auto res) {
    EXPECT_EQ(
        ToResult(pw::bluetooth::emboss::StatusCode::UNKNOWN_CONNECTION_ID),
        res);
  });
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());

  EXPECT_ACL_PACKET_OUT_(StaticByteBuffer(
                             // ACL data header (handle: 1, packet boundary
                             // flag: kFirstNonFlushable, length: 6)
                             0x01,
                             0x00,
                             0x06,
                             0x00,
                             // L2CAP B-frame
                             0x02,
                             0x00,  // length: 2
                             LowerBits(kRemoteId),
                             UpperBits(kRemoteId),  // remote id
                             'h',
                             'i'),  // payload
                         kLowPriority);
  EXPECT_TRUE(channel->Send(NewBuffer('h', 'i')));
}

TEST_F(ChannelManagerMockAclChannelTest, StartAndStopA2dpOffloadSuccess) {
  QueueRegisterACL(kTestHandle1,
                   pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  RunUntilIdle();

  A2dpOffloadManager::Configuration config = BuildConfiguration();
  Channel::WeakPtr channel = SetUpOutboundChannel();

  const auto command_complete =
      CommandCompletePacket(android_hci::kA2dpOffloadCommand,
                            pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        StartA2dpOffloadRequest(config,
                                                channel->link_handle(),
                                                channel->remote_id(),
                                                channel->max_tx_sdu_size()),
                        &command_complete);

  std::optional<hci::Result<>> result;
  channel->StartA2dpOffload(config, [&result](auto res) {
    EXPECT_EQ(ToResult(pw::bluetooth::emboss::StatusCode::SUCCESS), res);
    result = res;
  });
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->is_ok());

  EXPECT_CMD_PACKET_OUT(
      test_device(), StopA2dpOffloadRequest(), &command_complete);

  result.reset();
  channel->StopA2dpOffload([&result](auto res) {
    EXPECT_EQ(ToResult(pw::bluetooth::emboss::StatusCode::SUCCESS), res);
    result = res;
  });
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->is_ok());

  EXPECT_CMD_PACKET_OUT(test_device(),
                        StartA2dpOffloadRequest(config,
                                                channel->link_handle(),
                                                channel->remote_id(),
                                                channel->max_tx_sdu_size()),
                        &command_complete);

  result.reset();
  channel->StartA2dpOffload(config, [&result](auto res) {
    EXPECT_EQ(ToResult(pw::bluetooth::emboss::StatusCode::SUCCESS), res);
    result = res;
  });
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->is_ok());

  // Stop A2DP offload command sent on channel destruction
  EXPECT_CMD_PACKET_OUT(
      test_device(), StopA2dpOffloadRequest(), &command_complete);
}

TEST_F(ChannelManagerMockAclChannelTest,
       DisconnectChannelAfterA2dpOffloadStarted) {
  QueueRegisterACL(kTestHandle1,
                   pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  RunUntilIdle();

  A2dpOffloadManager::Configuration config = BuildConfiguration();
  Channel::WeakPtr channel = SetUpOutboundChannel();

  const auto command_complete =
      CommandCompletePacket(android_hci::kA2dpOffloadCommand,
                            pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        StartA2dpOffloadRequest(config,
                                                channel->link_handle(),
                                                channel->remote_id(),
                                                channel->max_tx_sdu_size()),
                        &command_complete);

  std::optional<hci::Result<>> result;
  channel->StartA2dpOffload(config, [&result](auto res) {
    EXPECT_EQ(ToResult(pw::bluetooth::emboss::StatusCode::SUCCESS), res);
    result = res;
  });
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->is_ok());

  ASSERT_TRUE(channel.is_alive());
  const auto disconn_req_id = NextCommandId();
  EXPECT_ACL_PACKET_OUT_(OutboundDisconnectionRequest(disconn_req_id),
                         kHighPriority);
  // Stop A2DP offload command sent on channel close
  EXPECT_CMD_PACKET_OUT(
      test_device(), StopA2dpOffloadRequest(), &command_complete);
  channel->Deactivate();
  ASSERT_FALSE(channel.is_alive());

  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());
}

TEST_F(ChannelManagerMockAclChannelTest,
       DisconnectChannelWhileA2dpOffloadStarting) {
  QueueRegisterACL(kTestHandle1,
                   pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  RunUntilIdle();

  A2dpOffloadManager::Configuration config = BuildConfiguration();
  Channel::WeakPtr channel = SetUpOutboundChannel();

  const auto command_complete =
      CommandCompletePacket(android_hci::kA2dpOffloadCommand,
                            pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        StartA2dpOffloadRequest(config,
                                                channel->link_handle(),
                                                channel->remote_id(),
                                                channel->max_tx_sdu_size()),
                        &command_complete);

  std::optional<hci::Result<>> result;
  channel->StartA2dpOffload(config, [&result](auto res) {
    EXPECT_EQ(ToResult(pw::bluetooth::emboss::StatusCode::SUCCESS), res);
    result = res;
  });
  EXPECT_FALSE(result.has_value());
  ASSERT_TRUE(channel.is_alive());

  const auto disconn_req_id = NextCommandId();
  EXPECT_ACL_PACKET_OUT_(OutboundDisconnectionRequest(disconn_req_id),
                         kHighPriority);
  // Stop A2DP offload command sent on channel close
  EXPECT_CMD_PACKET_OUT(
      test_device(), StopA2dpOffloadRequest(), &command_complete);
  channel->Deactivate();
  ASSERT_FALSE(channel.is_alive());

  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());
}

TEST_F(ChannelManagerMockAclChannelTest,
       SignalLinkErrorStopsDeliveryOfBufferedRxPackets) {
  // LE-U link
  LEFixedChannels fixed_channels =
      RegisterLE(kTestHandle1, pw::bluetooth::emboss::ConnectionRole::CENTRAL);

  // Queue 2 packets to be delivers on channel activation.
  StaticByteBuffer payload_0(0x00);
  ReceiveAclDataPacket(StaticByteBuffer(
      // ACL data header (starting fragment)
      0x01,
      0x00,  // connection handle + flags
      0x05,
      0x00,  // Length
      // L2CAP B-frame
      0x01,
      0x00,  // Length
      LowerBits(kATTChannelId),
      UpperBits(kATTChannelId),
      // Payload
      payload_0[0]));
  ReceiveAclDataPacket(StaticByteBuffer(
      // ACL data header (starting fragment)
      0x01,
      0x00,  // connection handle + flags
      0x05,
      0x00,  // Length
      // L2CAP B-frame
      0x01,
      0x00,  // Length
      LowerBits(kATTChannelId),
      UpperBits(kATTChannelId),
      // Payload
      0x01));
  RunUntilIdle();

  bool closed_called = false;
  auto closed_cb = [&closed_called] { closed_called = true; };

  int rx_count = 0;
  auto rx_callback = [&](ByteBufferPtr payload) {
    rx_count++;
    if (rx_count == 1) {
      EXPECT_THAT(*payload, BufferEq(payload_0));
      // This should stop delivery of the second packet.
      fixed_channels.att->SignalLinkError();
      return;
    }
  };
  ASSERT_TRUE(fixed_channels.att->Activate(std::move(rx_callback), closed_cb));
  RunUntilIdle();
  EXPECT_EQ(rx_count, 1);
  EXPECT_TRUE(closed_called);

  // Ensure the link is removed.
  chanmgr()->RemoveConnection(kTestHandle1);
  RunUntilIdle();
}

TEST_F(ChannelManagerRealAclChannelTest,
       InboundRfcommChannelFailsWithPsmNotSupported) {
  constexpr l2cap::Psm kPsm = l2cap::kRFCOMM;

  QueueAclConnection(kTestHandle1);
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  const l2cap::CommandId kPeerConnReqId = 1;

  // Incoming connection refused, RFCOMM is not routed.
  EXPECT_ACL_PACKET_OUT(test_device(),
                        l2cap::testing::AclConnectionRsp(
                            kPeerConnReqId,
                            kTestHandle1,
                            kRemoteId,
                            /*dst_id=*/0x0000,
                            l2cap::ConnectionResult::kPsmNotSupported));

  test_device()->SendACLDataChannelPacket(l2cap::testing::AclConnectionReq(
      kPeerConnReqId, kTestHandle1, kRemoteId, kPsm));

  RunUntilIdle();
}

TEST_F(ChannelManagerRealAclChannelTest,
       InboundPacketQueuedAfterChannelOpenIsNotDropped) {
  constexpr l2cap::Psm kPsm = l2cap::kSDP;

  QueueAclConnection(kTestHandle1);
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  Channel::WeakPtr channel;
  auto chan_cb = [&](l2cap::Channel::WeakPtr activated_chan) {
    EXPECT_EQ(kTestHandle1, activated_chan->link_handle());
    channel = std::move(activated_chan);
  };

  chanmgr()->RegisterService(kPsm, kChannelParameters, std::move(chan_cb));
  RunUntilIdle();

  constexpr l2cap::CommandId kConnectionReqId = 1;
  constexpr l2cap::CommandId kPeerConfigReqId = 6;
  const l2cap::CommandId kConfigReqId = NextCommandId();
  EXPECT_ACL_PACKET_OUT(
      test_device(),
      l2cap::testing::AclConnectionRsp(
          kConnectionReqId, kTestHandle1, kRemoteId, kLocalId));
  EXPECT_ACL_PACKET_OUT(
      test_device(),
      l2cap::testing::AclConfigReq(
          kConfigReqId, kTestHandle1, kRemoteId, kChannelParameters));
  test_device()->SendACLDataChannelPacket(l2cap::testing::AclConnectionReq(
      kConnectionReqId, kTestHandle1, kRemoteId, kPsm));

  // Config negotiation will not complete yet.
  RunUntilIdle();

  // Remaining config negotiation will be added to dispatch loop.
  EXPECT_ACL_PACKET_OUT(
      test_device(),
      l2cap::testing::AclConfigRsp(
          kPeerConfigReqId, kTestHandle1, kRemoteId, kChannelParameters));
  test_device()->SendACLDataChannelPacket(l2cap::testing::AclConfigReq(
      kPeerConfigReqId, kTestHandle1, kLocalId, kChannelParameters));
  test_device()->SendACLDataChannelPacket(l2cap::testing::AclConfigRsp(
      kConfigReqId, kTestHandle1, kLocalId, kChannelParameters));

  // Queue up a data packet for the new channel before the channel configuration
  // has been processed.
  ASSERT_FALSE(channel.is_alive());
  test_device()->SendACLDataChannelPacket(StaticByteBuffer(
      // ACL data header (handle: 1, length 8)
      0x01,
      0x00,
      0x08,
      0x00,

      // L2CAP B-frame: (length: 4, channel-id: 0x0040 (kLocalId))
      0x04,
      0x00,
      0x40,
      0x00,
      0xf0,
      0x9f,
      0x94,
      0xb0));

  // Run until the channel opens and the packet is written to the socket buffer.
  RunUntilIdle();
  ASSERT_TRUE(channel.is_alive());

  std::vector<ByteBufferPtr> rx_packets;
  auto rx_cb = [&rx_packets](ByteBufferPtr sdu) {
    rx_packets.push_back(std::move(sdu));
  };
  ASSERT_TRUE(channel->Activate(rx_cb, DoNothing));
  RunUntilIdle();
  ASSERT_EQ(rx_packets.size(), 1u);
  ASSERT_EQ(rx_packets[0]->size(), 4u);
  EXPECT_EQ("🔰", rx_packets[0]->view(0, 4u).AsString());
}

TEST_F(ChannelManagerRealAclChannelTest,
       NegotiateChannelParametersOnOutboundL2capChannel) {
  constexpr l2cap::Psm kPsm = l2cap::kAVDTP;
  constexpr uint16_t kMtu = l2cap::kMinACLMTU;

  l2cap::ChannelParameters chan_params;
  chan_params.mode =
      l2cap::RetransmissionAndFlowControlMode::kEnhancedRetransmission;
  chan_params.max_rx_sdu_size = kMtu;

  QueueAclConnection(kTestHandle1);
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  Channel::WeakPtr channel;
  auto chan_cb = [&](l2cap::Channel::WeakPtr activated_chan) {
    EXPECT_EQ(kTestHandle1, activated_chan->link_handle());
    channel = std::move(activated_chan);
  };

  QueueOutboundL2capConnection(kTestHandle1,
                               kPsm,
                               kLocalId,
                               kRemoteId,
                               chan_cb,
                               chan_params,
                               chan_params);

  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
  ASSERT_TRUE(channel.is_alive());
  EXPECT_EQ(kTestHandle1, channel->link_handle());
  EXPECT_EQ(*chan_params.max_rx_sdu_size, channel->max_rx_sdu_size());
  EXPECT_EQ(*chan_params.mode, channel->mode());
}

TEST_F(ChannelManagerRealAclChannelTest,
       NegotiateChannelParametersOnInboundChannel) {
  constexpr l2cap::Psm kPsm = l2cap::kAVDTP;

  l2cap::ChannelParameters chan_params;
  chan_params.mode =
      l2cap::RetransmissionAndFlowControlMode::kEnhancedRetransmission;
  chan_params.max_rx_sdu_size = l2cap::kMinACLMTU;

  QueueAclConnection(kTestHandle1);
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  Channel::WeakPtr channel;
  auto chan_cb = [&](l2cap::Channel::WeakPtr activated_chan) {
    EXPECT_EQ(kTestHandle1, activated_chan->link_handle());
    channel = std::move(activated_chan);
  };
  chanmgr()->RegisterService(kPsm, chan_params, chan_cb);

  QueueInboundL2capConnection(
      kTestHandle1, kPsm, kLocalId, kRemoteId, chan_params, chan_params);

  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
  ASSERT_TRUE(channel.is_alive());
  EXPECT_EQ(*chan_params.max_rx_sdu_size, channel->max_rx_sdu_size());
  EXPECT_EQ(*chan_params.mode, channel->mode());
}

TEST_F(ChannelManagerRealAclChannelTest,
       RequestConnectionParameterUpdateAndReceiveResponse) {
  // Valid parameter values
  constexpr uint16_t kIntervalMin = 6;
  constexpr uint16_t kIntervalMax = 7;
  constexpr uint16_t kPeripheralLatency = 1;
  constexpr uint16_t kTimeoutMult = 10;
  const hci_spec::LEPreferredConnectionParameters kParams(
      kIntervalMin, kIntervalMax, kPeripheralLatency, kTimeoutMult);

  QueueLEConnection(kTestHandle1,
                    pw::bluetooth::emboss::ConnectionRole::PERIPHERAL);

  std::optional<bool> accepted;
  auto request_cb = [&accepted](bool cb_accepted) { accepted = cb_accepted; };

  // Receive "Accepted" Response:
  l2cap::CommandId param_update_req_id = NextCommandId();
  EXPECT_ACL_PACKET_OUT(
      test_device(),
      l2cap::testing::AclConnectionParameterUpdateReq(param_update_req_id,
                                                      kTestHandle1,
                                                      kIntervalMin,
                                                      kIntervalMax,
                                                      kPeripheralLatency,
                                                      kTimeoutMult));
  chanmgr()->RequestConnectionParameterUpdate(
      kTestHandle1, kParams, request_cb);
  RunUntilIdle();
  EXPECT_FALSE(accepted.has_value());

  test_device()->SendACLDataChannelPacket(
      l2cap::testing::AclConnectionParameterUpdateRsp(
          param_update_req_id,
          kTestHandle1,
          l2cap::ConnectionParameterUpdateResult::kAccepted));
  RunUntilIdle();
  ASSERT_TRUE(accepted.has_value());
  EXPECT_TRUE(accepted.value());
  accepted.reset();
}

TEST_F(ChannelManagerRealAclChannelTest, AddLEConnectionReturnsFixedChannels) {
  auto channels = QueueLEConnection(
      kTestHandle1, pw::bluetooth::emboss::ConnectionRole::PERIPHERAL);
  ASSERT_TRUE(channels.att.is_alive());
  EXPECT_EQ(l2cap::kATTChannelId, channels.att->id());
  ASSERT_TRUE(channels.smp.is_alive());
  EXPECT_EQ(l2cap::kLESMPChannelId, channels.smp->id());
}

TEST_F(ChannelManagerRealAclChannelTest,
       OutboundChannelIsInvalidWhenL2capFailsToOpenChannel) {
  constexpr l2cap::Psm kPsm = l2cap::kAVCTP;

  // Don't register any links. This should cause outbound channels to fail.
  bool chan_cb_called = false;
  auto chan_cb = [&chan_cb_called](auto chan) {
    chan_cb_called = true;
    EXPECT_FALSE(chan.is_alive());
  };

  chanmgr()->OpenL2capChannel(
      kTestHandle1, kPsm, kChannelParameters, std::move(chan_cb));

  RunUntilIdle();

  EXPECT_TRUE(chan_cb_called);
}

TEST_F(ChannelManagerRealAclChannelTest, SignalingChannelAndOneDynamicChannel) {
  constexpr l2cap::Psm kPsm = l2cap::kSDP;

  // L2CAP connection request/response, config request, config response
  constexpr size_t kChannelCreationPacketCount = 3;

  QueueAclConnection(kTestHandle1);
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  l2cap::Channel::WeakPtr channel;
  auto chan_cb = [&](auto activated_chan) {
    EXPECT_EQ(kTestHandle1, activated_chan->link_handle());
    channel = std::move(activated_chan);
  };
  QueueOutboundL2capConnection(
      kTestHandle1, kPsm, kLocalId, kRemoteId, std::move(chan_cb));

  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
  EXPECT_TRUE(channel.is_alive());
  channel->Activate(NopRxCallback, DoNothing);

  // Free up the buffer space from packets sent while creating |channel|
  test_device()->SendCommandChannelPacket(NumberOfCompletedPacketsPacket(
      kTestHandle1,
      kConnectionCreationPacketCount + kChannelCreationPacketCount));

  // Fill up BR/EDR controller buffer then queue one additional packet
  for (size_t i = 0; i <= kBufferMaxNumPackets; i++) {
    // Last packet should remain queued
    if (i < kBufferMaxNumPackets) {
      const StaticByteBuffer kPacket(
          // ACL data header (handle: 0, length 1)
          0x01,
          0x00,
          0x05,
          0x00,
          // L2CAP B-frame: (length: 1, channel-id)
          0x01,
          0x00,
          LowerBits(kRemoteId),
          UpperBits(kRemoteId),
          // L2CAP payload
          0x01);
      EXPECT_ACL_PACKET_OUT(test_device(), kPacket);
    }
    // Create PDU to send on dynamic channel
    EXPECT_TRUE(channel->Send(NewBuffer(0x01)));
    RunUntilIdle();
  }
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  // Send out last packet
  EXPECT_ACL_PACKET_OUT(test_device(),
                        StaticByteBuffer(
                            // ACL data header (handle: 0, length 1)
                            0x01,
                            0x00,
                            0x05,
                            0x00,
                            // L2CAP B-frame: (length: 4, channel-id)
                            0x01,
                            0x00,
                            LowerBits(kRemoteId),
                            UpperBits(kRemoteId),
                            // L2CAP payload
                            0x01));
  test_device()->SendCommandChannelPacket(
      bt::testing::NumberOfCompletedPacketsPacket(kTestHandle1, 1));
  RunUntilIdle();

  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
}

TEST_F(ChannelManagerRealAclChannelTest,
       SignalingChannelAndTwoDynamicChannels) {
  constexpr l2cap::Psm kPsm0 = l2cap::kAVCTP;
  constexpr l2cap::ChannelId kLocalId0 = 0x0040;
  constexpr l2cap::ChannelId kRemoteId0 = 0x9042;

  constexpr l2cap::Psm kPsm1 = l2cap::kAVDTP;
  constexpr l2cap::ChannelId kLocalId1 = 0x0041;
  constexpr l2cap::ChannelId kRemoteId1 = 0x9043;

  // L2CAP connection request/response, config request, config response
  constexpr size_t kChannelCreationPacketCount = 3;

  QueueAclConnection(kTestHandle1);
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  l2cap::Channel::WeakPtr channel0;
  auto chan_cb0 = [&](auto activated_chan) {
    EXPECT_EQ(kTestHandle1, activated_chan->link_handle());
    channel0 = std::move(activated_chan);
  };
  QueueOutboundL2capConnection(
      kTestHandle1, kPsm0, kLocalId0, kRemoteId0, std::move(chan_cb0));

  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
  ASSERT_TRUE(channel0.is_alive());
  ASSERT_TRUE(channel0->Activate(NopRxCallback, DoNothing));

  l2cap::Channel::WeakPtr channel1;
  auto chan_cb1 = [&](auto activated_chan) {
    EXPECT_EQ(kTestHandle1, activated_chan->link_handle());
    channel1 = std::move(activated_chan);
  };
  QueueOutboundL2capConnection(
      kTestHandle1, kPsm1, kLocalId1, kRemoteId1, std::move(chan_cb1));

  // Free up the buffer space from packets sent while creating |channel0|
  test_device()->SendCommandChannelPacket(NumberOfCompletedPacketsPacket(
      kTestHandle1, kChannelCreationPacketCount));
  RunUntilIdle();

  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
  EXPECT_TRUE(channel1.is_alive());
  ASSERT_TRUE(channel1->Activate(NopRxCallback, DoNothing));

  // Free up the buffer space from packets sent while creating |channel1|
  test_device()->SendCommandChannelPacket(NumberOfCompletedPacketsPacket(
      kTestHandle1,
      kConnectionCreationPacketCount + kChannelCreationPacketCount));
  RunUntilIdle();

  // Queue size should be equal to or larger than |num_queued_packets| to ensure
  // that all packets get queued and sent
  uint16_t num_queued_packets = 5;
  EXPECT_TRUE(num_queued_packets <= kDefaultTxMaxQueuedCount);

  // Queue 15 packets in total, distributed between the two channels
  // Fill up BR/EDR controller buffer then queue 5 additional packets
  for (size_t i = 0; i < kBufferMaxNumPackets + num_queued_packets; i++) {
    Channel::WeakPtr channel = (i % 2) ? channel1 : channel0;
    ChannelId channel_id = (i % 2) ? kRemoteId1 : kRemoteId0;

    const StaticByteBuffer kPacket(
        // ACL data header (handle: 1, length 5)
        0x01,
        0x00,
        0x05,
        0x00,
        // L2CAP B-frame: (length: 1, channel_id)
        0x01,
        0x00,
        LowerBits(channel_id),
        UpperBits(channel_id),
        // L2CAP payload
        0x01);
    EXPECT_ACL_PACKET_OUT(test_device(), kPacket);

    // Create PDU to send on dynamic channel
    EXPECT_TRUE(channel->Send(NewBuffer(0x01)));
    RunUntilIdle();
  }
  EXPECT_FALSE(test_device()->AllExpectedDataPacketsSent());

  // Notify the processed packets with a Number Of Completed Packet HCI event
  // This should cause the remaining 5 packets to be sent
  test_device()->SendCommandChannelPacket(
      NumberOfCompletedPacketsPacket(kTestHandle1, 5));
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
}

TEST_F(ChannelManagerRealAclChannelTest, ChannelMaximumQueueSize) {
  constexpr l2cap::Psm kPsm = l2cap::kSDP;

  // L2CAP connection request/response, config request, config response
  constexpr size_t kChannelCreationPacketCount = 3;

  QueueAclConnection(kTestHandle1);
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  l2cap::Channel::WeakPtr channel;
  auto chan_cb = [&](auto activated_chan) {
    EXPECT_EQ(kTestHandle1, activated_chan->link_handle());
    channel = std::move(activated_chan);
  };
  QueueOutboundL2capConnection(
      kTestHandle1, kPsm, kLocalId, kRemoteId, std::move(chan_cb));

  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
  EXPECT_TRUE(channel.is_alive());
  channel->Activate(NopRxCallback, DoNothing);

  // Free up the buffer space from packets sent while creating |channel|
  test_device()->SendCommandChannelPacket(NumberOfCompletedPacketsPacket(
      kTestHandle1,
      kConnectionCreationPacketCount + kChannelCreationPacketCount));

  // Set maximum PDU queue size for |channel|. Queue size should be less than
  // |num_queued_packets| to ensure that some packets are dropped in order to
  // test maximum queue size behaviour
  const uint16_t kTxMaxQueuedCount = 10;
  channel->set_max_tx_queued(kTxMaxQueuedCount);
  uint16_t num_queued_packets = 20;

  // Fill up BR/EDR controller buffer then queue 20 additional packets. 10 of
  // these packets will be dropped since the maximum PDU queue size
  // |kTxMaxQueuedCount| is 10
  for (size_t i = 0; i < kBufferMaxNumPackets + num_queued_packets; i++) {
    if (i < kBufferMaxNumPackets + channel->max_tx_queued()) {
      const StaticByteBuffer kPacket(
          // ACL data header (handle: 0, length 1)
          0x01,
          0x00,
          0x05,
          0x00,
          // L2CAP B-frame: (length: 1, channel-id)
          0x01,
          0x00,
          LowerBits(kRemoteId),
          UpperBits(kRemoteId),
          // L2CAP payload
          0x01);

      EXPECT_ACL_PACKET_OUT(test_device(), kPacket);
    }

    // Create PDU to send on dynamic channel
    EXPECT_TRUE(channel->Send(NewBuffer(0x01)));
    RunUntilIdle();
  }
  EXPECT_FALSE(test_device()->AllExpectedDataPacketsSent());

  // Notify the processed packets with a Number Of Completed Packet HCI event
  // This should cause the remaining 10 packets to be sent
  test_device()->SendCommandChannelPacket(
      bt::testing::NumberOfCompletedPacketsPacket(kTestHandle1, 10));
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
}

class AclPriorityTest
    : public ChannelManagerRealAclChannelTest,
      public ::testing::WithParamInterface<std::pair<AclPriority, bool>> {};

TEST_P(AclPriorityTest, OutboundConnectAndSetPriority) {
  const auto kPriority = GetParam().first;
  const bool kExpectSuccess = GetParam().second;

  // Arbitrary command payload larger than CommandHeader.
  const auto op_code = hci_spec::VendorOpCode(0x01);
  const StaticByteBuffer kEncodedCommand(LowerBits(op_code),
                                         UpperBits(op_code),  // op code
                                         0x04,                // parameter size
                                         0x00,
                                         0x01,
                                         0x02,
                                         0x03);  // test parameter

  constexpr l2cap::Psm kPsm = l2cap::kAVCTP;

  std::optional<hci_spec::ConnectionHandle> connection_handle_from_encode_cb;
  std::optional<AclPriority> priority_from_encode_cb;
  test_device()->set_encode_vendor_command_cb(
      [&](pw::bluetooth::VendorCommandParameters vendor_params,
          fit::callback<void(pw::Result<pw::span<const std::byte>>)> callback) {
        ASSERT_TRUE(
            std::holds_alternative<
                pw::bluetooth::SetAclPriorityCommandParameters>(vendor_params));
        auto& params = std::get<pw::bluetooth::SetAclPriorityCommandParameters>(
            vendor_params);
        connection_handle_from_encode_cb = params.connection_handle;
        priority_from_encode_cb = params.priority;
        callback(kEncodedCommand.subspan());
      });

  QueueAclConnection(kTestHandle1);
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  Channel::WeakPtr channel;
  auto chan_cb = [&](l2cap::Channel::WeakPtr activated_chan) {
    EXPECT_EQ(kTestHandle1, activated_chan->link_handle());
    channel = std::move(activated_chan);
  };

  QueueOutboundL2capConnection(
      kTestHandle1, kPsm, kLocalId, kRemoteId, std::move(chan_cb));

  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
  // We should have opened a channel successfully.
  ASSERT_TRUE(channel.is_alive());
  channel->Activate([](auto) {}, []() {});

  if (kPriority != AclPriority::kNormal) {
    auto cmd_complete = CommandCompletePacket(
        op_code,
        kExpectSuccess ? pw::bluetooth::emboss::StatusCode::SUCCESS
                       : pw::bluetooth::emboss::StatusCode::UNKNOWN_COMMAND);
    EXPECT_CMD_PACKET_OUT(test_device(), kEncodedCommand, &cmd_complete);
  }

  size_t request_cb_count = 0;
  channel->RequestAclPriority(kPriority, [&](auto res) {
    request_cb_count++;
    EXPECT_EQ(kExpectSuccess, res.is_ok());
  });

  RunUntilIdle();
  EXPECT_EQ(request_cb_count, 1u);
  if (kPriority == AclPriority::kNormal) {
    EXPECT_FALSE(connection_handle_from_encode_cb);
  } else {
    ASSERT_TRUE(connection_handle_from_encode_cb);
    EXPECT_EQ(connection_handle_from_encode_cb.value(), kTestHandle1);
    ASSERT_TRUE(priority_from_encode_cb);
    EXPECT_EQ(priority_from_encode_cb.value(), kPriority);
  }
  connection_handle_from_encode_cb.reset();
  priority_from_encode_cb.reset();

  if (kPriority != AclPriority::kNormal && kExpectSuccess) {
    auto cmd_complete = CommandCompletePacket(
        op_code, pw::bluetooth::emboss::StatusCode::SUCCESS);
    EXPECT_CMD_PACKET_OUT(test_device(), kEncodedCommand, &cmd_complete);
  }

  EXPECT_ACL_PACKET_OUT(
      test_device(),
      l2cap::testing::AclDisconnectionReq(
          NextCommandId(), kTestHandle1, kLocalId, kRemoteId));

  // Deactivating channel should send priority command to revert priority back
  // to normal if it was changed.
  channel->Deactivate();
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  if (kPriority != AclPriority::kNormal && kExpectSuccess) {
    ASSERT_TRUE(connection_handle_from_encode_cb);
    EXPECT_EQ(connection_handle_from_encode_cb.value(), kTestHandle1);
    ASSERT_TRUE(priority_from_encode_cb);
    EXPECT_EQ(priority_from_encode_cb.value(), AclPriority::kNormal);
  } else {
    EXPECT_FALSE(connection_handle_from_encode_cb);
  }
}

const std::array<std::pair<AclPriority, bool>, 4> kPriorityParams = {
    {{AclPriority::kSource, false},
     {AclPriority::kSource, true},
     {AclPriority::kSink, true},
     {AclPriority::kNormal, true}}};
INSTANTIATE_TEST_SUITE_P(ChannelManagerMockControllerTest,
                         AclPriorityTest,
                         ::testing::ValuesIn(kPriorityParams));

#ifndef NINSPECT
TEST_F(ChannelManagerRealAclChannelTest, InspectHierarchy) {
  inspect::Inspector inspector;
  chanmgr()->AttachInspect(inspector.GetRoot(),
                           ChannelManager::kInspectNodeName);
  auto hierarchy = inspect::ReadFromVmo(inspector.DuplicateVmo());
  ASSERT_TRUE(hierarchy);
  auto l2cap_matcher = AllOf(NodeMatches(PropertyList(::testing::IsEmpty())),
                             ChildrenMatch(UnorderedElementsAre(
                                 NodeMatches(NameMatches("logical_links")),
                                 NodeMatches(NameMatches("services")))));
  EXPECT_THAT(hierarchy.value(),
              AllOf(ChildrenMatch(UnorderedElementsAre(l2cap_matcher))));
}
#endif  // NINSPECT

TEST_F(ChannelManagerMockAclChannelTest, LeaseAcquisitionAndRelease) {
  Channel::WeakPtr channel;
  auto channel_cb = [&channel](l2cap::Channel::WeakPtr opened_chan) {
    channel = std::move(opened_chan);
  };

  OpenInboundErtmChannel(std::move(channel_cb));
  ASSERT_TRUE(channel.is_alive());
  EXPECT_EQ(lease_provider().lease_count(), 0u);

  StaticByteBuffer payload(0x03);
  ReceiveAclDataPacket(testing::AclIFrame(kTestHandle1,
                                          channel->id(),
                                          /*receive_seq_num=*/0,
                                          /*tx_seq=*/0,
                                          /*is_poll_response=*/false,
                                          payload));

  RunUntilIdle();
  // Lease should be queued while RX packet queued.
  EXPECT_GE(lease_provider().lease_count(), 1u);

  // Activating should clear the rx packet queue, releasing lease.
  channel->Activate(NopRxCallback, DoNothing);
  EXPECT_TRUE(AllExpectedPacketsSent());
  EXPECT_EQ(lease_provider().lease_count(), 0u);

  // ERTM mode TX engine should queue the SDU until it is acknowledged,
  // resulting in lease acquisition.
  auto sdu = std::make_unique<StaticByteBuffer<1>>(0x01);
  EXPECT_ACL_PACKET_OUT_(testing::AclIFrame(kTestHandle1,
                                            channel->remote_id(),
                                            /*receive_seq_num=*/1,
                                            /*tx_seq=*/0,
                                            /*is_poll_response=*/false,
                                            StaticByteBuffer(0x01)),
                         kHighPriority);

  channel->Send(std::move(sdu));
  EXPECT_TRUE(AllExpectedPacketsSent());
  EXPECT_GE(lease_provider().lease_count(), 1u);

  // Acknowledging I-Frame should release lease.
  ReceiveAclDataPacket(testing::AclSFrameReceiverReady(kTestHandle1,
                                                       channel->id(),
                                                       /*receive_seq_num=*/1));
  RunUntilIdle();
  EXPECT_EQ(lease_provider().lease_count(), 0u);

  // Paused sending so the next PDU is queued.
  acl_data_channel()->set_sending_paused(true);

  // Poll request should trigger poll response PDU, which will be queued.
  ReceiveAclDataPacket(
      testing::AclSFrameReceiverReady(kTestHandle1,
                                      channel->id(),
                                      /*receive_seq_num=*/1,
                                      /*is_poll_request=*/true));
  EXPECT_GE(lease_provider().lease_count(), 1u);

  EXPECT_ACL_PACKET_OUT_(
      testing::AclSFrameReceiverReady(kTestHandle1,
                                      channel->remote_id(),
                                      /*receive_seq_num=*/1,
                                      /*is_poll_request=*/false,
                                      /*is_poll_response=*/true),
      kHighPriority);
  acl_data_channel()->set_sending_paused(false);
  EXPECT_EQ(lease_provider().lease_count(), 0u);
}

TEST_F(ChannelManagerMockAclChannelTest,
       CreditBasedConnectionQueuedSduLeaseAcquisitionAndRelease) {
  auto channels = chanmgr()->AddLEConnection(
      kTestHandle1,
      pw::bluetooth::emboss::ConnectionRole::CENTRAL,
      /*link_error_callback=*/[] {},
      /*conn_param_callback=*/[](auto&) {},
      /*security_callback=*/[](auto, auto, auto) {});

  static constexpr uint16_t kPsm = 0x015;
  static constexpr ChannelParameters kParams{
      .mode = CreditBasedFlowControlMode::kLeCreditBasedFlowControl,
      .max_rx_sdu_size = std::nullopt,
      .flush_timeout = std::nullopt,
  };

  const auto req =
      l2cap::testing::AclLeCreditBasedConnectionReq(1,
                                                    kTestHandle1,
                                                    kPsm,
                                                    kFirstDynamicChannelId,
                                                    kDefaultMTU,
                                                    kMaxInboundPduPayloadSize,
                                                    /*credits=*/0);
  EXPECT_ACL_PACKET_OUT_(req, kHighPriority);

  WeakPtr<Channel> channel;
  chanmgr()->OpenL2capChannel(
      kTestHandle1, kPsm, kParams, [&channel](auto result) {
        channel = std::move(result);
      });
  RunUntilIdle();

  ReceiveAclDataPacket(l2cap::testing::AclLeCreditBasedConnectionRsp(
      /*id=*/1,
      /*link_handle=*/kTestHandle1,
      /*cid=*/kFirstDynamicChannelId,
      /*mtu=*/64,
      /*mps=*/32,
      /*credits=*/0,
      /*result=*/LECreditBasedConnectionResult::kSuccess));
  ASSERT_TRUE(channel.is_alive());
  EXPECT_TRUE(AllExpectedPacketsSent());

  channel->Activate(NopRxCallback, DoNothing);

  EXPECT_EQ(lease_provider().lease_count(), 0u);
  auto sdu = std::make_unique<StaticByteBuffer<1>>(0x01);
  channel->Send(std::move(sdu));
  RunUntilIdle();
  EXPECT_GE(lease_provider().lease_count(), 1u);

  EXPECT_ACL_PACKET_OUT_(
      l2cap::testing::AclKFrame(
          kTestHandle1, channel->remote_id(), StaticByteBuffer(0x01)),
      kHighPriority);
  ReceiveAclDataPacket(l2cap::testing::AclFlowControlCreditInd(
      1, kTestHandle1, channel->remote_id(), /*credits=*/20));

  RunUntilIdle();
  EXPECT_TRUE(AllExpectedPacketsSent());
  EXPECT_EQ(lease_provider().lease_count(), 0u);

  StaticByteBuffer segment_0(
      // SDU Length
      2,
      0x00,
      // Payload
      0x01);
  StaticByteBuffer segment_1(
      // Payload
      0x02);

  ReceiveAclDataPacket(
      testing::AclBFrame(kTestHandle1, channel->id(), segment_0));
  // First segment should be queued.
  EXPECT_GE(lease_provider().lease_count(), 1u);

  ReceiveAclDataPacket(
      testing::AclBFrame(kTestHandle1, channel->id(), segment_1));
  EXPECT_EQ(lease_provider().lease_count(), 0u);
}

}  // namespace
}  // namespace bt::l2cap
