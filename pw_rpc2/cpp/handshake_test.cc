// Copyright 2026 The Pigweed Authors
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

#include "pw_rpc2/internal/handshake.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include "pw_allocator/testing.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/poll.h"
#include "pw_async2/task.h"
#include "pw_bytes/endian.h"
#include "pw_rpc2/internal/packet.h"
#include "pw_rpc2/internal/test_utils.h"
#include "pw_status/status.h"
#include "pw_transport/socket.h"
#include "pw_unit_test/framework.h"

namespace pw::rpc2::internal {
namespace {

template <typename FutureType>
class HandshakeTestTask : public async2::Task {
 public:
  explicit HandshakeTestTask(FutureType fut) : fut_(std::move(fut)) {}
  ~HandshakeTestTask() override { Deregister(); }

  async2::Poll<> DoPend(async2::Context& cx) override {
    auto poll = fut_.Pend(cx);
    if (poll.IsPending()) {
      return async2::Pending();
    }
    result_ = std::move(*poll);
    return async2::Ready();
  }

  const std::optional<Result<EstablishedConnection>>& result() const {
    return result_;
  }
  FutureType& future() { return fut_; }

 private:
  FutureType fut_;
  std::optional<Result<EstablishedConnection>> result_;
};

template <typename FutureType>
HandshakeTestTask(FutureType) -> HandshakeTestTask<FutureType>;

// Encodes a handshake packet as a hypothetical newer protocol version might
// send it: `extra_bytes` of trailing data and a non-zero reserved field, both
// of which this implementation must ignore.
Buf EncodeNewerPeerHandshake(Allocator& allocator,
                             HandshakePacket::Type type,
                             uint8_t version,
                             size_t extra_bytes) {
  Buf buf =
      Buf::Allocate(allocator, HandshakePacket::kWireSizeBytes + extra_bytes);
  EXPECT_EQ(buf.size(), HandshakePacket::kWireSizeBytes + extra_bytes);
  PW_TEST_EXPECT_OK(HandshakePacket(type, version).Encode(ByteSpan(buf)));
  buf[offsetof(HandshakeWireFormat, reserved)] = std::byte{0xa5};
  buf[offsetof(HandshakeWireFormat, reserved) + 1] = std::byte{0x5a};
  for (size_t i = HandshakePacket::kWireSizeBytes; i < buf.size(); ++i) {
    buf[i] = std::byte{0xee};
  }
  return buf;
}

TEST(HandshakeTest, ConstexprDefaultConstructor) {
  // A default-constructed handshake future is empty: it does not represent a
  // handshake, so it can neither be pended nor report completion.
  PW_CONSTINIT static InitiatorHandshakeFuture default_init;
  EXPECT_FALSE(default_init.is_complete());
  EXPECT_FALSE(default_init.is_pendable());

  PW_CONSTINIT static ResponderHandshakeFuture default_resp;
  EXPECT_FALSE(default_resp.is_complete());
  EXPECT_FALSE(default_resp.is_pendable());
}

TEST(HandshakeTest, EncodeDecodeHandshakePacketDirect) {
  std::byte buffer[HandshakePacket::kWireSizeBytes];
  const HandshakePacket syn(HandshakePacket::Type::kSyn);
  EXPECT_EQ(syn.Encode(buffer), OkStatus());

  std::byte small_buf[HandshakePacket::kWireSizeBytes - 1];
  EXPECT_EQ(syn.Encode(small_buf), Status::ResourceExhausted());

  auto dec_res = HandshakePacket::Decode(buffer);
  ASSERT_TRUE(dec_res.ok());
  EXPECT_EQ(dec_res->type(), HandshakePacket::Type::kSyn);
  EXPECT_EQ(dec_res->version(), 1u);
}

TEST(HandshakeTest, InitiatorAndResponderSuccessful3WayHandshake) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto paired = test::MakePairedConnections(allocator);

  HandshakeTestTask initiator_task(InitiatorHandshakeFuture(paired.first));
  HandshakeTestTask responder_task(ResponderHandshakeFuture(paired.second));

  dispatcher.Post(initiator_task);
  dispatcher.Post(responder_task);

  dispatcher.RunUntilStalled();

  ASSERT_TRUE(initiator_task.result().has_value());
  ASSERT_TRUE(initiator_task.result()->ok());
  EXPECT_EQ(initiator_task.result()->value().info.negotiated_version, 1u);
  EXPECT_TRUE(initiator_task.result()->value().connection);

  ASSERT_TRUE(responder_task.result().has_value());
  ASSERT_TRUE(responder_task.result()->ok());
  EXPECT_EQ(responder_task.result()->value().info.negotiated_version, 1u);
  EXPECT_TRUE(responder_task.result()->value().connection);

  // Initiator committed 2 packets: SYN, ACK
  EXPECT_EQ(paired.first_raw->commit_count(), 2u);
  // Responder committed 1 packet: SYN-ACK
  EXPECT_EQ(paired.second_raw->commit_count(), 1u);
}

TEST(HandshakeTest, VersionNegotiationInitiatorHigher) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto paired = test::MakePairedConnections(allocator);

  HandshakeTestTask initiator_task(
      InitiatorHandshakeFuture(paired.first, HandshakeInfo{2}));
  HandshakeTestTask responder_task(
      ResponderHandshakeFuture(paired.second, HandshakeInfo{1}));

  dispatcher.Post(initiator_task);
  dispatcher.Post(responder_task);

  dispatcher.RunUntilStalled();

  ASSERT_TRUE(initiator_task.result().has_value());
  ASSERT_TRUE(initiator_task.result()->ok());
  EXPECT_EQ(initiator_task.result()->value().info.negotiated_version, 1u);
  EXPECT_TRUE(initiator_task.result()->value().connection);

  ASSERT_TRUE(responder_task.result().has_value());
  ASSERT_TRUE(responder_task.result()->ok());
  EXPECT_EQ(responder_task.result()->value().info.negotiated_version, 1u);
  EXPECT_TRUE(responder_task.result()->value().connection);
}

TEST(HandshakeTest, VersionNegotiationResponderHigher) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto paired = test::MakePairedConnections(allocator);

  HandshakeTestTask initiator_task(
      InitiatorHandshakeFuture(paired.first, HandshakeInfo{1}));
  HandshakeTestTask responder_task(
      ResponderHandshakeFuture(paired.second, HandshakeInfo{2}));

  dispatcher.Post(initiator_task);
  dispatcher.Post(responder_task);

  dispatcher.RunUntilStalled();

  ASSERT_TRUE(initiator_task.result().has_value());
  ASSERT_TRUE(initiator_task.result()->ok());
  EXPECT_EQ(initiator_task.result()->value().info.negotiated_version, 1u);
  EXPECT_TRUE(initiator_task.result()->value().connection);

  ASSERT_TRUE(responder_task.result().has_value());
  ASSERT_TRUE(responder_task.result()->ok());
  EXPECT_EQ(responder_task.result()->value().info.negotiated_version, 1u);
  EXPECT_TRUE(responder_task.result()->value().connection);
}

TEST(HandshakeTest, StepByStepInitiatorAndResponder) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto [client_conn, raw_client] = test::MakeMockConnection(allocator);
  auto [server_conn, raw_server] = test::MakeMockConnection(allocator);

  HandshakeTestTask initiator_task(InitiatorHandshakeFuture{client_conn});
  HandshakeTestTask responder_task(ResponderHandshakeFuture{server_conn});

  dispatcher.Post(initiator_task);
  dispatcher.Post(responder_task);

  // Poll 1: Initiator sends SYN, Responder waits for SYN
  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_client->commit_count(), 1u);
  EXPECT_EQ(raw_server->commit_count(), 0u);
  EXPECT_FALSE(initiator_task.result().has_value());
  EXPECT_FALSE(responder_task.result().has_value());

  auto syn_pkt = HandshakePacket::Decode(raw_client->last_written_buf());
  ASSERT_TRUE(syn_pkt.ok());
  EXPECT_EQ(syn_pkt->version(), 1u);
  EXPECT_EQ(syn_pkt->type(), HandshakePacket::Type::kSyn);

  // Deliver SYN to Responder
  Buf syn_buf = Buf::Allocate(allocator, HandshakePacket::kWireSizeBytes);
  auto enc_syn = syn_pkt->Encode(std::move(syn_buf));
  ASSERT_TRUE(enc_syn.ok());
  raw_server->SetNextRead(std::move(enc_syn.value()));

  // Poll 2: Responder processes SYN, sends SYN-ACK, awaits ACK
  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_server->commit_count(), 1u);
  EXPECT_FALSE(responder_task.result().has_value());

  auto syn_ack_pkt = HandshakePacket::Decode(raw_server->last_written_buf());
  ASSERT_TRUE(syn_ack_pkt.ok());
  EXPECT_EQ(syn_ack_pkt->version(), 1u);
  EXPECT_EQ(syn_ack_pkt->type(), HandshakePacket::Type::kSynAck);

  // Deliver SYN-ACK to Initiator
  Buf syn_ack_buf = Buf::Allocate(allocator, HandshakePacket::kWireSizeBytes);
  auto enc_syn_ack = syn_ack_pkt->Encode(std::move(syn_ack_buf));
  ASSERT_TRUE(enc_syn_ack.ok());
  raw_client->SetNextRead(std::move(enc_syn_ack.value()));

  // Poll 3: Initiator processes SYN-ACK, sends ACK, completes!
  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_client->commit_count(), 2u);
  ASSERT_TRUE(initiator_task.result().has_value());
  ASSERT_TRUE(initiator_task.result()->ok());
  EXPECT_EQ(initiator_task.result()->value().info.negotiated_version, 1u);
  EXPECT_TRUE(initiator_task.result()->value().connection);
  EXPECT_FALSE(responder_task.result().has_value());

  auto ack_pkt = HandshakePacket::Decode(raw_client->last_written_buf());
  ASSERT_TRUE(ack_pkt.ok());
  EXPECT_EQ(ack_pkt->version(), 1u);
  EXPECT_EQ(ack_pkt->type(), HandshakePacket::Type::kAck);

  // Deliver ACK to Responder
  Buf ack_buf = Buf::Allocate(allocator, HandshakePacket::kWireSizeBytes);
  auto enc_ack = ack_pkt->Encode(std::move(ack_buf));
  ASSERT_TRUE(enc_ack.ok());
  raw_server->SetNextRead(std::move(enc_ack.value()));

  // Poll 4: Responder processes ACK, completes!
  dispatcher.RunUntilStalled();
  ASSERT_TRUE(responder_task.result().has_value());
  ASSERT_TRUE(responder_task.result()->ok());
  EXPECT_EQ(responder_task.result()->value().info.negotiated_version, 1u);
  EXPECT_TRUE(responder_task.result()->value().connection);
}

TEST(HandshakeTest, ResponderRejectsInvalidMagicInSyn) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  HandshakeTestTask responder_task(ResponderHandshakeFuture{conn});
  dispatcher.Post(responder_task);

  dispatcher.RunUntilStalled();

  // Send packet with bad magic
  Buf bad_magic_buf = Buf::Allocate(allocator, HandshakePacket::kWireSizeBytes);
  auto enc_res = HandshakePacket(HandshakePacket::Type::kSyn)
                     .Encode(ByteSpan(bad_magic_buf));
  ASSERT_TRUE(enc_res.ok());
  bad_magic_buf[0] ^= std::byte{0xff};
  raw_conn->SetNextRead(std::move(bad_magic_buf));

  dispatcher.RunUntilStalled();
  ASSERT_TRUE(responder_task.result().has_value());
  EXPECT_EQ(responder_task.result()->status(), Status::DataLoss());
}

TEST(HandshakeTest, InitiatorRejectsInvalidMagicInSynAck) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  HandshakeTestTask initiator_task(InitiatorHandshakeFuture{conn});
  dispatcher.Post(initiator_task);

  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_conn->commit_count(), 1u);

  // Send SYN-ACK with bad magic
  Buf bad_buf = Buf::Allocate(allocator, HandshakePacket::kWireSizeBytes);
  auto enc_res =
      HandshakePacket(HandshakePacket::Type::kSynAck).Encode(ByteSpan(bad_buf));
  ASSERT_TRUE(enc_res.ok());
  bad_buf[0] ^= std::byte{0xff};
  raw_conn->SetNextRead(std::move(bad_buf));

  dispatcher.RunUntilStalled();
  ASSERT_TRUE(initiator_task.result().has_value());
  EXPECT_EQ(initiator_task.result()->status(), Status::DataLoss());
}

TEST(HandshakeTest, InitiatorRejectsHigherVersionInSynAck) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  HandshakeTestTask initiator_task(
      InitiatorHandshakeFuture{conn, HandshakeInfo{1}});
  dispatcher.Post(initiator_task);

  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_conn->commit_count(), 1u);

  // Responder sends SYN-ACK with version 2
  Buf syn_ack_buf = Buf::Allocate(allocator, HandshakePacket::kWireSizeBytes);
  auto enc_syn_ack = HandshakePacket(HandshakePacket::Type::kSynAck, 2)
                         .Encode(ByteSpan(syn_ack_buf));
  ASSERT_TRUE(enc_syn_ack.ok());
  raw_conn->SetNextRead(std::move(syn_ack_buf));

  dispatcher.RunUntilStalled();
  ASSERT_TRUE(initiator_task.result().has_value());
  EXPECT_EQ(initiator_task.result()->status(), Status::DataLoss());
}

TEST(HandshakeTest, InitiatorRejectsLoopbackSynInSynAckStage) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  HandshakeTestTask initiator_task(InitiatorHandshakeFuture{conn});
  dispatcher.Post(initiator_task);

  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_conn->commit_count(), 1u);

  // Loopback / simultaneous open: Initiator receives SYN instead of SYN-ACK
  Buf syn_buf = Buf::Allocate(allocator, HandshakePacket::kWireSizeBytes);
  auto enc_syn =
      HandshakePacket(HandshakePacket::Type::kSyn).Encode(std::move(syn_buf));
  ASSERT_TRUE(enc_syn.ok());
  raw_conn->SetNextRead(std::move(enc_syn.value()));

  dispatcher.RunUntilStalled();
  ASSERT_TRUE(initiator_task.result().has_value());
  EXPECT_EQ(initiator_task.result()->status(), Status::DataLoss());
}

TEST(HandshakeTest, ResponderRejectsMismatchedVersionInAck) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  HandshakeTestTask responder_task(ResponderHandshakeFuture{conn});
  dispatcher.Post(responder_task);

  dispatcher.RunUntilStalled();

  // Send valid SYN with version 1
  Buf syn_buf = Buf::Allocate(allocator, HandshakePacket::kWireSizeBytes);
  auto enc_syn =
      HandshakePacket(HandshakePacket::Type::kSyn).Encode(std::move(syn_buf));
  ASSERT_TRUE(enc_syn.ok());
  raw_conn->SetNextRead(std::move(enc_syn.value()));

  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_conn->commit_count(), 1u);

  // Send ACK with mismatched version 2 (when negotiated was 1)
  Buf ack_buf = Buf::Allocate(allocator, HandshakePacket::kWireSizeBytes);
  auto enc_ack =
      HandshakePacket(HandshakePacket::Type::kAck, 2).Encode(ByteSpan(ack_buf));
  ASSERT_TRUE(enc_ack.ok());
  raw_conn->SetNextRead(std::move(ack_buf));

  dispatcher.RunUntilStalled();
  ASSERT_TRUE(responder_task.result().has_value());
  EXPECT_EQ(responder_task.result()->status(), Status::DataLoss());
}

TEST(HandshakeTest, ResponderRejectsInvalidMagicInAck) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  HandshakeTestTask responder_task(ResponderHandshakeFuture{conn});
  dispatcher.Post(responder_task);

  dispatcher.RunUntilStalled();

  // Send valid SYN with version 1
  Buf syn_buf = Buf::Allocate(allocator, HandshakePacket::kWireSizeBytes);
  auto enc_syn =
      HandshakePacket(HandshakePacket::Type::kSyn).Encode(std::move(syn_buf));
  ASSERT_TRUE(enc_syn.ok());
  raw_conn->SetNextRead(std::move(enc_syn.value()));

  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_conn->commit_count(), 1u);

  // Send ACK with bad magic
  Buf bad_ack = Buf::Allocate(allocator, HandshakePacket::kWireSizeBytes);
  auto enc_ack =
      HandshakePacket(HandshakePacket::Type::kAck).Encode(ByteSpan(bad_ack));
  ASSERT_TRUE(enc_ack.ok());
  bad_ack[0] ^= std::byte{0xff};
  raw_conn->SetNextRead(std::move(bad_ack));

  dispatcher.RunUntilStalled();
  ASSERT_TRUE(responder_task.result().has_value());
  EXPECT_EQ(responder_task.result()->status(), Status::DataLoss());
}

TEST(HandshakeTest, ResponderAcceptsHandshakePacketWithTrailingBytes) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  HandshakeTestTask responder_task(ResponderHandshakeFuture{conn});
  dispatcher.Post(responder_task);

  dispatcher.RunUntilStalled();

  // A 9-byte SYN, as a future protocol version might send, is accepted.
  raw_conn->SetNextRead(EncodeNewerPeerHandshake(
      allocator, HandshakePacket::Type::kSyn, 1, /*extra_bytes=*/1));

  dispatcher.RunUntilStalled();
  EXPECT_FALSE(responder_task.result().has_value());
  EXPECT_EQ(raw_conn->commit_count(), 1u);

  auto syn_ack = HandshakePacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(syn_ack.ok());
  EXPECT_EQ(syn_ack->type(), HandshakePacket::Type::kSynAck);
  EXPECT_EQ(syn_ack->version(), 1u);
}

TEST(HandshakeTest, ResponderNegotiatesDownWithNewerInitiator) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  HandshakeTestTask responder_task(ResponderHandshakeFuture{conn});
  dispatcher.Post(responder_task);

  dispatcher.RunUntilStalled();

  // A hypothetical v2 initiator advertises version 2 in an extended SYN.
  raw_conn->SetNextRead(EncodeNewerPeerHandshake(
      allocator, HandshakePacket::Type::kSyn, 2, /*extra_bytes=*/4));

  dispatcher.RunUntilStalled();
  EXPECT_FALSE(responder_task.result().has_value());
  EXPECT_EQ(raw_conn->commit_count(), 1u);

  // The local v1 responder negotiates down to version 1.
  auto syn_ack = HandshakePacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(syn_ack.ok());
  EXPECT_EQ(syn_ack->type(), HandshakePacket::Type::kSynAck);
  EXPECT_EQ(syn_ack->version(), HandshakePacket::kMaxVersion);
  EXPECT_EQ(syn_ack->version(), 1u);

  // The v2 initiator accepts version 1 and echoes it in the ACK.
  raw_conn->SetNextRead(EncodeNewerPeerHandshake(
      allocator, HandshakePacket::Type::kAck, 1, /*extra_bytes=*/4));

  dispatcher.RunUntilStalled();
  ASSERT_TRUE(responder_task.result().has_value());
  PW_TEST_ASSERT_OK(responder_task.result()->status());
  EXPECT_EQ(responder_task.result()->value().info.negotiated_version, 1u);
  EXPECT_TRUE(responder_task.result()->value().connection);
}

TEST(HandshakeTest, InitiatorAcceptsNegotiatedVersionFromNewerResponder) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  HandshakeTestTask initiator_task(InitiatorHandshakeFuture{conn});
  dispatcher.Post(initiator_task);

  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_conn->commit_count(), 1u);

  auto syn = HandshakePacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(syn.ok());
  EXPECT_EQ(syn->type(), HandshakePacket::Type::kSyn);
  EXPECT_EQ(syn->version(), HandshakePacket::kMaxVersion);

  // A hypothetical v2 responder negotiates down to version 1 and replies with
  // an extended SYN-ACK.
  raw_conn->SetNextRead(EncodeNewerPeerHandshake(
      allocator, HandshakePacket::Type::kSynAck, 1, /*extra_bytes=*/4));

  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_conn->commit_count(), 2u);
  ASSERT_TRUE(initiator_task.result().has_value());
  PW_TEST_ASSERT_OK(initiator_task.result()->status());
  EXPECT_EQ(initiator_task.result()->value().info.negotiated_version, 1u);
  EXPECT_TRUE(initiator_task.result()->value().connection);

  auto ack = HandshakePacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(ack.ok());
  EXPECT_EQ(ack->type(), HandshakePacket::Type::kAck);
  EXPECT_EQ(ack->version(), 1u);
}

TEST(HandshakeTest, InitiatorSendsMaxVersionAndEchoesNegotiatedVersion) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  HandshakeTestTask initiator_task(
      InitiatorHandshakeFuture{conn, HandshakeInfo{3}});
  dispatcher.Post(initiator_task);

  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_conn->commit_count(), 1u);

  auto syn = HandshakePacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(syn.ok());
  EXPECT_EQ(syn->type(), HandshakePacket::Type::kSyn);
  EXPECT_EQ(syn->version(), 3u);

  Buf syn_ack_buf = Buf::Allocate(allocator, HandshakePacket::kWireSizeBytes);
  auto enc_syn_ack = HandshakePacket(HandshakePacket::Type::kSynAck, 2)
                         .Encode(std::move(syn_ack_buf));
  PW_TEST_ASSERT_OK(enc_syn_ack.status());
  raw_conn->SetNextRead(std::move(*enc_syn_ack));

  dispatcher.RunUntilStalled();
  ASSERT_TRUE(initiator_task.result().has_value());
  PW_TEST_ASSERT_OK(initiator_task.result()->status());
  EXPECT_EQ(initiator_task.result()->value().info.negotiated_version, 2u);

  auto ack = HandshakePacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(ack.ok());
  EXPECT_EQ(ack->type(), HandshakePacket::Type::kAck);
  EXPECT_EQ(ack->version(), 2u);
}

TEST(HandshakeTest, ResponderSendsNegotiatedVersionInSynAck) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  HandshakeTestTask responder_task(
      ResponderHandshakeFuture{conn, HandshakeInfo{3}});
  dispatcher.Post(responder_task);

  dispatcher.RunUntilStalled();

  Buf syn_buf = Buf::Allocate(allocator, HandshakePacket::kWireSizeBytes);
  auto enc_syn = HandshakePacket(HandshakePacket::Type::kSyn, 2)
                     .Encode(std::move(syn_buf));
  PW_TEST_ASSERT_OK(enc_syn.status());
  raw_conn->SetNextRead(std::move(*enc_syn));

  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_conn->commit_count(), 1u);

  auto syn_ack = HandshakePacket::Decode(raw_conn->last_written_buf());
  ASSERT_TRUE(syn_ack.ok());
  EXPECT_EQ(syn_ack->type(), HandshakePacket::Type::kSynAck);
  EXPECT_EQ(syn_ack->version(), 2u);

  Buf ack_buf = Buf::Allocate(allocator, HandshakePacket::kWireSizeBytes);
  auto enc_ack = HandshakePacket(HandshakePacket::Type::kAck, 2)
                     .Encode(std::move(ack_buf));
  PW_TEST_ASSERT_OK(enc_ack.status());
  raw_conn->SetNextRead(std::move(*enc_ack));

  dispatcher.RunUntilStalled();
  ASSERT_TRUE(responder_task.result().has_value());
  PW_TEST_ASSERT_OK(responder_task.result()->status());
  EXPECT_EQ(responder_task.result()->value().info.negotiated_version, 2u);
}

TEST(HandshakeTest, VersionNegotiationBothAboveOne) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto paired = test::MakePairedConnections(allocator);

  HandshakeTestTask initiator_task(
      InitiatorHandshakeFuture(paired.first, HandshakeInfo{3}));
  HandshakeTestTask responder_task(
      ResponderHandshakeFuture(paired.second, HandshakeInfo{2}));

  dispatcher.Post(initiator_task);
  dispatcher.Post(responder_task);

  dispatcher.RunUntilStalled();

  ASSERT_TRUE(initiator_task.result().has_value());
  PW_TEST_ASSERT_OK(initiator_task.result()->status());
  EXPECT_EQ(initiator_task.result()->value().info.negotiated_version, 2u);

  ASSERT_TRUE(responder_task.result().has_value());
  PW_TEST_ASSERT_OK(responder_task.result()->status());
  EXPECT_EQ(responder_task.result()->value().info.negotiated_version, 2u);
}

TEST(HandshakeTest, ResponderRejectsVersionZeroInSyn) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  HandshakeTestTask responder_task(ResponderHandshakeFuture{conn});
  dispatcher.Post(responder_task);

  dispatcher.RunUntilStalled();

  Buf syn_buf = Buf::Allocate(allocator, HandshakePacket::kWireSizeBytes);
  auto enc_syn = HandshakePacket(HandshakePacket::Type::kSyn, 0)
                     .Encode(std::move(syn_buf));
  PW_TEST_ASSERT_OK(enc_syn.status());
  raw_conn->SetNextRead(std::move(*enc_syn));

  dispatcher.RunUntilStalled();
  ASSERT_TRUE(responder_task.result().has_value());
  EXPECT_EQ(responder_task.result()->status(), Status::DataLoss());
  EXPECT_EQ(raw_conn->commit_count(), 0u);
  EXPECT_TRUE(raw_conn->is_closed());
}

TEST(HandshakeTest, InitiatorRejectsVersionZeroInSynAck) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  HandshakeTestTask initiator_task(InitiatorHandshakeFuture{conn});
  dispatcher.Post(initiator_task);

  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_conn->commit_count(), 1u);

  Buf syn_ack_buf = Buf::Allocate(allocator, HandshakePacket::kWireSizeBytes);
  auto enc_syn_ack = HandshakePacket(HandshakePacket::Type::kSynAck, 0)
                         .Encode(std::move(syn_ack_buf));
  PW_TEST_ASSERT_OK(enc_syn_ack.status());
  raw_conn->SetNextRead(std::move(*enc_syn_ack));

  dispatcher.RunUntilStalled();
  ASSERT_TRUE(initiator_task.result().has_value());
  EXPECT_EQ(initiator_task.result()->status(), Status::DataLoss());
  EXPECT_EQ(raw_conn->commit_count(), 1u);
  EXPECT_TRUE(raw_conn->is_closed());
}

TEST(HandshakeTest, ConnectionClosedDuringHandshake) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  HandshakeTestTask initiator_task(InitiatorHandshakeFuture{conn});
  dispatcher.Post(initiator_task);

  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_conn->commit_count(), 1u);

  // Close connection
  conn.Close();
  dispatcher.RunUntilStalled();

  ASSERT_TRUE(initiator_task.result().has_value());
  EXPECT_EQ(initiator_task.result()->status(), Status::Cancelled());
}

TEST(HandshakeTest, FailedHandshakeClosesConnection) {
  allocator::test::AllocatorForTest<16384> allocator;
  async2::DispatcherForTest dispatcher;

  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  HandshakeTestTask initiator_task(InitiatorHandshakeFuture{conn});
  dispatcher.Post(initiator_task);

  dispatcher.RunUntilStalled();
  EXPECT_EQ(raw_conn->commit_count(), 1u);

  // Inject bad packet to trigger handshake failure
  Buf bad_buf = Buf::Allocate(allocator, 4);
  raw_conn->SetNextRead(std::move(bad_buf));

  dispatcher.RunUntilStalled();
  ASSERT_TRUE(initiator_task.result().has_value());
  EXPECT_EQ(initiator_task.result()->status(), Status::DataLoss());
  EXPECT_TRUE(raw_conn->is_closed());
}

TEST(HandshakeTest, AbandonedInitiatorHandshakeClosesConnection) {
  allocator::test::AllocatorForTest<16384> allocator;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  {
    InitiatorHandshakeFuture fut(conn);
    EXPECT_FALSE(raw_conn->is_closed());
  }
  EXPECT_TRUE(raw_conn->is_closed());
}

TEST(HandshakeTest, AbandonedResponderHandshakeClosesConnection) {
  allocator::test::AllocatorForTest<16384> allocator;
  auto [conn, raw_conn] = test::MakeMockConnection(allocator);
  {
    ResponderHandshakeFuture fut(conn);
    EXPECT_FALSE(raw_conn->is_closed());
  }
  EXPECT_TRUE(raw_conn->is_closed());
}

}  // namespace
}  // namespace pw::rpc2::internal
