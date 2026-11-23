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

// inclusive-language: disable

#include "pw_bluetooth_sapphire/internal/host/sm/phase_1.h"

#include <memory>

#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_bluetooth_sapphire/internal/host/hci/connection.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/l2cap_defs.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/mock_channel_test.h"
#include "pw_bluetooth_sapphire/internal/host/sm/fake_phase_listener.h"
#include "pw_bluetooth_sapphire/internal/host/sm/packet.h"
#include "pw_bluetooth_sapphire/internal/host/sm/smp.h"
#include "pw_bluetooth_sapphire/internal/host/sm/types.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"
#include "pw_unit_test/framework.h"

namespace bt::sm {
namespace {

struct Phase1Args {
  PairingRequestParams preq = PairingRequestParams();
  IOCapability io_capability = IOCapability::kNoInputNoOutput;
  BondableMode bondable_mode = BondableMode::Bondable;
  SecurityLevel level = SecurityLevel::kEncrypted;
  bool sc_supported = false;
};

class Phase1Test : public l2cap::testing::MockChannelTest {
 public:
  Phase1Test() = default;
  ~Phase1Test() override = default;

 protected:
  void SetUp() override { NewPhase1(); }

  void TearDown() override { phase_1_ = nullptr; }

  void NewPhase1(Role role = Role::kInitiator,
                 Phase1Args phase_args = Phase1Args(),
                 bt::LinkType ll_type = bt::LinkType::kLE) {
    l2cap::ChannelId cid = ll_type == bt::LinkType::kLE ? l2cap::kLESMPChannelId
                                                        : l2cap::kSMPChannelId;
    uint16_t mtu =
        phase_args.sc_supported ? l2cap::kMaxMTU : kNoSecureConnectionsMtu;
    ChannelOptions options(cid, mtu);
    options.link_type = ll_type;

    listener_ = std::make_unique<FakeListener>();
    l2cap::testing::FakeChannel::WeakPtr fake_chan = CreateFakeChannel(options);
    sm_chan_ = std::make_unique<PairingChannel>(fake_chan->GetWeakPtr());
    auto complete_cb = [this](PairingFeatures features,
                              PairingRequestParams preq,
                              PairingResponseParams pres) {
      feature_exchange_count_++;
      features_ = features;
      last_pairing_req_ = util::NewPdu(sizeof(PairingRequestParams));
      last_pairing_res_ = util::NewPdu(sizeof(PairingResponseParams));
      PacketWriter preq_writer(kPairingRequest, last_pairing_req_.get());
      PacketWriter pres_writer(kPairingResponse, last_pairing_res_.get());
      *preq_writer.mutable_payload<PairingRequestParams>() = preq;
      *pres_writer.mutable_payload<PairingResponseParams>() = pres;
    };
    if (role == Role::kInitiator) {
      phase_1_ = Phase1::CreatePhase1Initiator(sm_chan_->GetWeakPtr(),
                                               listener_->as_weak_ptr(),
                                               phase_args.io_capability,
                                               phase_args.bondable_mode,
                                               phase_args.level,
                                               std::move(complete_cb));
    } else {
      phase_1_ = Phase1::CreatePhase1Responder(sm_chan_->GetWeakPtr(),
                                               listener_->as_weak_ptr(),
                                               phase_args.preq,
                                               phase_args.io_capability,
                                               phase_args.bondable_mode,
                                               phase_args.level,
                                               std::move(complete_cb));
    }
  }

  Phase1* phase_1() { return phase_1_.get(); }
  FakeListener* listener() { return listener_.get(); }

  int feature_exchange_count() { return feature_exchange_count_; }
  PairingFeatures features() { return features_; }
  ByteBuffer* last_preq() { return last_pairing_req_.get(); }
  ByteBuffer* last_pres() { return last_pairing_res_.get(); }

 private:
  std::unique_ptr<FakeListener> listener_;
  std::unique_ptr<PairingChannel> sm_chan_;
  std::unique_ptr<Phase1> phase_1_;

  int feature_exchange_count_ = 0;
  PairingFeatures features_;
  MutableByteBufferPtr last_pairing_req_;
  MutableByteBufferPtr last_pairing_res_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(Phase1Test);
};

TEST_F(Phase1Test, FeatureExchangeStartDefaultParams) {
  const StaticByteBuffer kRequest(
      0x01,  // code: "Pairing Request"
      0x03,  // IO cap.: NoInputNoOutput
      0x00,  // OOB: not present
      AuthReq::kBondingFlag | AuthReq::kCT2,
      0x10,                 // encr. key size: 16 (default max)
      KeyDistGen::kEncKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey  // responder keys
  );
  EXPECT_PACKET_OUT(kRequest);
  phase_1()->Start();
}

TEST_F(Phase1Test, FeatureExchangeStartCustomParams) {
  auto phase_args = Phase1Args{.io_capability = IOCapability::kDisplayYesNo,
                               .bondable_mode = BondableMode::NonBondable,
                               .level = SecurityLevel::kAuthenticated,
                               .sc_supported = true};
  NewPhase1(Role::kInitiator, phase_args);

  const StaticByteBuffer kRequest(
      0x01,  // code: "Pairing Request"
      0x01,  // IO cap.: DisplayYesNo
      0x00,  // OOB: not present
      AuthReq::kMITM | AuthReq::kSC | AuthReq::kCT2,
      0x10,  // encr. key size: 16 (default max)
      0x00,  // initiator keys: none - non-bondable mode
      0x00   // responder keys: none - non-bondable mode
  );
  EXPECT_PACKET_OUT(kRequest);
  phase_1()->Start();
}

TEST_F(Phase1Test, FeatureExchangeInitiatorWithIdentityInfo) {
  listener()->set_identity_info(IdentityInfo());

  const StaticByteBuffer kRequest(
      0x01,  // code: "Pairing Request"
      0x03,  // IO cap.: NoInputNoOutput
      0x00,  // OOB: not present
      AuthReq::kBondingFlag | AuthReq::kCT2,
      0x10,  // encr. key size: 16 (default max)
      KeyDistGen::kEncKey | KeyDistGen::kIdKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey   // responder keys
  );

  EXPECT_PACKET_OUT(kRequest);
  phase_1()->Start();
  EXPECT_EQ(1, listener()->identity_info_count());
}

TEST_F(Phase1Test, FeatureExchangePairingFailed) {
  fake_chan()->Receive(StaticByteBuffer(0x05,  // code: Pairing Failed
                                        0x05   // reason: Pairing Not Supported
                                        ));
  RunUntilIdle();

  EXPECT_EQ(Error(ErrorCode::kPairingNotSupported), listener()->last_error());
  EXPECT_EQ(1, listener()->pairing_error_count());
}

TEST_F(Phase1Test, FeatureExchangeLocalRejectsUnsupportedInitiatorKeys) {
  const auto kRequest = StaticByteBuffer(
      0x01,  // code: Pairing Request
      0x03,  // IO cap.: NoInputNoOutput
      0x00,  // OOB: not present
      AuthReq::kBondingFlag | AuthReq::kCT2,
      0x10,                 // encr. key size: 16 (default max)
      KeyDistGen::kEncKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey  // responder keys
  );
  const auto kResponse = StaticByteBuffer(
      0x02,  // code: Pairing Response
      0x00,  // IO cap.: DisplayOnly
      0x00,  // OOB: not present
      0x00,
      0x07,                // encr. key size: 7 (default min)
      KeyDistGen::kIdKey,  // initiator keys - not listed in kRequest
      KeyDistGen::kEncKey | KeyDistGen::kIdKey  // responder keys
  );
  const StaticByteBuffer kFailure(0x05,  // code: Pairing Failed
                                  0x0A   // reason: Invalid Parameters
  );
  EXPECT_PACKET_OUT(kRequest);
  phase_1()->Start();

  // We should receive a pairing response and reply back with Pairing Failed.
  EXPECT_PACKET_OUT(kFailure);
  fake_chan()->Receive(kResponse);
  RunUntilIdle();
  EXPECT_TRUE(AllExpectedPacketsSent());

  EXPECT_EQ(1, listener()->pairing_error_count());
  EXPECT_EQ(Error(ErrorCode::kInvalidParameters), listener()->last_error());
  EXPECT_EQ(0, feature_exchange_count());
}

TEST_F(Phase1Test, FeatureExchangeLocalRejectsUnsupportedResponderKeys) {
  const auto kRequest = StaticByteBuffer(
      0x01,  // code: Pairing Request
      0x03,  // IO cap.: NoInputNoOutput
      0x00,  // OOB: not present
      AuthReq::kBondingFlag | AuthReq::kCT2,
      0x10,                 // encr. key size: 16 (default max)
      KeyDistGen::kEncKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey  // responder keys
  );
  const auto kResponse = StaticByteBuffer(
      0x02,  // code: Pairing Response
      0x00,  // IO cap.: DisplayOnly
      0x00,  // OOB: not present
      0x00,
      0x07,                 // encr. key size: 7 (default min)
      KeyDistGen::kEncKey,  // initiator keys
      KeyDistGen::kSignKey  // responder keys - kSignKey not in kRequest
  );
  const StaticByteBuffer kFailure(0x05,  // code: Pairing Failed
                                  0x0A   // reason: Invalid Parameters
  );

  EXPECT_PACKET_OUT(kRequest);
  phase_1()->Start();
  RunUntilIdle();
  ASSERT_TRUE(AllExpectedPacketsSent());

  // We should receive a pairing response and reply back with Pairing Failed.
  EXPECT_PACKET_OUT(kFailure);
  fake_chan()->Receive(kResponse);
  RunUntilIdle();
  ASSERT_TRUE(AllExpectedPacketsSent());

  EXPECT_EQ(1, listener()->pairing_error_count());
  EXPECT_EQ(Error(ErrorCode::kInvalidParameters), listener()->last_error());
  EXPECT_EQ(0, feature_exchange_count());
}

// Pairing should fail if MITM is required but the I/O capabilities cannot
// provide it
TEST_F(Phase1Test, FeatureExchangeFailureAuthenticationRequirements) {
  const auto kRequest = StaticByteBuffer(
      0x01,  // code: Pairing Request
      0x03,  // IO cap.: NoInputNoOutput
      0x00,  // OOB: not present
      AuthReq::kBondingFlag | AuthReq::kCT2,
      0x10,                 // encr. key size: 16 (default max)
      KeyDistGen::kEncKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey  // responder keys
  );
  const StaticByteBuffer kResponse(0x02,  // code: Pairing Response
                                   0x00,  // IO cap.: DisplayOnly
                                   0x00,  // OOB: not present
                                   AuthReq::kMITM,
                                   0x07,  // encr. key size: 7 (default min)
                                   0x00,  // initiator keys: none
                                   KeyDistGen::kEncKey  // responder keys: it's
                                                        // OK to use fewer keys
                                                        // than in kRequest
  );
  const StaticByteBuffer kFailure(0x05,  // code: Pairing Failed
                                  0x03   // reason: Authentication requirements
  );

  EXPECT_PACKET_OUT(kRequest);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  // We should receive a pairing response and reply back with Pairing Failed.
  EXPECT_PACKET_OUT(kFailure);
  fake_chan()->Receive(kResponse);
  RunUntilIdle();
  EXPECT_TRUE(AllExpectedPacketsSent());

  EXPECT_EQ(1, listener()->pairing_error_count());
  EXPECT_EQ(Error(ErrorCode::kAuthenticationRequirements),
            listener()->last_error());
  EXPECT_EQ(0, feature_exchange_count());
}

TEST_F(Phase1Test, FeatureExchangeFailureMalformedRequest) {
  const auto kMalformedResponse = StaticByteBuffer(
      0x02,                // code: Pairing Response
      0x03,                // IO cap.: NoInputNoOutput
      0x00,                // OOB: not present
      0x00,                // AuthReq: empty
      0x10,                // encr. key size: 16 (default max)
      KeyDistGen::kEncKey  // initiator key dist.: encr. key only
                           // Missing last byte, responder key dist.
  );
  const StaticByteBuffer kFailure(0x05,  // code: Pairing Failed
                                  0x0A   // reason: Invalid Parameters
  );

  EXPECT_PACKET_OUT(kFailure);
  fake_chan()->Receive(kMalformedResponse);
  RunUntilIdle();
  EXPECT_TRUE(AllExpectedPacketsSent());

  EXPECT_EQ(1, listener()->pairing_error_count());
  EXPECT_EQ(Error(ErrorCode::kInvalidParameters), listener()->last_error());
}

TEST_F(Phase1Test, FeatureExchangeBothSupportSCFeaturesHaveSC) {
  Phase1Args args;
  args.sc_supported = true;
  NewPhase1(Role::kInitiator, args);
  const StaticByteBuffer kRequest(
      0x01,  // code: Pairing Request
      0x03,  // IO cap.: NoInputNoOutput
      0x00,  // OOB: not present
      AuthReq::kBondingFlag | AuthReq::kSC | AuthReq::kCT2,
      0x10,  // encr. key size: 16 (default max)
      KeyDistGen::kEncKey | KeyDistGen::kLinkKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey |
          KeyDistGen::kLinkKey  // responder keys
  );
  const StaticByteBuffer kResponse(0x02,  // code: Pairing Response
                                   0x00,  // IO cap.: DisplayOnly
                                   0x00,  // OOB: not present
                                   AuthReq::kBondingFlag | AuthReq::kSC,
                                   0x07,  // encr. key size: 7 (default min)
                                   0x00,  // initiator keys: none
                                   KeyDistGen::kEncKey  // responder keys
  );

  EXPECT_PACKET_OUT(kRequest);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  fake_chan()->Receive(kResponse);
  RunUntilIdle();

  EXPECT_EQ(0, listener()->pairing_error_count());
  EXPECT_EQ(1, feature_exchange_count());

  EXPECT_TRUE(features().initiator);
  EXPECT_TRUE(features().secure_connections);
  ASSERT_TRUE(last_preq());
  ASSERT_TRUE(last_pres());
  EXPECT_TRUE(ContainersEqual(kRequest, *last_preq()));
  EXPECT_TRUE(ContainersEqual(kResponse, *last_pres()));
}

TEST_F(Phase1Test, FeatureExchangeScIgnoresEncKeyBit) {
  Phase1Args args;
  args.sc_supported = true;
  NewPhase1(Role::kInitiator, args);
  const StaticByteBuffer kRequest(
      0x01,  // code: Pairing Request
      0x03,  // IO cap.: NoInputNoOutput
      0x00,  // OOB: not present
      AuthReq::kBondingFlag | AuthReq::kSC | AuthReq::kCT2,
      0x10,  // encr. key size: 16 (default max)
      KeyDistGen::kEncKey | KeyDistGen::kLinkKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey |
          KeyDistGen::kLinkKey  // responder keys
  );
  const auto kResponse =
      StaticByteBuffer(0x02,  // code: Pairing Response
                       0x00,  // IO cap.: DisplayOnly
                       0x00,  // OOB: not present
                       AuthReq::kBondingFlag | AuthReq::kSC,
                       0x07,                // encr. key size: 7 (default min)
                       0x00,                // initiator keys: none
                       KeyDistGen::kEncKey  // responder keys
      );

  EXPECT_PACKET_OUT(kRequest);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  fake_chan()->Receive(kResponse);
  RunUntilIdle();

  EXPECT_EQ(0, listener()->pairing_error_count());
  EXPECT_EQ(1, feature_exchange_count());

  EXPECT_TRUE(features().initiator);
  EXPECT_TRUE(features().secure_connections);
  // Even though both the pairing request and response had the EncKey bit set,
  // because we resolved the features to secure connections, we zero the bit
  // out.
  EXPECT_FALSE(features().peer_key_distribution & KeyDistGen::kEncKey);
  EXPECT_FALSE(features().peer_key_distribution & KeyDistGen::kEncKey);
  ASSERT_TRUE(last_preq());
  ASSERT_TRUE(last_pres());
  EXPECT_TRUE(ContainersEqual(kRequest, *last_preq()));
  EXPECT_TRUE(ContainersEqual(kResponse, *last_pres()));
}

TEST_F(Phase1Test, FeatureExchangeLocalSCPeerNoSCFeaturesNoSc) {
  Phase1Args args;
  args.sc_supported = true;
  NewPhase1(Role::kInitiator, args);
  const auto kRequest = StaticByteBuffer(
      0x01,  // code: Pairing Request
      0x03,  // IO cap.: NoInputNoOutput
      0x00,  // OOB: not present
      AuthReq::kBondingFlag | AuthReq::kSC | AuthReq::kCT2,
      0x10,  // encr. key size: 16 (default max)
      KeyDistGen::kEncKey | KeyDistGen::kLinkKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey |
          KeyDistGen::kLinkKey  // responder keys
  );
  const auto kResponse =
      StaticByteBuffer(0x02,  // code: Pairing Response
                       0x00,  // IO cap.: DisplayOnly
                       0x00,  // OOB: not present
                       AuthReq::kBondingFlag,
                       0x07,                // encr. key size: 7 (default min)
                       0x00,                // initiator keys: none
                       KeyDistGen::kEncKey  // responder keys
      );

  EXPECT_PACKET_OUT(kRequest);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  fake_chan()->Receive(kResponse);
  RunUntilIdle();

  EXPECT_EQ(0, listener()->pairing_error_count());
  EXPECT_EQ(1, feature_exchange_count());

  EXPECT_TRUE(features().initiator);
  EXPECT_FALSE(features().secure_connections);
  ASSERT_TRUE(last_preq());
  ASSERT_TRUE(last_pres());
  EXPECT_TRUE(ContainersEqual(kRequest, *last_preq()));
  EXPECT_TRUE(ContainersEqual(kResponse, *last_pres()));
}

TEST_F(Phase1Test, FeatureExchangePairingResponseLegacyJustWorks) {
  const auto kRequest = StaticByteBuffer(
      0x01,  // code: Pairing Request
      0x03,  // IO cap.: NoInputNoOutput
      0x00,  // OOB: not present
      AuthReq::kBondingFlag | AuthReq::kCT2,
      0x10,                 // encr. key size: 16 (default max)
      KeyDistGen::kEncKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey  // responder keys
  );
  const StaticByteBuffer kResponse(0x02,  // code: Pairing Response
                                   0x00,  // IO cap.: DisplayOnly
                                   0x00,  // OOB: not present
                                   AuthReq::kBondingFlag,
                                   0x07,  // encr. key size: 7 (default min)
                                   KeyDistGen::kEncKey,  // initiator keys
                                   KeyDistGen::kEncKey   // responder keys
  );

  EXPECT_PACKET_OUT(kRequest);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  fake_chan()->Receive(kResponse);
  RunUntilIdle();

  EXPECT_EQ(0, listener()->pairing_error_count());
  EXPECT_EQ(1, feature_exchange_count());

  EXPECT_TRUE(features().initiator);
  EXPECT_EQ(PairingMethod::kJustWorks, features().method);
  EXPECT_EQ(7, features().encryption_key_size);
  EXPECT_TRUE(KeyDistGen::kEncKey & features().local_key_distribution);
  EXPECT_TRUE(KeyDistGen::kEncKey & features().peer_key_distribution);
  ASSERT_TRUE(last_preq());
  ASSERT_TRUE(last_pres());
  EXPECT_TRUE(ContainersEqual(kRequest, *last_preq()));
  EXPECT_TRUE(ContainersEqual(kResponse, *last_pres()));
}

TEST_F(Phase1Test, FeatureExchangePairingResponseLegacyMITM) {
  auto phase_args = Phase1Args{.io_capability = IOCapability::kDisplayYesNo};
  NewPhase1(Role::kInitiator, phase_args);

  const auto kRequest = StaticByteBuffer(
      0x01,  // code: Pairing Request
      0x01,  // IO cap.: DisplayYesNo
      0x00,  // OOB: not present
      AuthReq::kBondingFlag | AuthReq::kCT2,
      0x10,                 // encr. key size: 16 (default max)
      KeyDistGen::kEncKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey  // responder keys
  );
  const StaticByteBuffer kResponse(0x02,  // code: Pairing Response
                                   0x02,  // IO cap.: KeyboardOnly
                                   0x00,  // OOB: not present
                                   AuthReq::kBondingFlag | AuthReq::kMITM,
                                   0x07,  // encr. key size: 7 (default min)
                                   0x00,  // initiator keys: none
                                   KeyDistGen::kEncKey  // responder keys
  );

  EXPECT_PACKET_OUT(kRequest);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  fake_chan()->Receive(kResponse);
  RunUntilIdle();

  EXPECT_EQ(0, listener()->pairing_error_count());
  EXPECT_EQ(1, feature_exchange_count());

  EXPECT_TRUE(features().initiator);
  EXPECT_FALSE(features().secure_connections);
  EXPECT_EQ(PairingMethod::kPasskeyEntryDisplay, features().method);
  EXPECT_EQ(7, features().encryption_key_size);
  EXPECT_FALSE(features().local_key_distribution);
  EXPECT_TRUE(KeyDistGen::kEncKey & features().peer_key_distribution);
  ASSERT_TRUE(last_preq() && last_pres());
  EXPECT_TRUE(ContainersEqual(kRequest, *last_preq()));
  EXPECT_TRUE(ContainersEqual(kResponse, *last_pres()));
}

TEST_F(Phase1Test, FeatureExchangeEncryptionKeySize) {
  const auto kRequest = StaticByteBuffer(
      0x01,  // code: Pairing Request
      0x03,  // IO cap.: NoInputNoOutput
      0x00,  // OOB: not present
      AuthReq::kBondingFlag | AuthReq::kCT2,
      0x10,                 // encr. key size: 16 (default max)
      KeyDistGen::kEncKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey  // responder keys
  );
  const StaticByteBuffer kResponse(0x02,  // code: Pairing Response
                                   0x00,  // IO cap.: DisplayOnly
                                   0x00,  // OOB: not present
                                   AuthReq::kMITM,
                                   0x02,  // encr. key size: 2 (too small)
                                   KeyDistGen::kEncKey,  // initiator keys
                                   KeyDistGen::kEncKey   // responder keys
  );
  const StaticByteBuffer kFailure(0x05,  // code: Pairing Failed
                                  0x06   // reason: Encryption Key Size
  );

  EXPECT_PACKET_OUT(kRequest);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  EXPECT_PACKET_OUT(kFailure);
  fake_chan()->Receive(kResponse);
  RunUntilIdle();
  ASSERT_TRUE(AllExpectedPacketsSent());

  EXPECT_EQ(0, feature_exchange_count());
  EXPECT_EQ(1, listener()->pairing_error_count());
  EXPECT_EQ(Error(ErrorCode::kEncryptionKeySize), listener()->last_error());
}

TEST_F(Phase1Test, FeatureExchangeSecureAuthenticatedEncryptionKeySize) {
  auto phase_args = Phase1Args{.io_capability = IOCapability::kKeyboardDisplay,
                               .level = SecurityLevel::kSecureAuthenticated,
                               .sc_supported = true};
  NewPhase1(Role::kInitiator, phase_args);
  const StaticByteBuffer kRequest(
      0x01,  // code: Pairing Request
      0x04,  // IO cap.: KeyboardDisplay
      0x00,  // OOB: not present
      AuthReq::kBondingFlag | AuthReq::kMITM | AuthReq::kSC | AuthReq::kCT2,
      0x10,  // encr. key size: 16 (default max)
      KeyDistGen::kEncKey | KeyDistGen::kLinkKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey |
          KeyDistGen::kLinkKey  // responder keys
  );
  const StaticByteBuffer kResponse(
      0x02,  // code: Pairing Response
      0x04,  // IO cap.: KeyboardDisplay
      0x00,  // OOB: not present
      AuthReq::kBondingFlag | AuthReq::kMITM | AuthReq::kSC,
      0x0F,                 // encr. key size: 15, i.e. one byte less than max
                            // possible encryption key size.
      KeyDistGen::kEncKey,  // initiator keys
      KeyDistGen::kEncKey   // responder keys
  );
  const StaticByteBuffer kFailure(kPairingFailed,
                                  ErrorCode::kEncryptionKeySize);

  EXPECT_PACKET_OUT(kRequest);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  // We should receive a pairing response and reply back with Pairing Failed; we
  // enforce that all encryption keys are 16 bytes when `level` is set to
  // SecureAuthenticated.
  EXPECT_PACKET_OUT(kFailure);
  fake_chan()->Receive(kResponse);
  RunUntilIdle();
  ASSERT_TRUE(AllExpectedPacketsSent());

  EXPECT_EQ(0, feature_exchange_count());
  EXPECT_EQ(1, listener()->pairing_error_count());
  EXPECT_EQ(Error(ErrorCode::kEncryptionKeySize), listener()->last_error());
}

TEST_F(Phase1Test, FeatureExchangeSecureConnectionsRequiredNotPresent) {
  auto phase_args = Phase1Args{.io_capability = IOCapability::kKeyboardDisplay,
                               .level = SecurityLevel::kSecureAuthenticated,
                               .sc_supported = true};
  NewPhase1(Role::kInitiator, phase_args);
  const StaticByteBuffer kRequest(
      0x01,  // code: Pairing Request
      0x04,  // IO cap.: KeyboardDisplay
      0x00,  // OOB: not present
      AuthReq::kBondingFlag | AuthReq::kMITM | AuthReq::kSC | AuthReq::kCT2,
      0x10,  // encr. key size: 16 (default max)
      KeyDistGen::kEncKey | KeyDistGen::kLinkKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey |
          KeyDistGen::kLinkKey  // responder keys
  );
  const StaticByteBuffer kResponse(0x02,  // code: Pairing Response
                                   0x04,  // IO cap.: KeyboardDisplay
                                   0x00,  // OOB: not present
                                   AuthReq::kBondingFlag | AuthReq::kMITM,
                                   0x10,  // encr. key size: 16 (default max)
                                   KeyDistGen::kEncKey,  // initiator keys
                                   KeyDistGen::kEncKey   // responder keys
  );
  const auto kFailure =
      StaticByteBuffer(kPairingFailed, ErrorCode::kAuthenticationRequirements);

  EXPECT_PACKET_OUT(kRequest);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  // We should receive a pairing response and reply back with Pairing Failed; we
  // enforce that Secure Connections is used when `level` is set to
  // SecureAuthenticated.
  EXPECT_PACKET_OUT(kFailure);
  fake_chan()->Receive(kResponse);
  RunUntilIdle();
  ASSERT_TRUE(AllExpectedPacketsSent());

  EXPECT_EQ(0, feature_exchange_count());
  EXPECT_EQ(1, listener()->pairing_error_count());
  EXPECT_EQ(Error(ErrorCode::kAuthenticationRequirements),
            listener()->last_error());
}

TEST_F(Phase1Test, FeatureExchangeBothSupportScLinkKeyAndCt2GenerateH7CtKey) {
  auto phase_args = Phase1Args{.sc_supported = true};
  NewPhase1(Role::kInitiator, phase_args);
  const StaticByteBuffer kRequest(
      0x01,  // code: Pairing Request
      IOCapability::kNoInputNoOutput,
      0x00,  // OOB: not present
      AuthReq::kBondingFlag | AuthReq::kSC | AuthReq::kCT2,
      0x10,  // encr. key size: 16 (default max)
      KeyDistGen::kEncKey | KeyDistGen::kLinkKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey |
          KeyDistGen::kLinkKey  // responder keys
  );
  const auto kResponse =
      StaticByteBuffer(0x02,  // code: Pairing Response
                       0x04,  // IO cap.: KeyboardDisplay
                       0x00,  // OOB: not present
                       AuthReq::kBondingFlag | AuthReq::kSC | AuthReq::kCT2,
                       0x10,  // encr. key size: 16 (default max)
                       KeyDistGen::kLinkKey,  // initiator keys
                       KeyDistGen::kLinkKey   // responder keys
      );

  EXPECT_PACKET_OUT(kRequest);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  fake_chan()->Receive(kResponse);
  RunUntilIdle();
  EXPECT_EQ(1, feature_exchange_count());
  ASSERT_TRUE(features().generate_ct_key.has_value());
  EXPECT_EQ(CrossTransportKeyAlgo::kUseH7, features().generate_ct_key);
}

TEST_F(Phase1Test, FeatureExchangePeerDoesntSupportCt2GenerateH6CtKey) {
  auto phase_args = Phase1Args{.sc_supported = true};
  NewPhase1(Role::kInitiator, phase_args);
  const StaticByteBuffer kRequest(
      0x01,  // code: Pairing Request
      IOCapability::kNoInputNoOutput,
      0x00,  // OOB: not present
      AuthReq::kBondingFlag | AuthReq::kSC | AuthReq::kCT2,
      0x10,  // encr. key size: 16 (default max)
      KeyDistGen::kEncKey | KeyDistGen::kLinkKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey |
          KeyDistGen::kLinkKey  // responder keys
  );
  const StaticByteBuffer kResponse(0x02,  // code: Pairing Response
                                   0x04,  // IO cap.: KeyboardDisplay
                                   0x00,  // OOB: not present
                                   AuthReq::kBondingFlag | AuthReq::kSC,
                                   0x10,  // encr. key size: 16 (default max)
                                   KeyDistGen::kLinkKey,  // initiator keys
                                   KeyDistGen::kLinkKey   // responder keys
  );

  EXPECT_PACKET_OUT(kRequest);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  fake_chan()->Receive(kResponse);
  RunUntilIdle();
  EXPECT_EQ(1, feature_exchange_count());
  ASSERT_TRUE(features().generate_ct_key.has_value());
  EXPECT_EQ(CrossTransportKeyAlgo::kUseH6, features().generate_ct_key);
}

TEST_F(Phase1Test, FeatureExchangePeerDoesntSupportScDoNotGenerateCtKey) {
  const auto kRequest = StaticByteBuffer(
      0x01,  // code: Pairing Request
      IOCapability::kNoInputNoOutput,
      0x00,  // OOB: not present
      AuthReq::kBondingFlag | AuthReq::kCT2,
      0x10,                 // encr. key size: 16 (default max)
      KeyDistGen::kEncKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey  // responder keys
  );
  // Although the peer supports CTKG through the hci_spec::LinkKey field and SC,
  // locally we do not support SC, so CTKG is not allowed (v5.2 Vol. 3 Part
  // H 3.6.1).
  const StaticByteBuffer kResponse(0x02,  // code: Pairing Response
                                   0x04,  // IO cap.: KeyboardDisplay
                                   0x00,  // OOB: not present
                                   AuthReq::kBondingFlag | AuthReq::kSC,
                                   0x10,  // encr. key size: 16 (default max)
                                   KeyDistGen::kEncKey,  // initiator keys
                                   KeyDistGen::kEncKey   // responder keys
  );

  EXPECT_PACKET_OUT(kRequest);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  fake_chan()->Receive(kResponse);
  RunUntilIdle();
  EXPECT_EQ(1, feature_exchange_count());
  EXPECT_FALSE(features().generate_ct_key.has_value());
}

TEST_F(Phase1Test,
       FeatureExchangePeerSupportsCt2ButNotLinkKeyDoNotGenerateCtKey) {
  auto phase_args = Phase1Args{.sc_supported = true};
  NewPhase1(Role::kInitiator, phase_args);
  const StaticByteBuffer kRequest(
      0x01,  // code: Pairing Request
      IOCapability::kNoInputNoOutput,
      0x00,  // OOB: not present
      AuthReq::kBondingFlag | AuthReq::kSC | AuthReq::kCT2,
      0x10,  // encr. key size: 16 (default max)
      KeyDistGen::kEncKey | KeyDistGen::kLinkKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey |
          KeyDistGen::kLinkKey  // responder keys
  );
  // The peer indicates support for the Link Key (CTKG) on only one of the
  // Initiator/Responder Key Dist./Gen. fields, as such we do not generate the
  // CT key.
  const StaticByteBuffer kResponse(0x02,  // code: Pairing Response
                                   0x04,  // IO cap.: KeyboardDisplay
                                   0x00,  // OOB: not present
                                   AuthReq::kBondingFlag | AuthReq::kSC | kCT2,
                                   0x10,  // encr. key size: 16 (default max)
                                   0x00,  // initiator keys - none
                                   0x00   // responder keys - none
  );

  EXPECT_PACKET_OUT(kRequest);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  fake_chan()->Receive(kResponse);
  RunUntilIdle();
  EXPECT_EQ(1, feature_exchange_count());
  EXPECT_FALSE(features().generate_ct_key.has_value());
}

TEST_F(Phase1Test,
       FeatureExchangePeerOnlyIndicatesOneLinkKeyDoNotGenerateCtKey) {
  auto phase_args = Phase1Args{.sc_supported = true};
  NewPhase1(Role::kInitiator, phase_args);
  const StaticByteBuffer kRequest(
      0x01,  // code: Pairing Request
      IOCapability::kNoInputNoOutput,
      0x00,  // OOB: not present
      AuthReq::kBondingFlag | AuthReq::kSC | AuthReq::kCT2,
      0x10,  // encr. key size: 16 (default max)
      KeyDistGen::kEncKey | KeyDistGen::kLinkKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey |
          KeyDistGen::kLinkKey  // responder keys
  );
  // The peer indicates support for the Link Key (CTKG) on only one of the
  // Initiator/Responder Key Dist./Gen. fields, as such we do not generate the
  // CT key.
  const StaticByteBuffer kResponse(0x02,  // code: Pairing Response
                                   0x04,  // IO cap.: KeyboardDisplay
                                   0x00,  // OOB: not present
                                   AuthReq::kBondingFlag | AuthReq::kSC,
                                   0x10,  // encr. key size: 16 (default max)
                                   KeyDistGen::kLinkKey,  // initiator keys
                                   0x00  // responder keys - none
  );

  EXPECT_PACKET_OUT(kRequest);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  fake_chan()->Receive(kResponse);
  RunUntilIdle();
  EXPECT_EQ(1, feature_exchange_count());
  EXPECT_FALSE(features().generate_ct_key.has_value());
}

TEST_F(Phase1Test, FeatureExchangeResponderErrorCentral) {
  const auto kRequest = StaticByteBuffer(
      0x01,                 // code: Pairing Request
      0x03,                 // IO cap.: NoInputNoOutput
      0x00,                 // OOB: not present
      0x00,                 // AuthReq: no auth. request by default
      0x10,                 // encr. key size: 16 (default max)
      KeyDistGen::kEncKey,  // initiator key dist.: encr. key only
      KeyDistGen::kEncKey   // responder key dist.: encr. key only
  );
  const StaticByteBuffer kFailure(0x05,  // code: Pairing Failed
                                  ErrorCode::kUnspecifiedReason);

  NewPhase1(Role::kInitiator);
  EXPECT_PACKET_OUT(kFailure);
  fake_chan()->Receive(kRequest);
  RunUntilIdle();
  ASSERT_TRUE(AllExpectedPacketsSent());
  EXPECT_EQ(1, listener()->pairing_error_count());
}

// Verify that Pairing Requests are rejected by Phase1 - these are handled
// elsewhere in our stack.
TEST_F(Phase1Test, Phase1ResponderRejectsPairingRequest) {
  const auto kRequest = StaticByteBuffer(
      0x01,                 // code: Pairing Request
      0x03,                 // IO cap.: NoInputNoOutput
      0x00,                 // OOB: not present
      0x00,                 // AuthReq: no auth. request by default
      0x10,                 // encr. key size: 16 (default max)
      KeyDistGen::kEncKey,  // initiator key dist.: encr. key only
      KeyDistGen::kEncKey   // responder key dist.: encr. key only
  );
  const StaticByteBuffer kFailure(0x05,  // code: Pairing Failed
                                  ErrorCode::kUnspecifiedReason);

  NewPhase1(Role::kResponder);
  EXPECT_PACKET_OUT(kFailure);
  fake_chan()->Receive(kRequest);
  RunUntilIdle();
  ASSERT_TRUE(AllExpectedPacketsSent());
  EXPECT_EQ(1, listener()->pairing_error_count());
}

TEST_F(Phase1Test, FeatureExchangeResponderBothSupportSCFeaturesHaveSC) {
  const auto kResponse =
      StaticByteBuffer(0x02,  // code: Pairing Response
                       0x03,  // IO cap.: NoInputNoOutput
                       0x00,  // OOB: not present
                       AuthReq::kBondingFlag | AuthReq::kSC | AuthReq::kCT2,
                       0x10,                // encr. key size: 7 (default min)
                       0x00,                // initiator keys: none
                       KeyDistGen::kEncKey  // responder keys
      );

  Phase1Args args{.preq =
                      PairingRequestParams{
                          .io_capability = IOCapability::kNoInputNoOutput,
                          .oob_data_flag = OOBDataFlag::kNotPresent,
                          .auth_req = AuthReq::kBondingFlag | AuthReq::kSC,
                          .max_encryption_key_size = 0x10,  // 16, default max
                          .initiator_key_dist_gen = 0x00,
                          .responder_key_dist_gen = 0x01  // enc key
                      },
                  .sc_supported = true};
  NewPhase1(Role::kResponder, args);
  EXPECT_PACKET_OUT(kResponse);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  RunUntilIdle();

  EXPECT_EQ(0, listener()->pairing_error_count());
  EXPECT_EQ(1, feature_exchange_count());

  EXPECT_FALSE(features().initiator);
  EXPECT_TRUE(features().secure_connections);
}

TEST_F(Phase1Test, FeatureExchangeResponderLocalSCPeerNoSCFeaturesNoSC) {
  const auto kResponse =
      StaticByteBuffer(0x02,  // code: Pairing Response
                       0x03,  // IO cap.: NoInputNoOutput
                       0x00,  // OOB: not present
                       AuthReq::kBondingFlag | AuthReq::kSC | AuthReq::kCT2,
                       0x10,                // encr. key size: 7 (default min)
                       0x00,                // initiator keys: none
                       KeyDistGen::kEncKey  // responder keys
      );

  Phase1Args args{
      .preq =
          PairingRequestParams{
              .io_capability = IOCapability::kNoInputNoOutput,
              .oob_data_flag = OOBDataFlag::kNotPresent,
              .auth_req = AuthReq::kBondingFlag,
              .max_encryption_key_size = 0x10,  // 16, default max
              .initiator_key_dist_gen = 0x00,
              .responder_key_dist_gen = KeyDistGen::kEncKey  // enc key
          },
      .sc_supported = true};
  NewPhase1(Role::kResponder, args);
  EXPECT_PACKET_OUT(kResponse);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  RunUntilIdle();

  EXPECT_EQ(0, listener()->pairing_error_count());
  EXPECT_EQ(1, feature_exchange_count());

  EXPECT_FALSE(features().initiator);
  EXPECT_FALSE(features().secure_connections);
}

// Tests that the local responder does not request keys that the initiator
// cannot distribute.
TEST_F(Phase1Test, FeatureExchangeLocalResponderDoesNotRequestUnsupportedKeys) {
  auto phase_args =
      Phase1Args{.preq = PairingRequestParams{
                     .io_capability = IOCapability::kNoInputNoOutput,
                     .oob_data_flag = OOBDataFlag::kNotPresent,
                     .auth_req = AuthReq::kBondingFlag,
                     .max_encryption_key_size = 16,
                     .initiator_key_dist_gen = 0x04,  // sign key only
                     .responder_key_dist_gen = 0x00   // none
                 }};
  const StaticByteBuffer kResponse(
      0x02,  // code: Pairing Response
      0x03,  // IO cap.: NoInputNoOutput
      0x00,  // OOB: not present
      AuthReq::kBondingFlag | AuthReq::kCT2,
      0x10,  // encr. key size: 16 (default max)
      0x00,  // initiator keys: none - we shouldn't request the SignKey
             // as we don't support it
      0x00   // responder keys: none
  );

  NewPhase1(Role::kResponder, phase_args);
  EXPECT_PACKET_OUT(kResponse);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  EXPECT_EQ(1, feature_exchange_count());
  EXPECT_FALSE(features().initiator);
  EXPECT_FALSE(features().local_key_distribution);
  EXPECT_FALSE(features().peer_key_distribution);
  EXPECT_TRUE(ContainersEqual(kResponse, *last_pres()));
}

// Tests that we (as the responder) request to distribute identity information
// if available.
TEST_F(Phase1Test, FeatureExchangeResponderDistributesIdKey) {
  auto phase_args =
      Phase1Args{.preq = PairingRequestParams{
                     .io_capability = IOCapability::kNoInputNoOutput,
                     .oob_data_flag = OOBDataFlag::kNotPresent,
                     .auth_req = 0x01,                 // bondable mode
                     .max_encryption_key_size = 0x10,  // 16, default max
                     .initiator_key_dist_gen = 0x00,
                     .responder_key_dist_gen = KeyDistGen::kIdKey}};
  const StaticByteBuffer kResponse(0x02,  // code: Pairing Response
                                   0x03,  // IO cap.: NoInputNoOutput
                                   0x00,  // OOB: not present
                                   AuthReq::kBondingFlag | AuthReq::kCT2,
                                   0x10,  // encr. key size: 16 (default max)
                                   0x00,  // initiator keys: none
                                   KeyDistGen::kIdKey  // responder keys
  );

  NewPhase1(Role::kResponder, phase_args);
  listener()->set_identity_info(IdentityInfo());
  EXPECT_PACKET_OUT(kResponse);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  EXPECT_EQ(1, feature_exchange_count());
  EXPECT_FALSE(features().initiator);
  EXPECT_TRUE(features().local_key_distribution & KeyDistGen::kIdKey);
  EXPECT_FALSE(features().peer_key_distribution);
}

// Tests that local responder doesn't respond with distribute ID info if
// available but not requested by the initiator.
TEST_F(Phase1Test, FeatureExchangeResponderRespectsInitiatorForIdKey) {
  auto phase_args =
      Phase1Args{.preq = PairingRequestParams{
                     .io_capability = IOCapability::kNoInputNoOutput,
                     .oob_data_flag = OOBDataFlag::kNotPresent,
                     .auth_req = 0x01,                 // bondable mode
                     .max_encryption_key_size = 0x10,  // 16, default max
                     .initiator_key_dist_gen = 0x00,
                     .responder_key_dist_gen =
                         0x00  // Initiator explicitly does not request ID key
                 }};
  const StaticByteBuffer kResponse(
      0x02,  // code: Pairing Response
      0x03,  // IO cap.: NoInputNoOutput
      0x00,  // OOB: not present
      AuthReq::kBondingFlag | AuthReq::kCT2,
      0x10,  // encr. key size: 16 (default max)
      0x00,  // initiator keys: none
      0x00   // responder keys: none - we shouldn't distribute IdKey
             // even though we have it
  );

  NewPhase1(Role::kResponder, phase_args);
  listener()->set_identity_info(IdentityInfo());
  EXPECT_PACKET_OUT(kResponse);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  EXPECT_EQ(1, feature_exchange_count());
  EXPECT_FALSE(features().initiator);
  EXPECT_FALSE(features().local_key_distribution);
  EXPECT_FALSE(features().peer_key_distribution);
}

// Pairing should fail if MITM is required but the pairing method cannot provide
// it (due to I/O capabilities).
TEST_F(Phase1Test, FeatureExchangeResponderFailedAuthenticationRequirements) {
  const auto kRequest = StaticByteBuffer(
      0x01,  // code: Pairing Response
      0x00,  // IO cap.: DisplayOnly
      0x00,  // OOB: not present
      AuthReq::kMITM,
      0x07,                 // encr. key size: 7 (default min)
      KeyDistGen::kEncKey,  // initiator key dist.: encr. key only
      KeyDistGen::kEncKey   // responder key dist.: encr. key only
  );
  const StaticByteBuffer kFailure(0x05,  // code: Pairing Failed
                                  0x03   // reason: Authentication requirements
  );
  auto reader = PacketReader(&kRequest);
  auto phase_args = Phase1Args{.preq = reader.payload<PairingRequestParams>(),
                               .io_capability = IOCapability::kNoInputNoOutput};
  NewPhase1(Role::kResponder, phase_args);

  EXPECT_PACKET_OUT(kFailure);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());
  EXPECT_EQ(1, listener()->pairing_error_count());
  EXPECT_EQ(Error(ErrorCode::kAuthenticationRequirements),
            listener()->last_error());
}

TEST_F(Phase1Test, FeatureExchangeResponderJustWorks) {
  const auto kRequest = StaticByteBuffer(
      0x01,  // code: Pairing Response
      0x00,  // IO cap.: DisplayOnly
      0x00,  // OOB: not present
      AuthReq::kBondingFlag,
      0x07,                // encr. key size: 7 (default min)
      KeyDistGen::kIdKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey  // responder keys
  );
  const StaticByteBuffer kResponse(0x02,  // code: Pairing Response
                                   0x03,  // IO cap.: NoInputNoOutput
                                   0x00,  // OOB: not present
                                   AuthReq::kBondingFlag | AuthReq::kCT2,
                                   0x10,  // encr. key size: 16 (default max)
                                   KeyDistGen::kIdKey,  // initiator keys
                                   KeyDistGen::kEncKey  // responder keys
  );

  auto reader = PacketReader(&kRequest);
  auto phase_args = Phase1Args{.preq = reader.payload<PairingRequestParams>(),
                               .io_capability = IOCapability::kNoInputNoOutput};

  NewPhase1(Role::kResponder, phase_args);
  EXPECT_PACKET_OUT(kResponse);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  EXPECT_EQ(0, listener()->pairing_error_count());
  EXPECT_EQ(1, feature_exchange_count());
  EXPECT_FALSE(features().initiator);
  EXPECT_FALSE(features().secure_connections);
  EXPECT_EQ(PairingMethod::kJustWorks, features().method);
  EXPECT_EQ(7, features().encryption_key_size);
  ASSERT_TRUE(last_preq());
  ASSERT_TRUE(last_pres());
  EXPECT_TRUE(ContainersEqual(kRequest, *last_preq()));
  EXPECT_TRUE(ContainersEqual(kResponse, *last_pres()));

  // We send the LTK when we are the responder.
  EXPECT_TRUE(KeyDistGen::kEncKey & features().local_key_distribution);

  // The peer should send us identity information since we requested it and it
  // promised it.
  EXPECT_TRUE(KeyDistGen::kIdKey & features().peer_key_distribution);
}

TEST_F(Phase1Test, FeatureExchangeResponderRequestInitiatorEncKey) {
  const StaticByteBuffer kRequest(0x01,  // code: Pairing Response
                                  0x00,  // IO cap.: DisplayOnly
                                  0x00,  // OOB: not present
                                  AuthReq::kBondingFlag,
                                  0x07,  // encr. key size: 7 (default min)
                                  KeyDistGen::kEncKey,  // initiator keys
                                  0x03                  // responder keys
  );
  const StaticByteBuffer kResponse(0x02,  // code: Pairing Response
                                   0x03,  // IO cap.: NoInputNoOutput
                                   0x00,  // OOB: not present
                                   AuthReq::kBondingFlag | AuthReq::kCT2,
                                   0x10,  // encr. key size: 16 (default max)
                                   KeyDistGen::kEncKey,  // initiator keys
                                   KeyDistGen::kEncKey   // responder keys
  );

  auto reader = PacketReader(&kRequest);
  auto phase_args = Phase1Args{.preq = reader.payload<PairingRequestParams>(),
                               .io_capability = IOCapability::kNoInputNoOutput};

  NewPhase1(Role::kResponder, phase_args);
  EXPECT_PACKET_OUT(kResponse);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  EXPECT_EQ(0, listener()->pairing_error_count());
  EXPECT_EQ(1, feature_exchange_count());
  EXPECT_FALSE(features().initiator);
  EXPECT_TRUE(ContainersEqual(kRequest, *last_preq()));
  EXPECT_TRUE(ContainersEqual(kResponse, *last_pres()));

  // We send the LTK as the initiator requested it. We also request the LTK from
  // the initiator, which indicated it was capable of distributing it.
  EXPECT_TRUE(KeyDistGen::kEncKey & features().local_key_distribution);
  EXPECT_TRUE(KeyDistGen::kEncKey & features().peer_key_distribution);
}

TEST_F(Phase1Test, FeatureExchangeResponderSendsOnlyRequestedKeys) {
  const StaticByteBuffer kRequest(
      0x01,  // code: Pairing Response
      0x00,  // IO cap.: DisplayOnly
      0x00,  // OOB: not present
      AuthReq::kBondingFlag,
      0x07,                // encr. key size: 7 (default min)
      KeyDistGen::kIdKey,  // initiator keys
      KeyDistGen::kIdKey   // responder keys - responder
                           // doesn't have ID info, so
                           // won't send it
  );
  const StaticByteBuffer kResponse(0x02,  // code: Pairing Response
                                   0x03,  // IO cap.: NoInputNoOutput
                                   0x00,  // OOB: not present
                                   AuthReq::kBondingFlag | AuthReq::kCT2,
                                   0x10,  // encr. key size: 16 (default max)
                                   KeyDistGen::kIdKey,  // initiator keys
                                   0x00                 // responder keys: none
  );

  auto reader = PacketReader(&kRequest);
  auto phase_args = Phase1Args{.preq = reader.payload<PairingRequestParams>(),
                               .io_capability = IOCapability::kNoInputNoOutput};
  NewPhase1(Role::kResponder, phase_args);
  EXPECT_PACKET_OUT(kResponse);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());
  ASSERT_EQ(1, feature_exchange_count());
}

TEST_F(Phase1Test, FeatureExchangeResponderMITM) {
  const auto kRequest = StaticByteBuffer(
      0x01,  // code: Pairing Request
      0x02,  // IO cap.: KeyboardOnly
      0x00,  // OOB: not present
      AuthReq::kBondingFlag | AuthReq::kMITM,
      0x07,                // encr. key size: 7 (default min)
      KeyDistGen::kIdKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey  // responder keys
  );
  const StaticByteBuffer kResponse(0x02,  // code: Pairing Response
                                   0x01,  // IO cap.: DisplayYesNo
                                   0x00,  // OOB: not present
                                   AuthReq::kBondingFlag | AuthReq::kCT2,
                                   0x10,  // encr. key size: 16 (default max)
                                   KeyDistGen::kIdKey,  // initiator keys
                                   KeyDistGen::kEncKey  // responder keys
  );

  auto reader = PacketReader(&kRequest);
  auto phase_args = Phase1Args{.preq = reader.payload<PairingRequestParams>(),
                               .io_capability = IOCapability::kDisplayYesNo};
  NewPhase1(Role::kResponder, phase_args);

  EXPECT_PACKET_OUT(kResponse);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  EXPECT_EQ(0, listener()->pairing_error_count());
  EXPECT_EQ(1, feature_exchange_count());
  EXPECT_FALSE(features().initiator);
  EXPECT_FALSE(features().secure_connections);
  EXPECT_EQ(PairingMethod::kPasskeyEntryDisplay, features().method);
  EXPECT_EQ(7, features().encryption_key_size);
  ASSERT_TRUE(last_preq());
  ASSERT_TRUE(last_pres());
  EXPECT_TRUE(ContainersEqual(kRequest, *last_preq()));
  EXPECT_TRUE(ContainersEqual(kResponse, *last_pres()));

  // We send the LTK when we are the responder.
  EXPECT_TRUE(KeyDistGen::kEncKey & features().local_key_distribution);

  // The peer should send us identity information since we requested it and it
  // promised it.
  EXPECT_TRUE(KeyDistGen::kIdKey & features().peer_key_distribution);

  // We should have set bondable mode as both sides enabled it
  EXPECT_TRUE(features().will_bond);
}

TEST_F(Phase1Test, FeatureExchangeResponderRespectsDesiredLevel) {
  const StaticByteBuffer kRequest(0x01,  // code: Pairing Response
                                  0x01,  // IO cap.: KeyboardDisplay
                                  0x00,  // OOB: not present
                                  AuthReq::kBondingFlag | AuthReq::kSC,
                                  0x10,  // encr. key size: 16 (default max)
                                  0x00,  // initiator keys: none
                                  KeyDistGen::kEncKey  // responder keys
  );
  const auto kResponse = StaticByteBuffer(
      0x02,  // code: Pairing Response
      0x01,  // IO cap.: KeyboardDisplay
      0x00,  // OOB: not present
      AuthReq::kBondingFlag | AuthReq::kMITM | AuthReq::kSC | AuthReq::kCT2,
      0x10,                // encr. key size: 16 (default max)
      0x00,                // initiator keys: none
      KeyDistGen::kEncKey  // responder keys
  );

  auto reader = PacketReader(&kRequest);
  auto phase_args = Phase1Args{.preq = reader.payload<PairingRequestParams>(),
                               .io_capability = IOCapability::kDisplayYesNo,
                               .level = SecurityLevel::kAuthenticated,
                               .sc_supported = true};

  NewPhase1(Role::kResponder, phase_args);
  EXPECT_PACKET_OUT(kResponse);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  EXPECT_EQ(0, listener()->pairing_error_count());
  EXPECT_EQ(1, feature_exchange_count());
  EXPECT_TRUE(features().secure_connections);
  // We requested authenticated security, which led to Numeric Comparison as the
  // pairing method.
  EXPECT_EQ(PairingMethod::kNumericComparison, features().method);
  ASSERT_TRUE(last_preq());
  ASSERT_TRUE(last_pres());
  EXPECT_TRUE(ContainersEqual(kRequest, *last_preq()));
  EXPECT_TRUE(ContainersEqual(kResponse, *last_pres()));
}

TEST_F(Phase1Test,
       FeatureExchangeResponderRejectsMethodOfInsufficientSecurity) {
  const StaticByteBuffer kRequest(0x01,  // code: Pairing Response
                                  0x01,  // IO cap.: DisplayYesNo
                                  0x00,  // OOB: not present
                                  AuthReq::kBondingFlag,
                                  0x10,  // encr. key size: 16 (default max)
                                  0x00,  // initiator keys: none
                                  KeyDistGen::kEncKey  // responder keys
  );
  const auto kFailure =
      StaticByteBuffer(kPairingFailed, ErrorCode::kAuthenticationRequirements);

  auto reader = PacketReader(&kRequest);
  auto phase_args = Phase1Args{.preq = reader.payload<PairingRequestParams>(),
                               .io_capability = IOCapability::kDisplayYesNo,
                               .level = SecurityLevel::kAuthenticated,
                               .sc_supported = true};

  NewPhase1(Role::kResponder, phase_args);
  // Both devices have DisplayYesNo IOCap, but the initiator does not support
  // Secure Connections so Numeric Comparison cannot be used. Neither device has
  // a keyboard, so Passkey Entry cannot be used. Thus authenticated pairing,
  // the desired level, cannot be met and we fail.
  EXPECT_PACKET_OUT(kFailure);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  EXPECT_EQ(1, listener()->pairing_error_count());
  EXPECT_EQ(Error(ErrorCode::kAuthenticationRequirements),
            listener()->last_error());
}

TEST_F(Phase1Test,
       FeatureExchangeResponderSecureAuthenticatedInitiatorNoInputNoOutput) {
  const auto kRequest = StaticByteBuffer(
      0x01,  // code: Pairing Request
      0x03,  // IO cap.: NoInputNoOutput
      0x00,  // OOB: not present
      AuthReq::kBondingFlag | AuthReq::kSC,
      0x10,                 // encr. key size: 16 (default max)
      KeyDistGen::kEncKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey  // responder keys
  );
  const auto kFailure =
      StaticByteBuffer(kPairingFailed, ErrorCode::kAuthenticationRequirements);

  auto reader = PacketReader(&kRequest);
  auto phase_args = Phase1Args{.preq = reader.payload<PairingRequestParams>(),
                               .io_capability = IOCapability::kDisplayYesNo,
                               .level = SecurityLevel::kSecureAuthenticated,
                               .sc_supported = true};
  NewPhase1(Role::kResponder, phase_args);

  // Cannot perform SecureAuthenticated pairing with the peer's NoInputNoOutput
  // IOCapabilities.
  EXPECT_PACKET_OUT(kFailure);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  EXPECT_EQ(1, listener()->pairing_error_count());
  EXPECT_EQ(Error(ErrorCode::kAuthenticationRequirements),
            listener()->last_error());
}

TEST_F(Phase1Test, FeatureExchangeResponderDoesntSupportScDoNotGenerateCtKey) {
  const StaticByteBuffer kRequest(
      0x01,  // code: Pairing Request
      IOCapability::kNoInputNoOutput,
      0x00,  // OOB: not present
      AuthReq::kBondingFlag | AuthReq::kSC | AuthReq::kCT2,
      0x10,  // encr. key size: 16 (default max)
      KeyDistGen::kEncKey | KeyDistGen::kLinkKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey |
          KeyDistGen::kLinkKey  // responder keys
  );
  // Although the peer supports CTKG through the hci_spec::LinkKey field and SC,
  // locally we do not support support SC, so CTKG is not allowed (v5.2 Vol. 3
  // Part H 3.6.1).
  const StaticByteBuffer kResponse(0x02,  // code: Pairing Response
                                   IOCapability::kNoInputNoOutput,
                                   0x00,  // OOB: not present
                                   AuthReq::kBondingFlag | AuthReq::kCT2,
                                   0x10,  // encr. key size: 16 (default max)
                                   KeyDistGen::kEncKey,  // initiator keys
                                   KeyDistGen::kEncKey   // responder keys
  );

  auto reader = PacketReader(&kRequest);
  auto phase_args = Phase1Args{.preq = reader.payload<PairingRequestParams>(),
                               .sc_supported = false};
  NewPhase1(Role::kResponder, phase_args);
  EXPECT_PACKET_OUT(kResponse);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  EXPECT_EQ(1, feature_exchange_count());
  EXPECT_FALSE(features().generate_ct_key.has_value());
}

TEST_F(Phase1Test, UnsupportedCommandDuringPairing) {
  const StaticByteBuffer kRequest(
      0x01,  // code: "Pairing Request"
      0x03,  // IO cap.: NoInputNoOutput
      0x00,  // OOB: not present
      AuthReq::kBondingFlag | AuthReq::kCT2,
      0x10,                 // encr. key size: 16 (default max)
      KeyDistGen::kEncKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey  // responder keys
  );
  EXPECT_PACKET_OUT(kRequest);
  phase_1()->Start();

  const StaticByteBuffer kFailed(0x05,  // code: Pairing Failed
                                 0x07   // reason: Command Not Supported
  );
  EXPECT_PACKET_OUT(kFailed);
  fake_chan()->Receive(StaticByteBuffer(0xFF));
  RunUntilIdle();
  ASSERT_TRUE(AllExpectedPacketsSent());

  EXPECT_EQ(1, listener()->pairing_error_count());
  EXPECT_EQ(Error(ErrorCode::kCommandNotSupported), listener()->last_error());
}

TEST_F(Phase1Test, OnSecurityRequestWhilePairing) {
  const StaticByteBuffer kPairingRequest(
      0x01,  // code: "Pairing Request"
      0x03,  // IO cap.: NoInputNoOutput
      0x00,  // OOB: not present
      AuthReq::kBondingFlag | AuthReq::kCT2,
      0x10,                 // encr. key size: 16 (default max)
      KeyDistGen::kEncKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey  // responder keys
  );
  EXPECT_PACKET_OUT(kPairingRequest);
  phase_1()->Start();

  const StaticByteBuffer kSecurityRequest(0x0B,  // code: Security Request
                                          0x00   // auth_req
  );

  const StaticByteBuffer kFailed(0x05,  // code: Pairing Failed
                                 0x08   // reason: unspecified reason
  );
  EXPECT_PACKET_OUT(kFailed);
  fake_chan()->Receive(kSecurityRequest);
  RunUntilIdle();

  // The security request while pairing should cause pairing to fail.
  EXPECT_EQ(1, listener()->pairing_error_count());
}

// Tests whether a request from a device with bondable mode enabled to a peer
// with non-bondable mode enabled will return a PairingFeatures with
// non-bondable mode enabled, the desired result.
TEST_F(Phase1Test, FeatureExchangeInitiatorReqBondResNoBond) {
  const auto kRequest = StaticByteBuffer(
      0x01,  // code: Pairing Request
      0x03,  // IO cap.: NoInputNoOutput
      0x00,  // OOB: not present
      AuthReq::kBondingFlag | AuthReq::kCT2,
      0x10,                 // encr. key size: 16 (default max)
      KeyDistGen::kEncKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey  // responder keys
  );
  const auto kResponse =
      StaticByteBuffer(0x02,  // code: Pairing Response
                       0x00,  // IO cap.: DisplayOnly
                       0x00,  // OOB: not present
                       0x00,
                       0x07,  // encr. key size: 7 (default min)
                       0x00,  // initiator keys: none
                       0x00   // responder keys: none due to non-bondable mode
      );

  EXPECT_PACKET_OUT(kRequest);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  fake_chan()->Receive(kResponse);
  RunUntilIdle();

  // Should be in non-bondable mode even though the Initiator specifies bonding,
  // as kResponse indicated that the peer follower does not support bonding.
  EXPECT_FALSE(features().will_bond);
  EXPECT_EQ(features().local_key_distribution, 0u);
  EXPECT_EQ(features().peer_key_distribution, 0u);
}

TEST_F(Phase1Test, FeatureExchangeInitiatorReqNoBondResBond) {
  auto phase_args = Phase1Args{.bondable_mode = BondableMode::NonBondable};
  NewPhase1(Role::kInitiator, phase_args);
  const StaticByteBuffer kRequest(0x01,           // code: Pairing Request
                                  0x03,           // IO cap.: NoInputNoOutput
                                  0x00,           // OOB: not present
                                  AuthReq::kCT2,  // AuthReq: non-bondable
                                  0x10,  // encr. key size: 16 (default max)
                                  0x00,  // initiator keys: none
                                  0x00   // responder keys: none
  );
  const auto kResponse = StaticByteBuffer(
      0x02,  // code: Pairing Response
      0x00,  // IO cap.: DisplayOnly
      0x00,  // OOB: not present
      AuthReq::kBondingFlag,
      0x07,  // encr. key size: 7 (default min)
      0x00,  // initiator keys: none - should not change request field
      0x00   // responder keys: none - should not change request field
  );

  EXPECT_PACKET_OUT(kRequest);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  fake_chan()->Receive(kResponse);
  RunUntilIdle();

  // Although kResponse is bondable, features should not bond as local device is
  // non-bondable.
  ASSERT_EQ(1, feature_exchange_count());
  EXPECT_FALSE(features().will_bond);
  EXPECT_EQ(features().local_key_distribution, 0u);
  EXPECT_EQ(features().peer_key_distribution, 0u);
}

TEST_F(Phase1Test, FeatureExchangeResponderReqBondResNoBond) {
  const auto kRequest = StaticByteBuffer(
      0x01,  // code: Pairing Request
      0x03,  // IO cap.: NoInputNoOutput
      0x00,  // OOB: not present
      AuthReq::kBondingFlag,
      0x10,  // encr. key size: 16 (default max)
      0x00,  // initiator keys: none
      KeyDistGen::kEncKey | KeyDistGen::kIdKey  // responder keys
  );
  const auto kResponse = StaticByteBuffer(
      0x02,           // code: Pairing Response
      0x03,           // IO cap.: NoInputNoOutput
      0x00,           // OOB: not present
      AuthReq::kCT2,  // AuthReq: non-bondable to match local mode,
                      // even though kRequest was bondable
      0x10,           // encr. key size: 16 (default max)
      0x00,           // initiator keys: none - should not change request field
      0x00            // responder keys: none - should not change request field
  );

  auto reader = PacketReader(&kRequest);
  auto phase_args = Phase1Args{.preq = reader.payload<PairingRequestParams>(),
                               .bondable_mode = BondableMode::NonBondable};
  NewPhase1(Role::kResponder, phase_args);
  EXPECT_PACKET_OUT(kResponse);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  // Should be in non-bondable mode even though the peer requested bondable, as
  // the Bearer was created in non-bondable mode.
  EXPECT_FALSE(features().will_bond);
  EXPECT_EQ(features().local_key_distribution, 0u);
  EXPECT_EQ(features().peer_key_distribution, 0u);
}

TEST_F(Phase1Test, FeatureExchangeResponderReqNoBondResNoBond) {
  const StaticByteBuffer kRequest(0x01,  // code: Pairing Request
                                  0x03,  // IO cap.: NoInputNoOutput
                                  0x00,  // OOB: not present
                                  0x00,  // AuthReq: non-bondable
                                  0x10,  // encr. key size: 16 (default max)
                                  0x00,  // initiator keys: none
                                  0x00   // responder keys: none
  );
  const auto kResponse = StaticByteBuffer(
      0x02,           // code: Pairing Response
      0x03,           // IO cap.: NoInputNoOutput
      0x00,           // OOB: not present
      AuthReq::kCT2,  // AuthReq: non-bondable to match peer mode, even
                      // though Phase1 is bondable.
      0x10,           // encr. key size: 16 (default max)
      0x00,           // initiator keys: none - should not change request field
      0x00            // responder keys: none - should not change request field
  );

  auto reader = PacketReader(&kRequest);
  auto phase_args = Phase1Args{
      .preq = reader.payload<PairingRequestParams>(),
      .bondable_mode = BondableMode::Bondable,  // local mode is bondable,
                                                // although peer is not
  };
  NewPhase1(Role::kResponder, phase_args);
  EXPECT_PACKET_OUT(kResponse);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  // Should be in non-bondable mode even though Bearer was created in bondable
  // mode as kRequest indicated that peer does not support bonding.
  EXPECT_FALSE(features().will_bond);
  EXPECT_EQ(features().local_key_distribution, 0u);
  EXPECT_EQ(features().peer_key_distribution, 0u);
}

TEST_F(Phase1Test, FeatureExchangeResponderReqNoBondWithKeys) {
  const auto kRequest = StaticByteBuffer(
      0x01,           // code: Pairing Request
      0x03,           // IO cap.: NoInputNoOutput
      0x00,           // OOB: not present
      AuthReq::kCT2,  // AuthReq: non-bondable
      0x10,           // encr. key size: 16 (default max)
      KeyDistGen::kEncKey | KeyDistGen::kIdKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey   // responder keys
  );

  auto reader = PacketReader(&kRequest);
  auto phase_args = Phase1Args{.preq = reader.payload<PairingRequestParams>()};
  NewPhase1(Role::kResponder, phase_args);

  const StaticByteBuffer kFailed(0x05,  // code: Pairing Failed
                                 0x0A   // reason: invalid parameters
  );
  EXPECT_PACKET_OUT(kFailed);
  phase_1()->Start();
  RunUntilIdle();

  // Check that we fail with invalid parameters when a peer requests nonbondable
  // mode with a non-zero KeyDistGen field
  EXPECT_EQ(1, listener()->pairing_error_count());
  EXPECT_EQ(Error(ErrorCode::kInvalidParameters), listener()->last_error());
}

TEST_F(Phase1Test, BrEdrInitiatorLocalAndPeerRequestEncKeyAndCT2Success) {
  NewPhase1(Role::kInitiator, Phase1Args(), LinkType::kACL);
  const auto kRequest = StaticByteBuffer(
      0x01,  // code: Pairing Request
      0x03,  // IO cap.: NoInputNoOutput
      0x00,  // OOB: not present
      AuthReq::kCT2,
      0x10,                 // encr. key size: 16 (default max)
      KeyDistGen::kEncKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey  // responder keys
  );
  const auto kResponse =
      StaticByteBuffer(0x02,  // code: Pairing Response
                       0x00,  // IO cap.: DisplayOnly
                       0x00,  // OOB: not present
                       AuthReq::kCT2,
                       0x10,                 // encr. key size: 16 (default max)
                       KeyDistGen::kEncKey,  // initiator keys
                       KeyDistGen::kEncKey   // responder keys
      );

  EXPECT_PACKET_OUT(kRequest);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  fake_chan()->Receive(kResponse);
  RunUntilIdle();
  EXPECT_EQ(0, listener()->pairing_error_count());
  EXPECT_EQ(1, feature_exchange_count());
  EXPECT_TRUE(features().initiator);
  ASSERT_TRUE(features().generate_ct_key.has_value());
  EXPECT_EQ(features().generate_ct_key.value(), CrossTransportKeyAlgo::kUseH7);
}

TEST_F(Phase1Test, BrEdrInitiatorLocalAndPeerRequestEncKeyAndNoPeerCT2Success) {
  NewPhase1(Role::kInitiator, Phase1Args(), LinkType::kACL);
  const auto kRequest = StaticByteBuffer(
      0x01,  // code: Pairing Request
      0x03,  // IO cap.: NoInputNoOutput
      0x00,  // OOB: not present
      AuthReq::kCT2,
      0x10,                 // encr. key size: 16 (default max)
      KeyDistGen::kEncKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey  // responder keys
  );
  const auto kResponse =
      StaticByteBuffer(0x02,                 // code: Pairing Response
                       0x00,                 // IO cap.: DisplayOnly
                       0x00,                 // OOB: not present
                       0x00,                 // AuthReq, no CT2 bit set
                       0x10,                 // encr. key size: 16 (default max)
                       KeyDistGen::kEncKey,  // initiator keys
                       KeyDistGen::kEncKey   // responder keys
      );

  EXPECT_PACKET_OUT(kRequest);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  fake_chan()->Receive(kResponse);
  RunUntilIdle();
  EXPECT_EQ(0, listener()->pairing_error_count());
  EXPECT_EQ(1, feature_exchange_count());
  EXPECT_TRUE(features().initiator);
  ASSERT_TRUE(features().generate_ct_key.has_value());
  EXPECT_EQ(features().generate_ct_key.value(), CrossTransportKeyAlgo::kUseH6);
}

TEST_F(Phase1Test, BrEdrInitiatorPeerDoesNotSupportEncKey) {
  NewPhase1(Role::kInitiator, Phase1Args(), LinkType::kACL);
  const auto kRequest = StaticByteBuffer(
      0x01,  // code: Pairing Request
      0x03,  // IO cap.: NoInputNoOutput
      0x00,  // OOB: not present
      AuthReq::kCT2,
      0x10,                 // encr. key size: 16 (default max)
      KeyDistGen::kEncKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey  // responder keys
  );
  const auto kResponse =
      StaticByteBuffer(0x02,  // code: Pairing Response
                       0x00,  // IO cap.: DisplayOnly
                       0x00,  // OOB: not present
                       AuthReq::kCT2,
                       0x10,                 // encr. key size: 16 (default max)
                       KeyDistGen::kEncKey,  // initiator keys
                       0x00                  // responder keys
      );

  EXPECT_PACKET_OUT(kRequest);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  fake_chan()->Receive(kResponse);
  RunUntilIdle();
  EXPECT_EQ(0, listener()->pairing_error_count());
  EXPECT_EQ(1, feature_exchange_count());
  EXPECT_TRUE(features().initiator);
  ASSERT_FALSE(features().generate_ct_key.has_value());
}

TEST_F(Phase1Test, BrEdrResponderBothSupportEncKeyAndCT2) {
  const auto kResponse =
      StaticByteBuffer(0x02,  // code: Pairing Response
                       0x03,  // IO cap.: NoInputNoOutput
                       0x00,  // OOB: not present
                       AuthReq::kCT2,
                       0x10,                 // encr. key size: 16 (default max)
                       KeyDistGen::kEncKey,  // initiator keys: none
                       KeyDistGen::kEncKey   // responder keys
      );

  Phase1Args args;
  args.preq =
      PairingRequestParams{.io_capability = IOCapability::kNoInputNoOutput,
                           .oob_data_flag = OOBDataFlag::kNotPresent,
                           .auth_req = AuthReq::kCT2,
                           .max_encryption_key_size = 0x10,  // 16, default max
                           .initiator_key_dist_gen = KeyDistGen::kEncKey,
                           .responder_key_dist_gen = KeyDistGen::kEncKey};
  NewPhase1(Role::kResponder, args, LinkType::kACL);
  EXPECT_PACKET_OUT(kResponse);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  RunUntilIdle();
  EXPECT_EQ(0, listener()->pairing_error_count());
  EXPECT_EQ(1, feature_exchange_count());
  EXPECT_FALSE(features().initiator);
  ASSERT_TRUE(features().generate_ct_key.has_value());
  EXPECT_EQ(features().generate_ct_key.value(), CrossTransportKeyAlgo::kUseH7);
}

TEST_F(Phase1Test, BrEdrResponderBothSupportEncKeyAndNotCT2) {
  const auto kResponse =
      StaticByteBuffer(0x02,                 // code: Pairing Response
                       0x03,                 // IO cap.: NoInputNoOutput
                       0x00,                 // OOB: not present
                       AuthReq::kCT2,        // AuthReq
                       0x10,                 // encr. key size: 16 (default max)
                       KeyDistGen::kEncKey,  // initiator keys: none
                       KeyDistGen::kEncKey   // responder keys
      );

  Phase1Args args;
  args.preq =
      PairingRequestParams{.io_capability = IOCapability::kNoInputNoOutput,
                           .oob_data_flag = OOBDataFlag::kNotPresent,
                           .auth_req = 0x00,
                           .max_encryption_key_size = 0x10,  // 16, default max
                           .initiator_key_dist_gen = KeyDistGen::kEncKey,
                           .responder_key_dist_gen = KeyDistGen::kEncKey};
  NewPhase1(Role::kResponder, args, LinkType::kACL);
  EXPECT_PACKET_OUT(kResponse);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  RunUntilIdle();
  EXPECT_EQ(0, listener()->pairing_error_count());
  EXPECT_EQ(1, feature_exchange_count());
  EXPECT_FALSE(features().initiator);
  ASSERT_TRUE(features().generate_ct_key.has_value());
  EXPECT_EQ(features().generate_ct_key.value(), CrossTransportKeyAlgo::kUseH6);
}

TEST_F(Phase1Test, BrEdrInitiatorLocalAndPeerRequestIdKeyAndEncKey) {
  NewPhase1(Role::kInitiator, Phase1Args(), LinkType::kACL);
  listener()->set_identity_info(IdentityInfo());

  const auto kRequest = StaticByteBuffer(
      0x01,  // code: Pairing Request
      0x03,  // IO cap.: NoInputNoOutput
      0x00,  // OOB: not present
      AuthReq::kCT2,
      0x10,  // encr. key size: 16 (default max)
      KeyDistGen::kEncKey | KeyDistGen::kIdKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey   // responder keys
  );
  const auto kResponse = StaticByteBuffer(
      0x02,  // code: Pairing Response
      0x00,  // IO cap.: DisplayOnly
      0x00,  // OOB: not present
      AuthReq::kCT2,
      0x10,  // encr. key size: 16 (default max)
      KeyDistGen::kEncKey | KeyDistGen::kIdKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey   // responder keys
  );

  EXPECT_PACKET_OUT(kRequest);
  phase_1()->Start();
  EXPECT_EQ(1, listener()->identity_info_count());
  ASSERT_TRUE(AllExpectedPacketsSent());

  fake_chan()->Receive(kResponse);
  RunUntilIdle();
  EXPECT_EQ(0, listener()->pairing_error_count());
  EXPECT_EQ(1, feature_exchange_count());
  EXPECT_TRUE(features().initiator);
  EXPECT_TRUE(features().local_key_distribution & KeyDistGen::kIdKey);
  EXPECT_TRUE(features().peer_key_distribution & KeyDistGen::kIdKey);
}

TEST_F(Phase1Test, BrEdrInitiatorPeerTooSmallKeySize) {
  NewPhase1(Role::kInitiator, Phase1Args(), LinkType::kACL);
  const auto kRequest = StaticByteBuffer(
      0x01,  // code: Pairing Request
      0x03,  // IO cap.: NoInputNoOutput
      0x00,  // OOB: not present
      AuthReq::kCT2,
      0x10,                 // encr. key size: 16 (default max)
      KeyDistGen::kEncKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey  // responder keys
  );
  const auto kResponse =
      StaticByteBuffer(0x02,  // code: Pairing Response
                       0x00,  // IO cap.: DisplayOnly
                       0x00,  // OOB: not present
                       AuthReq::kCT2,
                       0x07,                 // encr. key size: 7 (default min)
                       KeyDistGen::kEncKey,  // initiator keys
                       KeyDistGen::kEncKey   // responder keys
      );
  const StaticByteBuffer kFailure(0x05,  // code: Pairing Failed
                                  0x06   // reason: Encryption Key Size
  );

  EXPECT_PACKET_OUT(kRequest);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  EXPECT_PACKET_OUT(kFailure);
  fake_chan()->Receive(kResponse);
  RunUntilIdle();
  EXPECT_EQ(1, listener()->pairing_error_count());
  EXPECT_EQ(Error(ErrorCode::kEncryptionKeySize), listener()->last_error());
  EXPECT_EQ(0, feature_exchange_count());
}

TEST_F(Phase1Test, BrEdrInitiatorPeerSetsRfuFieldsToWeirdValuesThatAreIgnored) {
  NewPhase1(Role::kInitiator, Phase1Args(), LinkType::kACL);
  const auto kRequest = StaticByteBuffer(
      0x01,  // code: Pairing Request
      0x03,  // IO cap.: NoInputNoOutput
      0x00,  // OOB: not present
      AuthReq::kCT2,
      0x10,                 // encr. key size: 16 (default max)
      KeyDistGen::kEncKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey  // responder keys
  );
  const auto kResponse =
      StaticByteBuffer(0x02,  // code: Pairing Response
                       0x04,  // IO cap.: KeyboardDisplay
                       0x01,  // OOB: present
                       AuthReq::kBondingFlag | AuthReq::kMITM | AuthReq::kSC |
                           AuthReq::kKeypress,
                       0x10,                 // encr. key size: 16 (default max)
                       KeyDistGen::kEncKey,  // initiator keys
                       KeyDistGen::kEncKey   // responder keys
      );

  EXPECT_PACKET_OUT(kRequest);
  phase_1()->Start();
  ASSERT_TRUE(AllExpectedPacketsSent());

  fake_chan()->Receive(kResponse);
  RunUntilIdle();
  EXPECT_EQ(0, listener()->pairing_error_count());
  EXPECT_EQ(1, feature_exchange_count());
  EXPECT_TRUE(features().initiator);
  ASSERT_TRUE(features().generate_ct_key.has_value());
}

}  // namespace
}  // namespace bt::sm
