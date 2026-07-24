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

#include "pw_bluetooth_sapphire/internal/host/l2cap/le_dynamic_channel.h"

#include <memory>

#include "pw_async/fake_dispatcher_fixture.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/dynamic_channel_registry.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/fake_signaling_channel.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/l2cap_defs.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/signaling_channel.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/types.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"
#include "pw_unit_test/framework.h"

namespace bt::l2cap::internal {
namespace {
constexpr ChannelId DynamicCid(int16_t channel_number = 0) {
  return kFirstDynamicChannelId + channel_number;
}

auto LeConnReq(const LECreditBasedConnectionRequestPayload& payload) {
  return StaticByteBuffer(LowerBits(payload.le_psm),
                          UpperBits(payload.le_psm),
                          LowerBits(payload.src_cid),
                          UpperBits(payload.src_cid),
                          LowerBits(payload.mtu),
                          UpperBits(payload.mtu),
                          LowerBits(payload.mps),
                          UpperBits(payload.mps),
                          LowerBits(payload.initial_credits),
                          UpperBits(payload.initial_credits));
}

auto LeConnRsp(const LECreditBasedConnectionResponsePayload& payload) {
  return StaticByteBuffer(LowerBits(payload.dst_cid),
                          UpperBits(payload.dst_cid),
                          LowerBits(payload.mtu),
                          UpperBits(payload.mtu),
                          LowerBits(payload.mps),
                          UpperBits(payload.mps),
                          LowerBits(payload.initial_credits),
                          UpperBits(payload.initial_credits),
                          LowerBits(static_cast<uint16_t>(payload.result)),
                          UpperBits(static_cast<uint16_t>(payload.result)));
}

auto DisconnReq(const DisconnectionRequestPayload& payload) {
  return StaticByteBuffer(LowerBits(payload.dst_cid),
                          UpperBits(payload.dst_cid),
                          LowerBits(payload.src_cid),
                          UpperBits(payload.src_cid));
}

auto DisconnRsp(const DisconnectionResponsePayload& payload) {
  return StaticByteBuffer(LowerBits(payload.dst_cid),
                          UpperBits(payload.dst_cid),
                          LowerBits(payload.src_cid),
                          UpperBits(payload.src_cid));
}

class LeDynamicChannelTest : public pw::async::test::FakeDispatcherFixture {
 public:
  LeDynamicChannelTest() = default;
  ~LeDynamicChannelTest() override = default;

 protected:
  // Import types for brevity.
  using DynamicChannelCallback = DynamicChannelRegistry::DynamicChannelCallback;
  using ServiceRequestCallback = DynamicChannelRegistry::ServiceRequestCallback;

  // TestLoopFixture overrides
  void SetUp() override {
    channel_close_cb_ = nullptr;
    inbound_open_cb_ = nullptr;
    signaling_channel_ =
        std::make_unique<testing::FakeSignalingChannel>(dispatcher());

    registry_ = std::make_unique<LeDynamicChannelRegistry>(
        sig(),
        fit::bind_member<&LeDynamicChannelTest::OnChannelClose>(this),
        fit::bind_member<&LeDynamicChannelTest::OnServiceRequest>(this),
        /*random_channel_ids=*/false);
  }

  void TearDown() override {
    RunUntilIdle();
    services_.clear();
    registry_ = nullptr;
    signaling_channel_ = nullptr;
    inbound_open_cb_ = nullptr;
    channel_close_cb_ = nullptr;
  }

  const DynamicChannel* DoOpenOutbound(
      const LECreditBasedConnectionRequestPayload& request,
      const LECreditBasedConnectionResponsePayload& response,
      ChannelParameters params,
      SignalingChannel::Status response_status =
          SignalingChannel::Status::kSuccess) {
    auto req = LeConnReq(request);
    auto rsp = LeConnRsp(response);
    EXPECT_OUTBOUND_REQ(*sig(),
                        kLECreditBasedConnectionRequest,
                        req.view(),
                        {response_status, rsp.view()});

    auto channel = std::make_shared<std::optional<const DynamicChannel*>>();
    registry()->OpenOutbound(
        request.le_psm, params, [weak = std::weak_ptr(channel)](auto chan) {
          if (auto channel = weak.lock()) {
            *channel = chan;
          }
        });

    RunUntilIdle();
    if (HasFatalFailure()) {
      return nullptr;
    }

    // We should always get a result, whether it is a success or failure.
    EXPECT_TRUE(*channel);
    if (!*channel) {
      return nullptr;
    }

    return **channel;
  }

  bool DoOpenInbound(
      const LECreditBasedConnectionRequestPayload& request,
      const LECreditBasedConnectionResponsePayload& expected_response) {
    auto req = LeConnReq(request);
    auto rsp = LeConnRsp(expected_response);

    sig()->ReceiveExpect(
        kLECreditBasedConnectionRequest, req.view(), rsp.view());

    RunUntilIdle();
    if (HasFatalFailure()) {
      return false;
    }

    return true;
  }

  bool DoCloseOutbound(const DisconnectionRequestPayload& request,
                       const DisconnectionResponsePayload& response) {
    auto req = DisconnReq(request);
    auto rsp = DisconnRsp(response);
    EXPECT_OUTBOUND_REQ(*sig(),
                        kDisconnectionRequest,
                        req.view(),
                        {SignalingChannel::Status::kSuccess, rsp.view()});
    bool channel_close_cb_called = false;
    registry()->CloseChannel(request.src_cid,
                             [&] { channel_close_cb_called = true; });
    RunUntilIdle();
    if (HasFatalFailure()) {
      return false;
    }

    return channel_close_cb_called;
  }

  bool DoCloseOutbound(const DisconnectionRequestPayload& request) {
    DisconnectionResponsePayload response = {request.dst_cid, request.src_cid};
    return DoCloseOutbound(request, response);
  }

  testing::FakeSignalingChannel* sig() const {
    return signaling_channel_.get();
  }

  LeDynamicChannelRegistry* registry() const { return registry_.get(); }

  void set_channel_close_cb(DynamicChannelCallback close_cb) {
    channel_close_cb_ = std::move(close_cb);
  }

  bool RegisterService(Psm psm, ChannelParameters params) {
    auto [_, result] = services_.insert(
        {psm,
         DynamicChannelRegistry::ServiceInfo{
             params,
             fit::bind_member(this, &LeDynamicChannelTest::OnInboundChannel)}});
    return result;
  }

  void SetNextInboundChannel(std::optional<const DynamicChannel*>* out) {
    ASSERT_EQ(inbound_open_cb_, nullptr);
    inbound_open_cb_ = [out](const DynamicChannel* channel) { *out = channel; };
  }

 private:
  void OnChannelClose(const DynamicChannel* channel) {
    if (channel_close_cb_) {
      channel_close_cb_(channel);
    }
  }

  void OnInboundChannel(const DynamicChannel* channel) {
    if (inbound_open_cb_) {
      std::move(inbound_open_cb_)(channel);
    }
  }

  std::optional<DynamicChannelRegistry::ServiceInfo> OnServiceRequest(Psm psm) {
    auto iter = services_.find(psm);
    if (iter == services_.end()) {
      return std::nullopt;
    }

    return DynamicChannelRegistry::ServiceInfo(iter->second.channel_params,
                                               iter->second.channel_cb.share());
  }

  DynamicChannelCallback channel_close_cb_;
  DynamicChannelCallback inbound_open_cb_;
  std::unordered_map<Psm, DynamicChannelRegistry::ServiceInfo> services_;
  std::unique_ptr<testing::FakeSignalingChannel> signaling_channel_;
  std::unique_ptr<LeDynamicChannelRegistry> registry_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(LeDynamicChannelTest);
};

TEST_F(LeDynamicChannelTest, StateToString) {
  constexpr std::array<std::pair<LeDynamicChannel::State, const char*>, 4>
      kCases{{
          {{.exchanged_connection_request = false,
            .exchanged_connection_response = false,
            .exchanged_disconnect_request = false},
           "{exchanged_connection_request: false, "
           "exchanged_connection_response: false, "
           "exchanged_disconnect_request: false}"},
          {{.exchanged_connection_request = false,
            .exchanged_connection_response = false,
            .exchanged_disconnect_request = true},
           "{exchanged_connection_request: false, "
           "exchanged_connection_response: false, "
           "exchanged_disconnect_request: true}"},
          {{.exchanged_connection_request = false,
            .exchanged_connection_response = true,
            .exchanged_disconnect_request = false},
           "{exchanged_connection_request: false, "
           "exchanged_connection_response: true, "
           "exchanged_disconnect_request: false}"},
          {{.exchanged_connection_request = true,
            .exchanged_connection_response = false,
            .exchanged_disconnect_request = false},
           "{exchanged_connection_request: true, "
           "exchanged_connection_response: false, "
           "exchanged_disconnect_request: false}"},
      }};
  for (const auto& [state, expected] : kCases) {
    EXPECT_EQ(state.ToString(), expected);
  }
}

TEST_F(LeDynamicChannelTest, OpenOutboundDefaultParametersCloseOutbound) {
  static constexpr auto kMode =
      CreditBasedFlowControlMode::kLeCreditBasedFlowControl;
  static constexpr ChannelParameters kChannelParams{
      .mode = kMode,
      .max_rx_sdu_size = std::nullopt,
      .flush_timeout = std::nullopt,
  };
  static constexpr LECreditBasedConnectionRequestPayload kLeConnReqPayload{
      .le_psm = 0x0015,
      .src_cid = DynamicCid(),
      .mtu = kDefaultMTU,
      .mps = kMaxInboundPduPayloadSize,
      .initial_credits = kLocalRxCredits,
  };
  static constexpr LECreditBasedConnectionResponsePayload kLeConnRspPayload{
      .dst_cid = DynamicCid(),
      .mtu = 0x0064,
      .mps = 0x0032,
      .initial_credits = kLocalRxCredits,
      .result = LECreditBasedConnectionResult::kSuccess,
  };
  static constexpr DisconnectionRequestPayload kDisconnReqPayload{
      .dst_cid = kLeConnRspPayload.dst_cid,
      .src_cid = kLeConnReqPayload.src_cid,
  };

  auto chan =
      DoOpenOutbound(kLeConnReqPayload, kLeConnRspPayload, kChannelParams);
  ASSERT_TRUE(chan);
  EXPECT_TRUE(chan->IsOpen());
  EXPECT_TRUE(chan->IsConnected());
  EXPECT_EQ(kLeConnReqPayload.src_cid, chan->local_cid());
  EXPECT_EQ(kLeConnRspPayload.dst_cid, chan->remote_cid());

  ChannelInfo expected_info = ChannelInfo::MakeCreditBasedFlowControlMode(
      kMode,
      kDefaultMTU,
      kLeConnRspPayload.mtu,
      kLeConnRspPayload.mps,
      kLeConnRspPayload.initial_credits,
      kLocalRxCredits);
  ChannelInfo actual_info = chan->info();

  EXPECT_EQ(expected_info.mode, actual_info.mode);
  EXPECT_EQ(expected_info.max_rx_sdu_size, actual_info.max_rx_sdu_size);
  EXPECT_EQ(expected_info.max_tx_sdu_size, actual_info.max_tx_sdu_size);
  EXPECT_EQ(expected_info.n_frames_in_tx_window,
            actual_info.n_frames_in_tx_window);
  EXPECT_EQ(expected_info.max_transmissions, actual_info.max_transmissions);
  EXPECT_EQ(expected_info.max_tx_pdu_payload_size,
            actual_info.max_tx_pdu_payload_size);
  EXPECT_EQ(expected_info.psm, actual_info.psm);
  EXPECT_EQ(expected_info.flush_timeout, actual_info.flush_timeout);
  EXPECT_EQ(expected_info.remote_initial_credits,
            actual_info.remote_initial_credits);

  EXPECT_TRUE(DoCloseOutbound(kDisconnReqPayload));
}

TEST_F(LeDynamicChannelTest, OpenOutboundSpecificParametersCloseOutbound) {
  static constexpr auto kMode =
      CreditBasedFlowControlMode::kLeCreditBasedFlowControl;
  static constexpr ChannelParameters kChannelParams{
      .mode = kMode,
      .max_rx_sdu_size = 0x0023,
      .flush_timeout = std::nullopt,
  };
  static constexpr LECreditBasedConnectionRequestPayload kLeConnReqPayload{
      .le_psm = 0x0015,
      .src_cid = DynamicCid(),
      .mtu = kChannelParams.max_rx_sdu_size.value_or(0),
      .mps = kMaxInboundPduPayloadSize,
      .initial_credits = kLocalRxCredits,
  };
  static constexpr LECreditBasedConnectionResponsePayload kLeConnRspPayload{
      .dst_cid = DynamicCid(),
      .mtu = 0x0064,
      .mps = 0x0032,
      .initial_credits = kLocalRxCredits,
      .result = LECreditBasedConnectionResult::kSuccess,
  };
  static constexpr DisconnectionRequestPayload kDisconnReqPayload{
      .dst_cid = kLeConnRspPayload.dst_cid,
      .src_cid = kLeConnReqPayload.src_cid,
  };

  auto chan =
      DoOpenOutbound(kLeConnReqPayload, kLeConnRspPayload, kChannelParams);
  ASSERT_TRUE(chan);
  EXPECT_TRUE(chan->IsOpen());
  EXPECT_TRUE(chan->IsConnected());
  EXPECT_EQ(kLeConnReqPayload.src_cid, chan->local_cid());
  EXPECT_EQ(kLeConnRspPayload.dst_cid, chan->remote_cid());

  ChannelInfo expected_info = ChannelInfo::MakeCreditBasedFlowControlMode(
      kMode,
      *kChannelParams.max_rx_sdu_size,
      kLeConnRspPayload.mtu,
      kLeConnRspPayload.mps,
      kLeConnRspPayload.initial_credits,
      kLocalRxCredits);
  ChannelInfo actual_info = chan->info();

  EXPECT_EQ(expected_info.mode, actual_info.mode);
  EXPECT_EQ(expected_info.max_rx_sdu_size, actual_info.max_rx_sdu_size);
  EXPECT_EQ(expected_info.max_tx_sdu_size, actual_info.max_tx_sdu_size);
  EXPECT_EQ(expected_info.n_frames_in_tx_window,
            actual_info.n_frames_in_tx_window);
  EXPECT_EQ(expected_info.max_transmissions, actual_info.max_transmissions);
  EXPECT_EQ(expected_info.max_tx_pdu_payload_size,
            actual_info.max_tx_pdu_payload_size);
  EXPECT_EQ(expected_info.psm, actual_info.psm);
  EXPECT_EQ(expected_info.flush_timeout, actual_info.flush_timeout);
  EXPECT_EQ(expected_info.remote_initial_credits,
            actual_info.remote_initial_credits);

  EXPECT_TRUE(DoCloseOutbound(kDisconnReqPayload));
}

TEST_F(LeDynamicChannelTest, OpenOutboundBadChannel) {
  static constexpr auto kMode =
      CreditBasedFlowControlMode::kLeCreditBasedFlowControl;
  static constexpr ChannelParameters kChannelParams{
      .mode = kMode,
      .max_rx_sdu_size = std::nullopt,
      .flush_timeout = std::nullopt,
  };
  static constexpr LECreditBasedConnectionRequestPayload kLeConnReqPayload{
      .le_psm = 0x0015,
      .src_cid = DynamicCid(),
      .mtu = kDefaultMTU,
      .mps = kMaxInboundPduPayloadSize,
      .initial_credits = kLocalRxCredits,
  };
  static constexpr LECreditBasedConnectionResponsePayload kLeConnRspPayload{
      .dst_cid = DynamicCid(-1),
      .mtu = 0x0064,
      .mps = 0x0032,
      .initial_credits = kLocalRxCredits,
      .result = LECreditBasedConnectionResult::kSuccess,
  };

  auto chan =
      DoOpenOutbound(kLeConnReqPayload, kLeConnRspPayload, kChannelParams);
  EXPECT_FALSE(chan);
}

TEST_F(LeDynamicChannelTest, OpenOutboundRejected) {
  static constexpr auto kMode =
      CreditBasedFlowControlMode::kLeCreditBasedFlowControl;
  static constexpr ChannelParameters kChannelParams{
      .mode = kMode,
      .max_rx_sdu_size = std::nullopt,
      .flush_timeout = std::nullopt,
  };
  static constexpr LECreditBasedConnectionRequestPayload kLeConnReqPayload{
      .le_psm = 0x0015,
      .src_cid = DynamicCid(),
      .mtu = kDefaultMTU,
      .mps = kMaxInboundPduPayloadSize,
      .initial_credits = kLocalRxCredits,
  };
  static constexpr LECreditBasedConnectionResponsePayload kLeConnRspPayload{
      .dst_cid = DynamicCid(),
      .mtu = 0x0064,
      .mps = 0x0032,
      .initial_credits = kLocalRxCredits,
      .result = LECreditBasedConnectionResult::kSuccess,
  };

  auto chan = DoOpenOutbound(kLeConnReqPayload,
                             kLeConnRspPayload,
                             kChannelParams,
                             SignalingChannel::Status::kReject);
  EXPECT_FALSE(chan);
}

TEST_F(LeDynamicChannelTest, OpenOutboundPsmNotSupported) {
  static constexpr auto kMode =
      CreditBasedFlowControlMode::kLeCreditBasedFlowControl;
  static constexpr ChannelParameters kChannelParams{
      .mode = kMode,
      .max_rx_sdu_size = std::nullopt,
      .flush_timeout = std::nullopt,
  };
  static constexpr LECreditBasedConnectionRequestPayload kLeConnReqPayload{
      .le_psm = 0x0015,
      .src_cid = DynamicCid(),
      .mtu = kDefaultMTU,
      .mps = kMaxInboundPduPayloadSize,
      .initial_credits = kLocalRxCredits,
  };
  static constexpr LECreditBasedConnectionResponsePayload kLeConnRspPayload{
      .dst_cid = DynamicCid(),
      .mtu = 0x0064,
      .mps = 0x0032,
      .initial_credits = kLocalRxCredits,
      .result = LECreditBasedConnectionResult::kPsmNotSupported,
  };

  auto chan =
      DoOpenOutbound(kLeConnReqPayload, kLeConnRspPayload, kChannelParams);
  EXPECT_FALSE(chan);
}

TEST_F(LeDynamicChannelTest, OpenInboundDefaultParamsCloseInbound) {
  static constexpr auto kMode =
      CreditBasedFlowControlMode::kLeCreditBasedFlowControl;
  static constexpr Psm kPsm = 0x0015;
  static constexpr ChannelParameters kChannelParams{
      .mode = kMode,
      .max_rx_sdu_size = std::nullopt,
      .flush_timeout = std::nullopt,
  };
  static constexpr LECreditBasedConnectionRequestPayload kLeConnReqPayload{
      .le_psm = kPsm,
      .src_cid = DynamicCid(),
      .mtu = 0x0064,
      .mps = 0x0032,
      .initial_credits = 0x0032,
  };
  static constexpr LECreditBasedConnectionResponsePayload kLeConnRspPayload{
      .dst_cid = DynamicCid(),
      .mtu = kDefaultMTU,
      .mps = kMaxInboundPduPayloadSize,
      .initial_credits = kLocalRxCredits,
      .result = LECreditBasedConnectionResult::kSuccess,
  };
  static constexpr DisconnectionRequestPayload kDisconnReqPayload{
      .dst_cid = kLeConnRspPayload.dst_cid,
      .src_cid = kLeConnReqPayload.src_cid,
  };

  std::optional<const DynamicChannel*> channel;
  SetNextInboundChannel(&channel);
  ASSERT_TRUE(RegisterService(kPsm, kChannelParams));
  EXPECT_TRUE(DoOpenInbound(kLeConnReqPayload, kLeConnRspPayload));
  ASSERT_TRUE(channel);
  ASSERT_TRUE(*channel);
  EXPECT_TRUE((*channel)->IsOpen());
  EXPECT_TRUE((*channel)->IsConnected());
  EXPECT_EQ(kLeConnReqPayload.src_cid, (*channel)->remote_cid());
  EXPECT_EQ(kLeConnRspPayload.dst_cid, (*channel)->local_cid());

  ChannelInfo expected_info = ChannelInfo::MakeCreditBasedFlowControlMode(
      kMode,
      kLeConnRspPayload.mtu,
      kLeConnReqPayload.mtu,
      kLeConnReqPayload.mps,
      kLeConnReqPayload.initial_credits,
      kLocalRxCredits);
  ChannelInfo actual_info = (*channel)->info();

  EXPECT_EQ(expected_info.mode, actual_info.mode);
  EXPECT_EQ(expected_info.max_rx_sdu_size, actual_info.max_rx_sdu_size);
  EXPECT_EQ(expected_info.max_tx_sdu_size, actual_info.max_tx_sdu_size);
  EXPECT_EQ(expected_info.n_frames_in_tx_window,
            actual_info.n_frames_in_tx_window);
  EXPECT_EQ(expected_info.max_transmissions, actual_info.max_transmissions);
  EXPECT_EQ(expected_info.max_tx_pdu_payload_size,
            actual_info.max_tx_pdu_payload_size);
  EXPECT_EQ(expected_info.psm, actual_info.psm);
  EXPECT_EQ(expected_info.flush_timeout, actual_info.flush_timeout);
  EXPECT_EQ(expected_info.remote_initial_credits,
            actual_info.remote_initial_credits);

  EXPECT_TRUE(DoCloseOutbound(kDisconnReqPayload));
}

TEST_F(LeDynamicChannelTest, OpenInboundUnsupportedPsm) {
  static constexpr Psm kPsm = 0x0015;
  static constexpr LECreditBasedConnectionRequestPayload kLeConnReqPayload{
      .le_psm = kPsm,
      .src_cid = DynamicCid(),
      .mtu = 0x0064,
      .mps = 0x0032,
      .initial_credits = 0x0032,
  };
  static constexpr LECreditBasedConnectionResponsePayload kLeConnRspPayload{
      .dst_cid = kInvalidChannelId,
      .mtu = 0,
      .mps = 0,
      .initial_credits = 0,
      .result = LECreditBasedConnectionResult::kPsmNotSupported,
  };

  std::optional<const DynamicChannel*> channel;
  SetNextInboundChannel(&channel);
  EXPECT_TRUE(DoOpenInbound(kLeConnReqPayload, kLeConnRspPayload));
  EXPECT_FALSE(channel);
}

TEST_F(LeDynamicChannelTest, OpenInboundBadChannel) {
  static constexpr LECreditBasedConnectionRequestPayload kLeConnReqPayload{
      .le_psm = 0x0015,
      .src_cid = DynamicCid(-1),
      .mtu = 0x0064,
      .mps = 0x0032,
      .initial_credits = kLocalRxCredits,
  };
  static constexpr LECreditBasedConnectionResponsePayload kLeConnRspPayload{
      .dst_cid = kInvalidChannelId,
      .mtu = 0,
      .mps = 0,
      .initial_credits = 0,
      .result = LECreditBasedConnectionResult::kInvalidSourceCID,
  };

  std::optional<const DynamicChannel*> channel;
  SetNextInboundChannel(&channel);
  EXPECT_TRUE(DoOpenInbound(kLeConnReqPayload, kLeConnRspPayload));
  EXPECT_FALSE(channel);
}

TEST_F(LeDynamicChannelTest, OpenInboundUnacceptableParameters) {
  static constexpr auto kMode =
      CreditBasedFlowControlMode::kLeCreditBasedFlowControl;
  static constexpr Psm kPsm = 0x0015;
  static constexpr ChannelParameters kChannelParams{
      .mode = kMode,
      .max_rx_sdu_size = std::nullopt,
      .flush_timeout = std::nullopt,
  };

  struct {
    uint16_t mtu;
    uint16_t mps;
  } kInvalidParams[] = {
      {0, 50},       // MTU too small
      {22, 50},      // MTU too small
      {100, 0},      // MPS too small
      {100, 22},     // MPS too small
      {100, 65534},  // MPS too large
  };

  ASSERT_TRUE(RegisterService(kPsm, kChannelParams));

  std::optional<const DynamicChannel*> channel;
  SetNextInboundChannel(&channel);

  for (const auto& params : kInvalidParams) {
    LECreditBasedConnectionRequestPayload req_payload{
        .le_psm = kPsm,
        .src_cid = DynamicCid(),
        .mtu = params.mtu,
        .mps = params.mps,
        .initial_credits = 0x0032,
    };
    LECreditBasedConnectionResponsePayload rsp_payload{
        .dst_cid = kInvalidChannelId,
        .mtu = 0,
        .mps = 0,
        .initial_credits = 0,
        .result = LECreditBasedConnectionResult::kUnacceptableParameters,
    };

    EXPECT_TRUE(DoOpenInbound(req_payload, rsp_payload));
    EXPECT_FALSE(channel);
  }
}

TEST_F(LeDynamicChannelTest, OpenOutboundUnacceptableParameters) {
  static constexpr auto kMode =
      CreditBasedFlowControlMode::kLeCreditBasedFlowControl;
  static constexpr ChannelParameters kChannelParams{
      .mode = kMode,
      .max_rx_sdu_size = std::nullopt,
      .flush_timeout = std::nullopt,
  };

  struct {
    uint16_t mtu;
    uint16_t mps;
  } kInvalidParams[] = {
      {0, 50},       // MTU too small
      {22, 50},      // MTU too small
      {100, 0},      // MPS too small
      {100, 22},     // MPS too small
      {100, 65534},  // MPS too large
  };

  for (const auto& params : kInvalidParams) {
    LECreditBasedConnectionRequestPayload req_payload{
        .le_psm = 0x0015,
        .src_cid = DynamicCid(),
        .mtu = kDefaultMTU,
        .mps = kMaxInboundPduPayloadSize,
        .initial_credits = kLocalRxCredits,
    };
    LECreditBasedConnectionResponsePayload rsp_payload{
        .dst_cid = DynamicCid(),
        .mtu = params.mtu,
        .mps = params.mps,
        .initial_credits = 0x0050,
        .result = LECreditBasedConnectionResult::kSuccess,
    };

    auto chan = DoOpenOutbound(req_payload, rsp_payload, kChannelParams);
    EXPECT_FALSE(chan);
  }
}

}  // namespace
}  // namespace bt::l2cap::internal
