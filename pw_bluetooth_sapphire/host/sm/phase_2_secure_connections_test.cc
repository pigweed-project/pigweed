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

#include "pw_bluetooth_sapphire/internal/host/sm/phase_2_secure_connections.h"

#include <pw_assert/check.h>

#include <cstdint>
#include <memory>

#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/common/device_address.h"
#include "pw_bluetooth_sapphire/internal/host/common/random.h"
#include "pw_bluetooth_sapphire/internal/host/common/uint128.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"
#include "pw_bluetooth_sapphire/internal/host/hci/connection.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/fake_channel_test.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/l2cap_defs.h"
#include "pw_bluetooth_sapphire/internal/host/sm/ecdh_key.h"
#include "pw_bluetooth_sapphire/internal/host/sm/fake_phase_listener.h"
#include "pw_bluetooth_sapphire/internal/host/sm/packet.h"
#include "pw_bluetooth_sapphire/internal/host/sm/smp.h"
#include "pw_bluetooth_sapphire/internal/host/sm/types.h"
#include "pw_bluetooth_sapphire/internal/host/sm/util.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"
#include "pw_unit_test/framework.h"

namespace bt::sm {
namespace {
using ConfirmCallback = FakeListener::ConfirmCallback;
using PasskeyResponseCallback = FakeListener::PasskeyResponseCallback;

const PairingFeatures kDefaultFeatures = {
    .initiator = true,
    .secure_connections = true,
    .will_bond = true,
    .generate_ct_key = std::optional<CrossTransportKeyAlgo>{std::nullopt},
    .method = PairingMethod::kJustWorks,
    .encryption_key_size = kMaxEncryptionKeySize,
    .local_key_distribution = KeyDistGen::kIdKey,
    .remote_key_distribution = KeyDistGen::kIdKey | KeyDistGen::kEncKey};

const PairingRequestParams kDefaultPreq{
    .io_capability = IOCapability::kNoInputNoOutput,
    .oob_data_flag = OOBDataFlag::kNotPresent,
    .auth_req = AuthReq::kSC | AuthReq::kBondingFlag,
    .max_encryption_key_size = kMaxEncryptionKeySize,
    .initiator_key_dist_gen = KeyDistGen::kIdKey,
    .responder_key_dist_gen = KeyDistGen::kIdKey | KeyDistGen::kEncKey};

const PairingResponseParams kDefaultPres{
    .io_capability = IOCapability::kNoInputNoOutput,
    .oob_data_flag = OOBDataFlag::kNotPresent,
    .auth_req = AuthReq::kSC | AuthReq::kBondingFlag,
    .max_encryption_key_size = kMaxEncryptionKeySize,
    .initiator_key_dist_gen = KeyDistGen::kIdKey,
    .responder_key_dist_gen = KeyDistGen::kIdKey | KeyDistGen::kEncKey};

const DeviceAddress kAddr1(DeviceAddress::Type::kLEPublic,
                           {0x00, 0x00, 0x00, 0x00, 0x00, 0x01});
const DeviceAddress kAddr2(DeviceAddress::Type::kLEPublic,
                           {0x00, 0x00, 0x00, 0x00, 0x00, 0x02});

LocalEcdhKey GenKey() {
  static bool backend_set_up = false;
  if (!backend_set_up) {
    pw::crypto::ecdh::SetUpBackendForTesting();
    backend_set_up = true;
  }
  return LocalEcdhKey::Create().value();
}

const LocalEcdhKey kDefaultEcdhKey = GenKey();

using util::PacketSize;

class Phase2SecureConnectionsTest : public l2cap::testing::FakeChannelTest {
 public:
  Phase2SecureConnectionsTest() = default;
  ~Phase2SecureConnectionsTest() override = default;

 protected:
  void SetUp() override { NewPhase2SecureConnections(); }

  void TearDown() override { phase_2_sc_ = nullptr; }

  void NewPhase2SecureConnections(
      Role local_role = Role::kInitiator,
      PairingMethod method = PairingMethod::kJustWorks,
      uint16_t mtu = kLeSecureConnectionsMtu) {
    features_.initiator = (local_role == Role::kInitiator);
    features_.method = method;
    ChannelOptions options(l2cap::kLESMPChannelId, mtu);
    options.link_type = bt::LinkType::kLE;
    fake_sm_chan_ = CreateFakeChannel(options);
    sm_chan_ = std::make_unique<PairingChannel>(fake_sm_chan_->GetWeakPtr());

    listener_ = std::make_unique<FakeListener>();
    phase_2_sc_ =
        std::make_unique<Phase2SecureConnections>(sm_chan_->GetWeakPtr(),
                                                  listener_->as_weak_ptr(),
                                                  local_role,
                                                  features_,
                                                  preq_,
                                                  pres_,
                                                  initiator_addr_,
                                                  responder_addr_,
                                                  [this](const UInt128& ltk) {
                                                    phase_2_complete_count_++;
                                                    ltk_ = ltk;
                                                  });
  }

  template <typename T>
  void ReceiveCmd(Code cmd_code, const T& value) {
    fake_chan()->Receive(MakeCmd(cmd_code, value));
  }

  template <typename T>
  StaticByteBuffer<PacketSize<T>()> MakeCmd(Code cmd_code, const T& value) {
    StaticByteBuffer<PacketSize<T>()> buffer;
    PacketWriter writer(cmd_code, &buffer);
    *writer.mutable_payload<T>() = value;
    return buffer;
  }

  static std::pair<Code, UInt128> ExtractCodeAnd128BitCmd(ByteBufferPtr sdu) {
    PW_CHECK(sdu, "Tried to ExtractCodeAnd128BitCmd from nullptr in test");
    auto maybe_reader = ValidPacketReader::ParseSdu(sdu);
    PW_CHECK(maybe_reader.is_ok(),
             "Tried to ExtractCodeAnd128BitCmd from invalid SMP packet");
    return {maybe_reader.value().code(),
            maybe_reader.value().payload<UInt128>()};
  }

  std::optional<PairingConfirmValue> FastForwardPublicKeyExchange() {
    std::optional<PairingConfirmValue> responder_jw_nc_confirm = std::nullopt;
    fake_chan()->SetSendCallback(
        [&responder_jw_nc_confirm, this](ByteBufferPtr sdu) {
          auto reader = ValidPacketReader::ParseSdu(sdu).value();
          if (reader.code() == kPairingPublicKey) {
            local_key_ = EcdhKey::ParseFromPublicKey(
                reader.payload<PairingPublicKeyParams>());
          } else if (reader.code() == kPairingConfirm) {
            // Confirm must come after ECDH Pub Key if it comes now
            ASSERT_TRUE(local_key_.has_value());
            responder_jw_nc_confirm = reader.payload<PairingConfirmValue>();
          } else {
            ADD_FAILURE() << "unexpected packet code " << reader.code();
          }
        },
        dispatcher());

    if (phase_2_sc_->role() == Role::kInitiator) {
      phase_2_sc_->Start();
      RunUntilIdle();
      PW_CHECK(local_key_.has_value(),
               "initiator did not send ecdh key upon starting");
      ReceiveCmd(kPairingPublicKey, peer_key_.GetSerializedPublicKey());
      RunUntilIdle();
    } else {
      phase_2_sc_->Start();
      ReceiveCmd(kPairingPublicKey, peer_key_.GetSerializedPublicKey());
      RunUntilIdle();
      PW_CHECK(local_key_.has_value(),
               "responder did not send ecdh key upon peer key");
      if (features_.method == PairingMethod::kJustWorks ||
          features_.method == PairingMethod::kNumericComparison) {
        PW_CHECK(responder_jw_nc_confirm.has_value());
        return responder_jw_nc_confirm;
      }
    }
    // We should only send confirm value immediately after ECDH key as responder
    // in Numeric Comparison/Just Works pairing, in which case we would've
    // already returned.
    PW_CHECK(!responder_jw_nc_confirm.has_value());
    return responder_jw_nc_confirm;
  }

  UInt128 GenerateConfirmValue(const UInt128& random,
                               bool gen_initiator_confirm,
                               uint8_t r = 0) const {
    PW_CHECK(local_key_.has_value(), "cannot compute confirm, missing key!");
    UInt256 pka = local_key_->GetPublicKeyX(), pkb = peer_key_.GetPublicKeyX();
    if (phase_2_sc_->role() == Role::kResponder) {
      std::swap(pka, pkb);
    }
    return gen_initiator_confirm ? util::F4(pka, pkb, random, r).value()
                                 : util::F4(pkb, pka, random, r).value();
  }

  struct MatchingPair {
    UInt128 confirm;
    UInt128 random;
  };
  MatchingPair GenerateMatchingConfirmAndRandom(uint8_t r = 0) const {
    MatchingPair pair{.confirm = {}, .random = {1, 2, 3, 4}};
    pair.confirm = GenerateConfirmValue(
        pair.random, phase_2_sc_->role() == Role::kResponder, r);
    return pair;
  }

  struct LtkAndChecks {
    UInt128 ltk;
    UInt128 dhkey_check_a;
    UInt128 dhkey_check_b;
  };
  LtkAndChecks GenerateLtkAndChecks(const UInt128& initiator_rand,
                                    const UInt128& responder_rand,
                                    uint64_t r = 0) {
    LtkAndChecks vals;
    util::F5Results f5 = *util::F5(peer_key_.CalculateDhKey(*local_key_),
                                   initiator_rand,
                                   responder_rand,
                                   initiator_addr_,
                                   responder_addr_);
    vals.ltk = f5.ltk;

    UInt128 r_array{0};
    // Copy little-endian uint64 r to the UInt128 array needed for Stage 2
    std::memcpy(r_array.data(), &r, sizeof(uint64_t));
    vals.dhkey_check_a = *util::F6(f5.mac_key,
                                   initiator_rand,
                                   responder_rand,
                                   r_array,
                                   preq_.auth_req,
                                   preq_.oob_data_flag,
                                   preq_.io_capability,
                                   initiator_addr_,
                                   responder_addr_);
    vals.dhkey_check_b = *util::F6(f5.mac_key,
                                   responder_rand,
                                   initiator_rand,
                                   r_array,
                                   pres_.auth_req,
                                   pres_.oob_data_flag,
                                   pres_.io_capability,
                                   responder_addr_,
                                   initiator_addr_);
    return vals;
  }

  LtkAndChecks FastForwardToDhKeyCheck() {
    PW_CHECK(
        features_.method == PairingMethod::kJustWorks,
        "Fast forward to DHKey check only implemented for JustWorks method");
    return phase_2_sc_->role() == Role::kInitiator
               ? FastForwardToDhKeyCheckInitiatorJustWorks()
               : FastForwardToDhKeyCheckResponderJustWorks();
  }

  void DestroyPhase2() { phase_2_sc_.reset(nullptr); }
  Phase2SecureConnections* phase_2_sc() { return phase_2_sc_.get(); }
  FakeListener* listener() { return listener_.get(); }
  const LocalEcdhKey& peer_key() { return peer_key_; }
  const std::optional<EcdhKey>& local_key() { return local_key_; }
  std::optional<EcdhKey>& mut_local_key() { return local_key_; }
  int phase_2_complete_count() const { return phase_2_complete_count_; }
  UInt128 ltk() const { return ltk_; }

 private:
  LtkAndChecks FastForwardToDhKeyCheckInitiatorJustWorks() {
    FastForwardPublicKeyExchange();
    Code sent_code = kPairingFailed;
    PairingRandomValue initiator_rand;
    fake_chan()->SetSendCallback(
        [&](ByteBufferPtr sdu) {
          std::tie(sent_code, initiator_rand) =
              ExtractCodeAnd128BitCmd(std::move(sdu));
        },
        dispatcher());
    MatchingPair stage1_vals = GenerateMatchingConfirmAndRandom();
    ReceiveCmd<PairingConfirmValue>(kPairingConfirm, stage1_vals.confirm);
    RunUntilIdle();
    PW_CHECK(kPairingRandom == sent_code,
             "did not send pairing random when expected!");

    ReceiveCmd(kPairingRandom, stage1_vals.random);
    RunUntilIdle();
    return GenerateLtkAndChecks(initiator_rand, stage1_vals.random);
  }

  LtkAndChecks FastForwardToDhKeyCheckResponderJustWorks() {
    UInt128 rsp_confirm = *FastForwardPublicKeyExchange();
    Code sent_code = kPairingFailed;
    UInt128 rsp_rand;
    fake_chan()->SetSendCallback(
        [&](ByteBufferPtr sdu) {
          std::tie(sent_code, rsp_rand) =
              ExtractCodeAnd128BitCmd(std::move(sdu));
        },
        dispatcher());
    PairingRandomValue initiator_rand{1};
    ReceiveCmd(kPairingRandom, initiator_rand);
    RunUntilIdle();

    PW_CHECK(kPairingRandom == sent_code,
             "did not send pairing random when expected!");
    PW_CHECK(GenerateConfirmValue(rsp_rand, /*gen_initiator_confirm=*/false) ==
                 rsp_confirm,
             "send invalid confirm value as JustWorks responder");
    return GenerateLtkAndChecks(initiator_rand, rsp_rand);
  }

  std::unique_ptr<FakeListener> listener_;
  std::unique_ptr<l2cap::testing::FakeChannel> fake_sm_chan_;
  std::unique_ptr<PairingChannel> sm_chan_;
  std::unique_ptr<Phase2SecureConnections> phase_2_sc_;
  // Key of the internal bt-host SMP stack
  std::optional<EcdhKey> local_key_;
  // Key of the unit test peer we're mocking
  const LocalEcdhKey& peer_key_ = kDefaultEcdhKey;

  PairingFeatures features_ = kDefaultFeatures;
  PairingRequestParams preq_ = kDefaultPreq;
  PairingResponseParams pres_ = kDefaultPres;
  DeviceAddress initiator_addr_ = kAddr1;
  DeviceAddress responder_addr_ = kAddr2;
  int phase_2_complete_count_ = 0;
  UInt128 ltk_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(Phase2SecureConnectionsTest);
};

using Phase2SecureConnectionsDeathTest = Phase2SecureConnectionsTest;

TEST_F(Phase2SecureConnectionsDeathTest, MtuTooSmallDies) {
  ASSERT_DEATH_IF_SUPPORTED(
      NewPhase2SecureConnections(
          Role::kInitiator, PairingMethod::kJustWorks, kNoSecureConnectionsMtu),
      ".*SecureConnections.*");
}

TEST_F(Phase2SecureConnectionsTest, ReceivePairingFailed) {
  phase_2_sc()->Start();
  fake_chan()->Receive(StaticByteBuffer<PacketSize<ErrorCode>()>{
      kPairingFailed, ErrorCode::kPairingNotSupported});
  RunUntilIdle();

  EXPECT_EQ(Error(ErrorCode::kPairingNotSupported), listener()->last_error());
}

TEST_F(Phase2SecureConnectionsTest, ReceiveMalformedPacket) {
  phase_2_sc()->Start();
  // PairingPublicKeyParams is expected to have both an X and Y value, not just
  // an X.
  const UInt256 kX = peer_key().GetPublicKeyX();
  const auto kPairingPublicKeyCmd = MakeCmd(kPairingPublicKey, kX);
  const StaticByteBuffer<PacketSize<ErrorCode>()> kExpectedFailure{
      kPairingFailed, ErrorCode::kInvalidParameters};

  EXPECT_TRUE(ReceiveAndExpect(kPairingPublicKeyCmd, kExpectedFailure));
}

TEST_F(Phase2SecureConnectionsTest, ReceiveUnexpectedPacket) {
  phase_2_sc()->Start();
  // Pairing Responses should only be sent during Phase 1 of pairing.
  const auto kPairingResponseCmd =
      MakeCmd(kPairingResponse, PairingResponseParams());
  const StaticByteBuffer<PacketSize<ErrorCode>()> kExpectedFailure{
      kPairingFailed, ErrorCode::kUnspecifiedReason};

  EXPECT_TRUE(ReceiveAndExpect(kPairingResponseCmd, kExpectedFailure));
}

TEST_F(Phase2SecureConnectionsTest, InitiatorPubKeyOutOfOrder) {
  NewPhase2SecureConnections(Role::kInitiator);

  const auto kPairingPublicKeyCmd =
      MakeCmd(kPairingPublicKey, peer_key().GetSerializedPublicKey());
  const StaticByteBuffer<PacketSize<ErrorCode>()> kExpectedFailure{
      kPairingFailed, ErrorCode::kUnspecifiedReason};
  ASSERT_TRUE(ReceiveAndExpect(kPairingPublicKeyCmd, kExpectedFailure));
  ASSERT_EQ(1, listener()->pairing_error_count());
}

TEST_F(Phase2SecureConnectionsTest, RejectsPublicKeyOffCurve) {
  phase_2_sc()->Start();
  const auto kPairingPublicKeyCmd = MakeCmd(
      kPairingPublicKey, PairingPublicKeyParams{.x = {0x01}, .y = {0x02}});
  const StaticByteBuffer<PacketSize<ErrorCode>()> kExpectedFailure{
      kPairingFailed, ErrorCode::kInvalidParameters};
  ASSERT_TRUE(ReceiveAndExpect(kPairingPublicKeyCmd, kExpectedFailure));
  ASSERT_EQ(1, listener()->pairing_error_count());
}

TEST_F(Phase2SecureConnectionsTest, RejectsPublicKeyIdenticalToLocalKey) {
  // Read local key sent to peer
  fake_chan()->SetSendCallback(
      [this](ByteBufferPtr sdu) {
        auto reader = ValidPacketReader::ParseSdu(sdu).value();
        if (reader.code() == kPairingPublicKey) {
          mut_local_key() = EcdhKey::ParseFromPublicKey(
              reader.payload<PairingPublicKeyParams>());
        }
      },
      dispatcher());

  phase_2_sc()->Start();
  RunUntilIdle();
  ASSERT_TRUE(local_key().has_value());

  // Mirror back local key as the peer's public key
  const auto kPairingPublicKeyCmd =
      MakeCmd(kPairingPublicKey, local_key()->GetSerializedPublicKey());
  const StaticByteBuffer kExpectedFailure{kPairingFailed,
                                          ErrorCode::kInvalidParameters};
  EXPECT_TRUE(ReceiveAndExpect(kPairingPublicKeyCmd, kExpectedFailure));
  EXPECT_EQ(1, listener()->pairing_error_count());
}

TEST_F(Phase2SecureConnectionsTest, RejectsPublicKeyWithNegatedY) {
  // Read local key sent to peer
  fake_chan()->SetSendCallback(
      [this](ByteBufferPtr sdu) {
        auto reader = ValidPacketReader::ParseSdu(sdu).value();
        if (reader.code() == kPairingPublicKey) {
          mut_local_key() = EcdhKey::ParseFromPublicKey(
              reader.payload<PairingPublicKeyParams>());
        }
      },
      dispatcher());

  phase_2_sc()->Start();
  RunUntilIdle();
  ASSERT_TRUE(local_key().has_value());

  auto local_serialized = local_key()->GetSerializedPublicKey();

  // P-256 prime in Little Endian
  const std::array<uint8_t, 32> p = {
      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
      0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};

  // Calculate the negated Y coordinate of the local key
  PairingPublicKeyParams negated_key = local_serialized;
  int borrow = 0;
  for (size_t i = 0; i < 32; ++i) {
    int diff = p[i] - local_serialized.y[i] - borrow;
    if (diff < 0) {
      diff += 256;
      borrow = 1;
    } else {
      borrow = 0;
    }
    negated_key.y[i] = static_cast<uint8_t>(diff);
  }

  const auto kPairingPublicKeyCmd = MakeCmd(kPairingPublicKey, negated_key);
  const StaticByteBuffer kExpectedFailure{kPairingFailed,
                                          ErrorCode::kInvalidParameters};
  EXPECT_TRUE(ReceiveAndExpect(kPairingPublicKeyCmd, kExpectedFailure));
  EXPECT_EQ(1, listener()->pairing_error_count());
}

TEST_F(Phase2SecureConnectionsTest, ReceivePeerPublicKeyTwice) {
  phase_2_sc()->Start();
  const auto kPairingPublicKeyCmd =
      MakeCmd(kPairingPublicKey, peer_key().GetSerializedPublicKey());
  fake_chan()->Receive(kPairingPublicKeyCmd);
  const StaticByteBuffer<PacketSize<ErrorCode>()> kExpectedFailure{
      kPairingFailed, ErrorCode::kUnspecifiedReason};
  ASSERT_TRUE(ReceiveAndExpect(kPairingPublicKeyCmd, kExpectedFailure));
  ASSERT_EQ(1, listener()->pairing_error_count());
}

TEST_F(Phase2SecureConnectionsTest, ReceiveConfirmValueBeforeStage1Fails) {
  phase_2_sc()->Start();
  const auto kPairingConfirmCmd =
      MakeCmd(kPairingConfirm, PairingConfirmValue{1});
  const StaticByteBuffer<PacketSize<ErrorCode>()> kExpectedFailure{
      kPairingFailed, ErrorCode::kUnspecifiedReason};
  ASSERT_TRUE(ReceiveAndExpect(kPairingConfirmCmd, kExpectedFailure));
  ASSERT_EQ(1, listener()->pairing_error_count());
}

TEST_F(Phase2SecureConnectionsTest, ReceiveRandomValueBeforeStage1Fails) {
  phase_2_sc()->Start();
  const auto kPairingRandomCmd = MakeCmd(kPairingRandom, PairingRandomValue{1});
  const StaticByteBuffer<PacketSize<ErrorCode>()> kExpectedFailure{
      kPairingFailed, ErrorCode::kUnspecifiedReason};
  ASSERT_TRUE(ReceiveAndExpect(kPairingRandomCmd, kExpectedFailure));
  ASSERT_EQ(1, listener()->pairing_error_count());
}

TEST_F(Phase2SecureConnectionsTest, ReceiveDhKeyCheckValueBeforeStage1Fails) {
  phase_2_sc()->Start();
  const auto kPairingDHKeyCheckCmd =
      MakeCmd(kPairingDHKeyCheck, PairingDHKeyCheckValueE{1});
  const StaticByteBuffer<PacketSize<ErrorCode>()> kExpectedFailure{
      kPairingFailed, ErrorCode::kUnspecifiedReason};
  ASSERT_TRUE(ReceiveAndExpect(kPairingDHKeyCheckCmd, kExpectedFailure));
  ASSERT_EQ(1, listener()->pairing_error_count());
}

TEST_F(Phase2SecureConnectionsTest, ReceiveConfirmValueAfterStage1Fails) {
  FastForwardToDhKeyCheck();
  const auto kPairingConfirmCmd =
      MakeCmd(kPairingConfirm, PairingConfirmValue{1});
  const StaticByteBuffer<PacketSize<ErrorCode>()> kExpectedFailure{
      kPairingFailed, ErrorCode::kUnspecifiedReason};
  ASSERT_TRUE(ReceiveAndExpect(kPairingConfirmCmd, kExpectedFailure));
  ASSERT_EQ(1, listener()->pairing_error_count());
}

TEST_F(Phase2SecureConnectionsTest, ReceiveRandomValueAfterStage1Fails) {
  FastForwardToDhKeyCheck();
  const auto kPairingRandomCmd = MakeCmd(kPairingRandom, PairingRandomValue{1});
  const StaticByteBuffer<PacketSize<ErrorCode>()> kExpectedFailure{
      kPairingFailed, ErrorCode::kUnspecifiedReason};
  ASSERT_TRUE(ReceiveAndExpect(kPairingRandomCmd, kExpectedFailure));
  ASSERT_EQ(1, listener()->pairing_error_count());
}

TEST_F(Phase2SecureConnectionsTest,
       InitiatorReceiveDhKeyCheckWhileWaitingForConfirmFails) {
  NewPhase2SecureConnections(Role::kInitiator);
  ConfirmCallback confirm_cb = nullptr;
  listener()->set_confirm_delegate(
      [&](ConfirmCallback cb) { confirm_cb = std::move(cb); });
  LtkAndChecks expected_stage2_vals = FastForwardToDhKeyCheck();
  ASSERT_TRUE(confirm_cb);
  // Receiving the peer DHKey check before user confirmation should fail as
  // initiator.
  const auto kPairingDHKeyCheckCmd =
      MakeCmd(kPairingDHKeyCheck, expected_stage2_vals.dhkey_check_b);
  const StaticByteBuffer<PacketSize<ErrorCode>()> kExpectedFailure{
      kPairingFailed, ErrorCode::kUnspecifiedReason};
  ASSERT_TRUE(ReceiveAndExpect(kPairingDHKeyCheckCmd, kExpectedFailure));
  ASSERT_EQ(1, listener()->pairing_error_count());
}

TEST_F(Phase2SecureConnectionsTest, Stage1JustWorksErrorPropagates) {
  NewPhase2SecureConnections(Role::kInitiator, PairingMethod::kJustWorks);
  ConfirmCallback confirm_cb = nullptr;
  listener()->set_confirm_delegate(
      [&](ConfirmCallback cb) { confirm_cb = std::move(cb); });
  FastForwardPublicKeyExchange();

  Code sent_code = kPairingFailed;
  fake_chan()->SetSendCallback(
      [&](ByteBufferPtr sdu) {
        std::tie(sent_code, std::ignore) =
            ExtractCodeAnd128BitCmd(std::move(sdu));
      },
      dispatcher());
  MatchingPair stage1_vals = GenerateMatchingConfirmAndRandom();
  ReceiveCmd<PairingConfirmValue>(kPairingConfirm, stage1_vals.confirm);
  RunUntilIdle();
  ASSERT_EQ(kPairingRandom, sent_code);
  UInt128 mismatched_random = stage1_vals.random;
  mismatched_random[0] += 1;
  const auto kMismatchedPairingRandomCmd =
      MakeCmd(kPairingRandom, mismatched_random);
  const StaticByteBuffer<PacketSize<ErrorCode>()> kExpectedFailure{
      kPairingFailed, ErrorCode::kConfirmValueFailed};
  ASSERT_TRUE(ReceiveAndExpect(kMismatchedPairingRandomCmd, kExpectedFailure));
  ASSERT_EQ(1, listener()->pairing_error_count());
}

TEST_F(Phase2SecureConnectionsTest, Stage1PasskeyErrorPropagates) {
  NewPhase2SecureConnections(Role::kInitiator,
                             PairingMethod::kPasskeyEntryDisplay);
  uint32_t passkey;
  ConfirmCallback confirm_cb = nullptr;
  listener()->set_display_delegate([&](uint32_t disp_passkey,
                                       Delegate::DisplayMethod method,
                                       ConfirmCallback cb) {
    ASSERT_EQ(Delegate::DisplayMethod::kPeerEntry, method);
    confirm_cb = std::move(cb);
    passkey = disp_passkey;
  });
  FastForwardPublicKeyExchange();

  ASSERT_TRUE(confirm_cb);
  const StaticByteBuffer<PacketSize<ErrorCode>()> kExpectedFailure{
      kPairingFailed, ErrorCode::kPasskeyEntryFailed};
  bool failure_sent = false;
  fake_chan()->SetSendCallback(
      [&](ByteBufferPtr sdu) {
        ASSERT_TRUE(ContainersEqual(kExpectedFailure, *sdu));
        failure_sent = true;
      },
      dispatcher());
  confirm_cb(false);
  RunUntilIdle();
  ASSERT_EQ(1, listener()->pairing_error_count());
  ASSERT_TRUE(failure_sent);
}

TEST_F(Phase2SecureConnectionsTest, InitiatorReceiveWrongDhKeyCheckFails) {
  NewPhase2SecureConnections(Role::kInitiator);
  ConfirmCallback confirm_cb = nullptr;
  listener()->set_confirm_delegate(
      [&](ConfirmCallback cb) { confirm_cb = std::move(cb); });
  LtkAndChecks expected_stage2_vals = FastForwardToDhKeyCheck();
  ASSERT_TRUE(confirm_cb);
  Code sent_code = kPairingFailed;
  UInt128 sent_payload;
  fake_chan()->SetSendCallback(
      [&](ByteBufferPtr sdu) {
        std::tie(sent_code, sent_payload) =
            ExtractCodeAnd128BitCmd(std::move(sdu));
      },
      dispatcher());
  confirm_cb(true);
  RunUntilIdle();
  ASSERT_EQ(kPairingDHKeyCheck, sent_code);
  // As initiator, we expect the dhkey_check_b value, not the a.
  const auto kPairingDHKeyCheckCmd =
      MakeCmd(kPairingDHKeyCheck, expected_stage2_vals.dhkey_check_a);
  const StaticByteBuffer<PacketSize<ErrorCode>()> kExpectedFailure{
      kPairingFailed, ErrorCode::kDHKeyCheckFailed};
  ASSERT_TRUE(ReceiveAndExpect(kPairingDHKeyCheckCmd, kExpectedFailure));
  ASSERT_EQ(1, listener()->pairing_error_count());
  ASSERT_EQ(0, phase_2_complete_count());
}

TEST_F(Phase2SecureConnectionsTest, InitiatorFlowSuccessJustWorks) {
  NewPhase2SecureConnections(Role::kInitiator, PairingMethod::kJustWorks);
  ConfirmCallback confirm_cb = nullptr;
  listener()->set_confirm_delegate(
      [&](ConfirmCallback cb) { confirm_cb = std::move(cb); });
  FastForwardPublicKeyExchange();

  Code sent_code = kPairingFailed;
  UInt128 sent_payload;
  fake_chan()->SetSendCallback(
      [&](ByteBufferPtr sdu) {
        std::tie(sent_code, sent_payload) =
            ExtractCodeAnd128BitCmd(std::move(sdu));
      },
      dispatcher());
  MatchingPair stage1_vals = GenerateMatchingConfirmAndRandom();
  ReceiveCmd<PairingConfirmValue>(kPairingConfirm, stage1_vals.confirm);
  RunUntilIdle();
  ASSERT_EQ(kPairingRandom, sent_code);
  UInt128 local_rand = sent_payload;
  ASSERT_FALSE(confirm_cb);
  ReceiveCmd(kPairingRandom, stage1_vals.random);
  RunUntilIdle();
  ASSERT_TRUE(confirm_cb);

  LtkAndChecks expected_stage2_vals =
      GenerateLtkAndChecks(local_rand, stage1_vals.random);
  confirm_cb(true);
  RunUntilIdle();
  // After receiving user confirmation, we should send (the correct) DHKey Check
  // Ea
  ASSERT_EQ(kPairingDHKeyCheck, sent_code);
  ASSERT_EQ(expected_stage2_vals.dhkey_check_a, sent_payload);
  ReceiveCmd(kPairingDHKeyCheck, expected_stage2_vals.dhkey_check_b);
  RunUntilIdle();
  ASSERT_EQ(1, phase_2_complete_count());
  // We should generate the same LTK on "both sides"
  ASSERT_EQ(expected_stage2_vals.ltk, ltk());
}

TEST_F(Phase2SecureConnectionsTest, InitiatorFlowSuccessNumericComparison) {
  NewPhase2SecureConnections(Role::kInitiator,
                             PairingMethod::kNumericComparison);
  ConfirmCallback confirm_cb = nullptr;
  listener()->set_display_delegate(
      [&](uint32_t, Delegate::DisplayMethod method, ConfirmCallback cb) {
        ASSERT_EQ(Delegate::DisplayMethod::kComparison, method);
        confirm_cb = std::move(cb);
      });
  FastForwardPublicKeyExchange();

  Code sent_code = kPairingFailed;
  UInt128 sent_payload;
  fake_chan()->SetSendCallback(
      [&](ByteBufferPtr sdu) {
        std::tie(sent_code, sent_payload) =
            ExtractCodeAnd128BitCmd(std::move(sdu));
      },
      dispatcher());
  MatchingPair stage1_vals = GenerateMatchingConfirmAndRandom();
  ReceiveCmd(kPairingConfirm, stage1_vals.confirm);
  RunUntilIdle();
  EXPECT_EQ(kPairingRandom, sent_code);
  UInt128 initiator_random = sent_payload;
  ASSERT_FALSE(confirm_cb);

  ReceiveCmd(kPairingRandom, stage1_vals.random);
  RunUntilIdle();
  ASSERT_TRUE(confirm_cb);
  confirm_cb(true);
  RunUntilIdle();
  LtkAndChecks vals =
      GenerateLtkAndChecks(initiator_random, stage1_vals.random);
  EXPECT_EQ(kPairingDHKeyCheck, sent_code);
  EXPECT_EQ(vals.dhkey_check_a, sent_payload);
  ReceiveCmd(kPairingDHKeyCheck, vals.dhkey_check_b);
  RunUntilIdle();
  ASSERT_EQ(vals.ltk, ltk());
}

TEST_F(Phase2SecureConnectionsTest, InitiatorFlowSuccessPasskeyEntryDisplay) {
  NewPhase2SecureConnections(Role::kInitiator,
                             PairingMethod::kPasskeyEntryDisplay);
  uint32_t passkey;
  ConfirmCallback confirm_cb = nullptr;
  listener()->set_display_delegate([&](uint32_t disp_passkey,
                                       Delegate::DisplayMethod method,
                                       ConfirmCallback cb) {
    ASSERT_EQ(Delegate::DisplayMethod::kPeerEntry, method);
    confirm_cb = std::move(cb);
    passkey = disp_passkey;
  });
  FastForwardPublicKeyExchange();
  ASSERT_TRUE(confirm_cb);
  Code sent_code = kPairingFailed;
  UInt128 sent_payload;
  fake_chan()->SetSendCallback(
      [&](ByteBufferPtr sdu) {
        std::tie(sent_code, sent_payload) =
            ExtractCodeAnd128BitCmd(std::move(sdu));
      },
      dispatcher());
  confirm_cb(true);
  MatchingPair stage1_vals;
  UInt128 last_sent_rand;
  for (size_t i = 0; i < 20; ++i) {
    const uint8_t r = (passkey & (1 << i)) ? 0x81 : 0x80;
    stage1_vals = GenerateMatchingConfirmAndRandom(r);
    RunUntilIdle();

    ASSERT_EQ(kPairingConfirm, sent_code);
    PairingConfirmValue init_confirm = sent_payload;
    ReceiveCmd(kPairingConfirm, stage1_vals.confirm);
    RunUntilIdle();

    ASSERT_EQ(kPairingRandom, sent_code);
    last_sent_rand = sent_payload;
    EXPECT_EQ(
        GenerateConfirmValue(sent_payload, /*gen_initiator_confirm=*/true, r),
        init_confirm);
    ReceiveCmd(kPairingRandom, stage1_vals.random);
  }
  LtkAndChecks vals = GenerateLtkAndChecks(
      last_sent_rand, stage1_vals.random, uint64_t{passkey});
  RunUntilIdle();
  EXPECT_EQ(kPairingDHKeyCheck, sent_code);
  EXPECT_EQ(vals.dhkey_check_a, sent_payload);
  ReceiveCmd(kPairingDHKeyCheck, vals.dhkey_check_b);
  RunUntilIdle();
  ASSERT_EQ(vals.ltk, ltk());
}

TEST_F(Phase2SecureConnectionsTest, InitiatorFlowSuccessPasskeyEntryInput) {
  NewPhase2SecureConnections(Role::kInitiator,
                             PairingMethod::kPasskeyEntryInput);
  PasskeyResponseCallback passkey_cb = nullptr;
  listener()->set_request_passkey_delegate(
      [&](PasskeyResponseCallback cb) { passkey_cb = std::move(cb); });
  FastForwardPublicKeyExchange();
  ASSERT_TRUE(passkey_cb);
  Code sent_code = kPairingFailed;
  UInt128 sent_payload;
  fake_chan()->SetSendCallback(
      [&](ByteBufferPtr sdu) {
        std::tie(sent_code, sent_payload) =
            ExtractCodeAnd128BitCmd(std::move(sdu));
      },
      dispatcher());
  const int64_t passkey = 123456;
  passkey_cb(passkey);
  MatchingPair stage1_vals;
  UInt128 last_sent_rand;
  for (size_t i = 0; i < 20; ++i) {
    const uint8_t r = (passkey & (1 << i)) ? 0x81 : 0x80;
    stage1_vals = GenerateMatchingConfirmAndRandom(r);
    RunUntilIdle();

    ASSERT_EQ(kPairingConfirm, sent_code);
    PairingConfirmValue init_confirm = sent_payload;
    ReceiveCmd(kPairingConfirm, stage1_vals.confirm);
    RunUntilIdle();

    ASSERT_EQ(kPairingRandom, sent_code);
    last_sent_rand = sent_payload;
    EXPECT_EQ(
        GenerateConfirmValue(sent_payload, /*gen_initiator_confirm=*/true, r),
        init_confirm);
    ReceiveCmd(kPairingRandom, stage1_vals.random);
  }
  LtkAndChecks vals = GenerateLtkAndChecks(
      last_sent_rand, stage1_vals.random, uint64_t{passkey});
  RunUntilIdle();
  EXPECT_EQ(kPairingDHKeyCheck, sent_code);
  EXPECT_EQ(vals.dhkey_check_a, sent_payload);
  ReceiveCmd(kPairingDHKeyCheck, vals.dhkey_check_b);
  RunUntilIdle();
  ASSERT_EQ(vals.ltk, ltk());
}

TEST_F(Phase2SecureConnectionsTest, ResponderReceiveWrongDhKeyCheckFails) {
  NewPhase2SecureConnections(Role::kResponder);
  ConfirmCallback confirm_cb = nullptr;
  listener()->set_confirm_delegate(
      [&](ConfirmCallback cb) { confirm_cb = std::move(cb); });
  LtkAndChecks expected_stage2_vals = FastForwardToDhKeyCheck();
  ASSERT_TRUE(confirm_cb);
  Code sent_code = kPairingFailed;
  UInt128 sent_payload;
  fake_chan()->SetSendCallback(
      [&](ByteBufferPtr sdu) {
        std::tie(sent_code, sent_payload) =
            ExtractCodeAnd128BitCmd(std::move(sdu));
      },
      dispatcher());
  confirm_cb(true);
  RunUntilIdle();
  // As responder, we expect the dhkey_check_a value, not the b.
  const auto kPairingDHKeyCheckCmdWithBValue =
      MakeCmd(kPairingDHKeyCheck, expected_stage2_vals.dhkey_check_b);
  const StaticByteBuffer<PacketSize<ErrorCode>()> kExpectedFailure{
      kPairingFailed, ErrorCode::kDHKeyCheckFailed};
  ASSERT_TRUE(
      ReceiveAndExpect(kPairingDHKeyCheckCmdWithBValue, kExpectedFailure));
  ASSERT_EQ(1, listener()->pairing_error_count());
  ASSERT_EQ(0, phase_2_complete_count());
}

TEST_F(Phase2SecureConnectionsTest, ResponderFlowSuccessJustWorks) {
  NewPhase2SecureConnections(Role::kResponder, PairingMethod::kJustWorks);
  ConfirmCallback confirm_cb = nullptr;
  listener()->set_confirm_delegate(
      [&](ConfirmCallback cb) { confirm_cb = std::move(cb); });
  PairingConfirmValue responder_confirm = *FastForwardPublicKeyExchange();

  Code sent_code = kPairingFailed;
  UInt128 sent_payload;
  fake_chan()->SetSendCallback(
      [&](ByteBufferPtr sdu) {
        std::tie(sent_code, sent_payload) =
            ExtractCodeAnd128BitCmd(std::move(sdu));
      },
      dispatcher());
  ASSERT_FALSE(confirm_cb);
  const PairingRandomValue kInitiatorRand{1};
  ReceiveCmd(kPairingRandom, kInitiatorRand);
  RunUntilIdle();

  ASSERT_EQ(kPairingRandom, sent_code);
  UInt128 responder_rand = sent_payload;
  ASSERT_EQ(
      GenerateConfirmValue(responder_rand, /*gen_initiator_confirm=*/false),
      responder_confirm);
  ASSERT_TRUE(confirm_cb);
  LtkAndChecks expected_stage2_vals =
      GenerateLtkAndChecks(kInitiatorRand, responder_rand);
  // After receiving user confirmation & the peer dhkey check, we should send
  // the DHKey Check Eb.
  confirm_cb(true);
  ReceiveCmd(kPairingDHKeyCheck, expected_stage2_vals.dhkey_check_a);
  RunUntilIdle();
  ASSERT_EQ(kPairingDHKeyCheck, sent_code);
  ASSERT_EQ(expected_stage2_vals.dhkey_check_b, sent_payload);
  ASSERT_EQ(1, phase_2_complete_count());
  // We should generate the same LTK on "both sides"
  ASSERT_EQ(expected_stage2_vals.ltk, ltk());
}

TEST_F(Phase2SecureConnectionsTest,
       ResponderReceiveDhKeyCheckWhileWaitingForConfirmSuccess) {
  NewPhase2SecureConnections(Role::kResponder, PairingMethod::kJustWorks);
  ConfirmCallback confirm_cb = nullptr;
  listener()->set_confirm_delegate(
      [&](ConfirmCallback cb) { confirm_cb = std::move(cb); });
  LtkAndChecks expected_stage2_vals = FastForwardToDhKeyCheck();

  Code sent_code;
  PairingDHKeyCheckValueE sent_dhkey_check;
  fake_chan()->SetSendCallback(
      [&](ByteBufferPtr sdu) {
        std::tie(sent_code, sent_dhkey_check) =
            ExtractCodeAnd128BitCmd(std::move(sdu));
      },
      dispatcher());
  ASSERT_TRUE(confirm_cb);
  // Receiving the peer DHKey check before user confirmation should work as
  // responder.
  ReceiveCmd(kPairingDHKeyCheck, expected_stage2_vals.dhkey_check_a);
  RunUntilIdle();
  confirm_cb(true);
  RunUntilIdle();

  ASSERT_EQ(kPairingDHKeyCheck, sent_code);
  ASSERT_EQ(expected_stage2_vals.dhkey_check_b, sent_dhkey_check);
  ASSERT_EQ(1, phase_2_complete_count());
}

}  // namespace
}  // namespace bt::sm
