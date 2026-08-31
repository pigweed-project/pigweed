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

#include "pw_bluetooth_proxy/config.h"

#if PW_BLUETOOTH_PROXY_CONFIG_ENABLE_RECOVERY

#include <cstdint>
#include <utility>

#include "pw_bluetooth_proxy/acl_snapshot.h"
#include "pw_bluetooth_proxy/h4_packet.h"
#include "pw_bluetooth_proxy/internal/logical_transport.h"
#include "pw_bluetooth_proxy/l2cap_snapshot.h"
#include "pw_bluetooth_proxy/proxy_host.h"
#include "pw_bluetooth_proxy_private/test_utils.h"
#include "pw_function/function.h"
#include "pw_status/status.h"
#include "pw_unit_test/framework.h"

namespace pw::bluetooth::proxy {
namespace {

constexpr uint16_t kLeConnectionHandle1 = 0x123;
constexpr uint16_t kLeConnectionHandle2 = 0x124;
constexpr uint16_t kBrEdrConnectionHandle = 0x456;

constexpr uint16_t kLeMaxAclCredits = 10;
constexpr uint16_t kBrEdrMaxAclCredits = 5;

AclConnectionSnapshot CreateAclConnectionSnapshot(
    uint16_t connection_handle = kLeConnectionHandle1,
    AclTransportType transport = AclTransportType::kLe,
    uint16_t num_proxy_pending = 0,
    uint16_t num_host_pending = 0,
    uint16_t num_queued_host = 0) {
  AclConnectionSnapshot snapshot;
  snapshot.connection_handle = connection_handle;
  snapshot.transport = transport;
  snapshot.num_proxy_pending_packets = num_proxy_pending;
  snapshot.num_host_pending_packets = num_host_pending;
  snapshot.num_queued_host_packets = num_queued_host;
  return snapshot;
}

TEST(AclSnapshotTest, AclConnectionSnapshotHelpers) {
  AclConnectionSnapshot snapshot = CreateAclConnectionSnapshot();

  EXPECT_TRUE(snapshot.MatchesKey(kLeConnectionHandle1));
  EXPECT_FALSE(snapshot.MatchesKey(kLeConnectionHandle2));

  AclConnectionSnapshot update = CreateAclConnectionSnapshot(
      kLeConnectionHandle1, AclTransportType::kBrEdr, 1);
  PW_TEST_EXPECT_OK(snapshot.Update(update));
  EXPECT_EQ(snapshot.transport, AclTransportType::kBrEdr);
  EXPECT_EQ(snapshot.num_proxy_pending_packets, 1);

  update.connection_handle = kLeConnectionHandle2;
  EXPECT_EQ(snapshot.Update(update), Status::InvalidArgument());
}

TEST(AclSnapshotTest, AclSnapshotApplyStateUpdate) {
  AclSnapshot snapshot;

  // Verify connection insertion.
  AclConnectionSnapshot connection = CreateAclConnectionSnapshot();
  PW_TEST_EXPECT_OK(snapshot.ApplyStateUpdate(connection));
  ASSERT_EQ(snapshot.acl_connections.size(), 1u);
  EXPECT_EQ(snapshot.acl_connections[0].num_proxy_pending_packets, 0);

  // Verify connection updating.
  connection.num_proxy_pending_packets = 1;
  PW_TEST_EXPECT_OK(snapshot.ApplyStateUpdate(connection));
  ASSERT_EQ(snapshot.acl_connections.size(), 1u);
  EXPECT_EQ(snapshot.acl_connections[0].num_proxy_pending_packets, 1);

  // Verify that removing a non-existent connection is a no-op.
  AclConnectionRemoved removed{.connection_handle = kLeConnectionHandle2};
  PW_TEST_EXPECT_OK(snapshot.ApplyStateUpdate(removed));
  ASSERT_EQ(snapshot.acl_connections.size(), 1u);

  // Verify connection removal.
  removed.connection_handle = kLeConnectionHandle1;
  PW_TEST_EXPECT_OK(snapshot.ApplyStateUpdate(removed));
  EXPECT_TRUE(snapshot.acl_connections.empty());

  // Verify that exceeding connection capacity causes the snapshot to be marked
  // as incomplete.
  for (uint16_t i = 0; i < PW_BLUETOOTH_PROXY_CONFIG_MAX_SNAPSHOT_CONNECTIONS;
       ++i) {
    connection.connection_handle = i;
    PW_TEST_EXPECT_OK(snapshot.ApplyStateUpdate(connection));
  }
  EXPECT_TRUE(snapshot.acl_connections.full());
  EXPECT_FALSE(snapshot.snapshot_incomplete);
  connection.connection_handle = 100;
  EXPECT_EQ(snapshot.ApplyStateUpdate(connection), Status::ResourceExhausted());
  EXPECT_TRUE(snapshot.snapshot_incomplete);
}

class AclRecoveryTest : public ProxyHostTest {
 protected:
  static AclSnapshot CreateAclSnapshot(
      uint16_t le_max = kLeMaxAclCredits,
      uint16_t br_edr_max = kBrEdrMaxAclCredits,
      bool incomplete = false) {
    AclSnapshot snapshot;
    snapshot.snapshot_incomplete = incomplete;
    snapshot.le_controller_max_packets = le_max;
    snapshot.br_edr_controller_max_packets = br_edr_max;
    return snapshot;
  }
};

TEST_F(AclRecoveryTest, SnapshotCaptureAndRecover) {
  constexpr uint16_t kLePendingAclCredits = 2;
  constexpr uint16_t kBrEdrPendingAclCredits = 1;
  std::optional<AclConnectionSnapshot> connection_snapshot;

  Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(
      std::move(send_to_host_fn),
      std::move(send_to_controller_fn),
      /*le_acl_credits_to_reserve=*/2,
      /*br_edr_acl_credits_to_reserve=*/2,
      GetProxyHostAllocator(),
      [&connection_snapshot](const ProxyHostStateUpdate& update) {
        if (auto* conn_snap = std::get_if<AclConnectionSnapshot>(&update)) {
          connection_snapshot = *conn_snap;
        }
      });
  StartDispatcherOnCurrentThread(proxy);

  ProxyHostSnapshot snapshot;
  snapshot.acl = CreateAclSnapshot(kLeMaxAclCredits, kBrEdrMaxAclCredits);
  snapshot.acl.acl_connections.push_back(
      CreateAclConnectionSnapshot(kLeConnectionHandle1,
                                  AclTransportType::kLe,
                                  /*num_proxy_pending=*/kLePendingAclCredits,
                                  /*num_host_pending=*/2,
                                  /*num_queued_host=*/0));
  snapshot.acl.acl_connections.push_back(
      CreateAclConnectionSnapshot(kBrEdrConnectionHandle,
                                  AclTransportType::kBrEdr,
                                  /*num_proxy_pending=*/kBrEdrPendingAclCredits,
                                  /*num_host_pending=*/1,
                                  /*num_queued_host=*/0));

  PW_TEST_ASSERT_OK(proxy.RecoverFromSnapshot(snapshot));

  // Verify ACL subsystem recovery. Expect free proxy packets to be 0 (2
  // reserved - 2 pending).
  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 0);

  Result<AclFrameWithStorage> acl_frame = SetupAcl(kLeConnectionHandle1, 10);
  ASSERT_TRUE(acl_frame.ok());
  proxy.HandleH4HciFromHost(
      H4PacketWithH4(emboss::H4PacketType::ACL_DATA, acl_frame->h4_span()));

#if PW_BLUETOOTH_PROXY_CONFIG_ENABLE_CREDIT_SNAPSHOT_UPDATES
  // Verify that the first connection was restored correctly. Expect
  // num_host_pending_packets to be 3 (2 restored + 1 new).
  ASSERT_TRUE(connection_snapshot.has_value());
  EXPECT_EQ(connection_snapshot->connection_handle, kLeConnectionHandle1);
  EXPECT_EQ(connection_snapshot->transport, AclTransportType::kLe);
  EXPECT_EQ(connection_snapshot->num_proxy_pending_packets,
            kLePendingAclCredits);
  EXPECT_EQ(connection_snapshot->num_host_pending_packets, 3);
  EXPECT_EQ(connection_snapshot->num_queued_host_packets, 0);

  acl_frame = SetupAcl(kBrEdrConnectionHandle, 10);
  ASSERT_TRUE(acl_frame.ok());
  connection_snapshot.reset();
  proxy.HandleH4HciFromHost(
      H4PacketWithH4(emboss::H4PacketType::ACL_DATA, acl_frame->h4_span()));

  // Verify that the second connection was restored correctly. Expect
  // num_host_pending_packets to be 2 (1 restored + 1 new).
  ASSERT_TRUE(connection_snapshot.has_value());
  EXPECT_EQ(connection_snapshot->connection_handle, kBrEdrConnectionHandle);
  EXPECT_EQ(connection_snapshot->transport, AclTransportType::kBrEdr);
  EXPECT_EQ(connection_snapshot->num_proxy_pending_packets,
            kBrEdrPendingAclCredits);
  EXPECT_EQ(connection_snapshot->num_host_pending_packets, 2);
  EXPECT_EQ(connection_snapshot->num_queued_host_packets, 0);
#else
  // Verify that credit mutations don't trigger a state update callback when
  // credit snapshot updates are disabled.
  EXPECT_FALSE(connection_snapshot.has_value());
#endif  // PW_BLUETOOTH_PROXY_CONFIG_ENABLE_CREDIT_SNAPSHOT_UPDATES
}

TEST_F(AclRecoveryTest, SnapshotRecoverFailsOnIncomplete) {
  Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/2,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator(),
                              nullptr);
  StartDispatcherOnCurrentThread(proxy);

  ProxyHostSnapshot snapshot;
  snapshot.acl.snapshot_incomplete = true;
  EXPECT_EQ(proxy.RecoverFromSnapshot(snapshot), Status::DataLoss());
}

TEST_F(AclRecoveryTest, RegisterStateUpdateCallback) {
  struct {
    uint32_t updates_sent = 0;
    ProxyHostStateUpdate last_update;
  } update_capture;

  Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy =
      ProxyHost(std::move(send_to_host_fn),
                std::move(send_to_controller_fn),
                /*le_acl_credits_to_reserve=*/2,
                /*br_edr_acl_credits_to_reserve=*/0,
                GetProxyHostAllocator(),
                [&update_capture](const ProxyHostStateUpdate& update) {
                  update_capture.updates_sent++;
                  update_capture.last_update = update;
                });
  StartDispatcherOnCurrentThread(proxy);

  PW_TEST_ASSERT_OK(SendLeReadBufferResponseFromController(proxy, 10));

  // Verify that connection creation triggers a state update callback.
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kLeConnectionHandle1, emboss::StatusCode::SUCCESS));
  EXPECT_EQ(update_capture.updates_sent, 1u);
  ASSERT_TRUE(std::holds_alternative<AclConnectionSnapshot>(
      update_capture.last_update));
  AclConnectionSnapshot connection_snapshot =
      std::get<AclConnectionSnapshot>(update_capture.last_update);
  EXPECT_EQ(connection_snapshot.connection_handle, kLeConnectionHandle1);
  EXPECT_EQ(connection_snapshot.num_proxy_pending_packets, 0);
  EXPECT_EQ(connection_snapshot.num_host_pending_packets, 0);

  Result<AclFrameWithStorage> acl_frame = SetupAcl(kLeConnectionHandle1, 10);
  ASSERT_TRUE(acl_frame.ok());
  H4PacketWithH4 h4_packet(emboss::H4PacketType::ACL_DATA,
                           acl_frame->h4_span());
  proxy.HandleH4HciFromHost(std::move(h4_packet));
#if PW_BLUETOOTH_PROXY_CONFIG_ENABLE_CREDIT_SNAPSHOT_UPDATES
  // Verify that sending a packet from the host triggers a state update
  // callback.
  EXPECT_EQ(update_capture.updates_sent, 2u);
  ASSERT_TRUE(std::holds_alternative<AclConnectionSnapshot>(
      update_capture.last_update));
  connection_snapshot =
      std::get<AclConnectionSnapshot>(update_capture.last_update);
  EXPECT_EQ(connection_snapshot.connection_handle, kLeConnectionHandle1);
  EXPECT_EQ(connection_snapshot.num_host_pending_packets, 1);
#else
  // Verify that credit mutations don't trigger a state update callback when
  // credit snapshot updates are disabled.
  EXPECT_EQ(update_capture.updates_sent, 1u);
#endif  // PW_BLUETOOTH_PROXY_CONFIG_ENABLE_CREDIT_SNAPSHOT_UPDATES

  PW_TEST_ASSERT_OK(
      SendNumberOfCompletedPackets(proxy, {{kLeConnectionHandle1, 1}}));
#if PW_BLUETOOTH_PROXY_CONFIG_ENABLE_CREDIT_SNAPSHOT_UPDATES
  // Verify that reclaiming credits via NOCP event triggers a state update
  // callback.
  EXPECT_EQ(update_capture.updates_sent, 3u);
  ASSERT_TRUE(std::holds_alternative<AclConnectionSnapshot>(
      update_capture.last_update));
  connection_snapshot =
      std::get<AclConnectionSnapshot>(update_capture.last_update);
  EXPECT_EQ(connection_snapshot.connection_handle, kLeConnectionHandle1);
  EXPECT_EQ(connection_snapshot.num_host_pending_packets, 0);
#else
  // Verify that credit mutations don't trigger a state update callback when
  // credit snapshot updates are disabled.
  EXPECT_EQ(update_capture.updates_sent, 1u);
#endif  // PW_BLUETOOTH_PROXY_CONFIG_ENABLE_CREDIT_SNAPSHOT_UPDATES

  // Verify that disconnection triggers a state update callback.
  PW_TEST_ASSERT_OK(
      SendDisconnectionCompleteEvent(proxy, kLeConnectionHandle1));
#if PW_BLUETOOTH_PROXY_CONFIG_ENABLE_CREDIT_SNAPSHOT_UPDATES
  EXPECT_EQ(update_capture.updates_sent, 4u);
#else
  EXPECT_EQ(update_capture.updates_sent, 2u);
#endif  // PW_BLUETOOTH_PROXY_CONFIG_ENABLE_CREDIT_SNAPSHOT_UPDATES
  ASSERT_TRUE(
      std::holds_alternative<AclConnectionRemoved>(update_capture.last_update));
  EXPECT_EQ(std::get<AclConnectionRemoved>(update_capture.last_update)
                .connection_handle,
            kLeConnectionHandle1);
}

TEST_F(AclRecoveryTest, CreditResynchronizationDefersAndSends) {
  constexpr uint16_t kQueuedPackets1 = 3;
  constexpr uint16_t kQueuedPackets2 = 5;

  struct {
    std::optional<H4PacketWithHci> captured_packet;
    Allocator* allocator;
  } send_capture;
  send_capture.allocator = GetProxyHostAllocator();

  Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [&send_capture](H4PacketWithHci&& packet) {
        send_capture.captured_packet =
            H4PacketWithHci::CopyFrom(*send_capture.allocator, packet).value();
      });

  Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/2,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator(),
                              nullptr);
  StartDispatcherOnCurrentThread(proxy);

  // Create a snapshot where connections had queued packets that were lost.
  ProxyHostSnapshot snapshot;
  snapshot.acl = CreateAclSnapshot();
  snapshot.acl.acl_connections.push_back(
      CreateAclConnectionSnapshot(kLeConnectionHandle1,
                                  AclTransportType::kLe,
                                  /*num_proxy_pending=*/0,
                                  /*num_host_pending=*/0,
                                  kQueuedPackets1));
  snapshot.acl.acl_connections.push_back(
      CreateAclConnectionSnapshot(kLeConnectionHandle2,
                                  AclTransportType::kLe,
                                  /*num_proxy_pending=*/0,
                                  /*num_host_pending=*/0,
                                  kQueuedPackets2));

  PW_TEST_ASSERT_OK(proxy.RecoverFromSnapshot(snapshot));

  // Verify that no refund event is sent to host immediately.
  EXPECT_FALSE(send_capture.captured_packet.has_value());

  proxy.InitiateAclCreditResynchronization();

  // Verify that a NUMBER_OF_COMPLETED_PACKETS event was sent with correct
  // refunds.
  ASSERT_TRUE(send_capture.captured_packet.has_value());
  auto view = MakeEmbossView<emboss::NumberOfCompletedPacketsEventView>(
      send_capture.captured_packet->GetHciSpan());
  ASSERT_TRUE(view.ok());
  EXPECT_EQ(view->header().event_code().Read(),
            emboss::EventCode::NUMBER_OF_COMPLETED_PACKETS);
  EXPECT_EQ(view->num_handles().Read(), 2);
  EXPECT_EQ(view->nocp_data()[0].connection_handle().Read(),
            kLeConnectionHandle1);
  EXPECT_EQ(view->nocp_data()[0].num_completed_packets().Read(),
            kQueuedPackets1);
  EXPECT_EQ(view->nocp_data()[1].connection_handle().Read(),
            kLeConnectionHandle2);
  EXPECT_EQ(view->nocp_data()[1].num_completed_packets().Read(),
            kQueuedPackets2);

  // Verify that subsequent triggers do not send duplicate refunds.
  send_capture.captured_packet.reset();
  proxy.InitiateAclCreditResynchronization();
  EXPECT_FALSE(send_capture.captured_packet.has_value());
}

TEST_F(AclRecoveryTest, StaticCreditsPendingDerivedFromConnections) {
  Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/2,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator(),
                              nullptr);
  StartDispatcherOnCurrentThread(proxy);

  // Connection snapshot has 5 host pending packets (which would exceed
  // proxy_max = 2 if treated as proxy pending) and 0 proxy pending packets.
  ProxyHostSnapshot snapshot;
  snapshot.acl = CreateAclSnapshot(/*le_max=*/10, /*br_edr_max=*/0);
  snapshot.acl.acl_connections.push_back(
      CreateAclConnectionSnapshot(kLeConnectionHandle1,
                                  AclTransportType::kLe,
                                  /*num_proxy_pending=*/0,
                                  /*num_host_pending=*/5,
                                  /*num_queued_host=*/0));

  PW_TEST_ASSERT_OK(proxy.RecoverFromSnapshot(snapshot));

  // The proxy reserved 2 credits and has 0 proxy pending packets.
  // Verify that remaining credits is 2 (not reduced by host pending packets).
  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 2);
}

TEST_F(AclRecoveryTest, DynamicCreditsPendingDerivedFromConnections) {
  Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              *GetProxyHostAllocator(),
                              nullptr);
  StartDispatcherOnCurrentThread(proxy);

  // In dynamic credit sharing, both proxy and host in-flight packets are
  // counted towards total pending credits against controller max capacity.
  ProxyHostSnapshot snapshot;
  snapshot.acl = CreateAclSnapshot(/*le_max=*/10, /*br_edr_max=*/0);
  snapshot.acl.acl_connections.push_back(
      CreateAclConnectionSnapshot(kLeConnectionHandle1,
                                  AclTransportType::kLe,
                                  /*num_proxy_pending=*/2,
                                  /*num_host_pending=*/3,
                                  /*num_queued_host=*/0));

  PW_TEST_ASSERT_OK(proxy.RecoverFromSnapshot(snapshot));

  // Controller max is 10, total pending is 2 + 3 = 5.
  // Verify that remaining credits is 10 - 5 = 5.
  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 5);
}

constexpr uint16_t kLocalCid1 = 0x40;
constexpr uint16_t kLocalCid2 = 0x41;
constexpr uint16_t kRemoteCid1 = 0x50;
constexpr uint16_t kRemoteCid2 = 0x51;

L2capChannelSnapshot CreateL2capChannelSnapshot(
    uint16_t connection_handle = kLeConnectionHandle1,
    uint16_t local_cid = kLocalCid1,
    uint16_t remote_cid = kRemoteCid1,
    AclTransportType transport = AclTransportType::kLe,
    L2capChannelMode mode = L2capChannelMode::kBasic) {
  L2capChannelSnapshot snapshot;
  snapshot.connection_handle = connection_handle;
  snapshot.local_cid = local_cid;
  snapshot.remote_cid = remote_cid;
  snapshot.transport = transport;
  snapshot.mode = mode;
  return snapshot;
}

TEST(L2capSnapshotTest, L2capChannelSnapshotHelpers) {
  L2capChannelSnapshot snapshot = CreateL2capChannelSnapshot();

  EXPECT_TRUE(snapshot.MatchesKey(kLeConnectionHandle1, kLocalCid1));
  EXPECT_FALSE(snapshot.MatchesKey(kLeConnectionHandle1, kLocalCid2));
  EXPECT_FALSE(snapshot.MatchesKey(kLeConnectionHandle2, kLocalCid1));

  L2capChannelRemoved removed{.connection_handle = kLeConnectionHandle1,
                              .local_cid = kLocalCid1};
  EXPECT_TRUE(snapshot.MatchesKey(removed));
  removed.local_cid = kLocalCid2;
  EXPECT_FALSE(snapshot.MatchesKey(removed));

  L2capChannelSnapshot update = CreateL2capChannelSnapshot(
      kLeConnectionHandle1, kLocalCid1, kRemoteCid2, AclTransportType::kBrEdr);
  PW_TEST_EXPECT_OK(snapshot.Update(update));
  EXPECT_EQ(snapshot.remote_cid, kRemoteCid2);
  EXPECT_EQ(snapshot.transport, AclTransportType::kBrEdr);

  update.local_cid = kLocalCid2;
  EXPECT_EQ(snapshot.Update(update), Status::InvalidArgument());
}

TEST(L2capSnapshotTest, L2capSnapshotApplyStateUpdate) {
  L2capSnapshot snapshot;

  // Verify channel insertion.
  L2capChannelSnapshot channel = CreateL2capChannelSnapshot();
  PW_TEST_EXPECT_OK(snapshot.ApplyStateUpdate(channel));
  ASSERT_EQ(snapshot.l2cap_channels.size(), 1u);
  EXPECT_EQ(snapshot.l2cap_channels[0].remote_cid, kRemoteCid1);

  // Verify channel updating.
  channel.remote_cid = kRemoteCid2;
  PW_TEST_EXPECT_OK(snapshot.ApplyStateUpdate(channel));
  ASSERT_EQ(snapshot.l2cap_channels.size(), 1u);
  EXPECT_EQ(snapshot.l2cap_channels[0].remote_cid, kRemoteCid2);

  // Verify that removing a non-existent channel is a no-op.
  L2capChannelRemoved removed{.connection_handle = kLeConnectionHandle2,
                              .local_cid = kLocalCid1};
  PW_TEST_EXPECT_OK(snapshot.ApplyStateUpdate(removed));
  ASSERT_EQ(snapshot.l2cap_channels.size(), 1u);

  // Verify channel removal.
  removed.connection_handle = kLeConnectionHandle1;
  PW_TEST_EXPECT_OK(snapshot.ApplyStateUpdate(removed));
  EXPECT_TRUE(snapshot.l2cap_channels.empty());

  // Verify that exceeding channel capacity causes the snapshot to be marked as
  // incomplete.
  for (uint16_t i = 0;
       i < PW_BLUETOOTH_PROXY_CONFIG_MAX_SNAPSHOT_L2CAP_CHANNELS;
       ++i) {
    channel.local_cid = i;
    PW_TEST_EXPECT_OK(snapshot.ApplyStateUpdate(channel));
  }
  EXPECT_TRUE(snapshot.l2cap_channels.full());
  EXPECT_FALSE(snapshot.snapshot_incomplete);
  channel.local_cid = 100;
  EXPECT_EQ(snapshot.ApplyStateUpdate(channel), Status::ResourceExhausted());
  EXPECT_TRUE(snapshot.snapshot_incomplete);
}

constexpr uint16_t kMtu = 100;
constexpr uint16_t kMps = 100;

class L2capRecoveryTest : public ProxyHostTest {};

TEST_F(L2capRecoveryTest, SnapshotCaptureAndRecover) {
  std::optional<L2capSignalingStateSnapshot> signaling_snapshot;

  Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy =
      ProxyHost(std::move(send_to_host_fn),
                std::move(send_to_controller_fn),
                /*le_acl_credits_to_reserve=*/2,
                /*br_edr_acl_credits_to_reserve=*/0,
                GetProxyHostAllocator(),
                [&signaling_snapshot](const ProxyHostStateUpdate& update) {
                  if (auto* sig_snap =
                          std::get_if<L2capSignalingStateSnapshot>(&update)) {
                    signaling_snapshot = *sig_snap;
                  }
                });
  StartDispatcherOnCurrentThread(proxy);

  ProxyHostSnapshot snapshot;
  snapshot.acl.acl_connections.push_back(
      AclConnectionSnapshot{.connection_handle = kLeConnectionHandle1,
                            .transport = AclTransportType::kLe});

  snapshot.l2cap.l2cap_channels.push_back(CreateL2capChannelSnapshot());
  snapshot.l2cap.l2cap_signaling_states.push_back(L2capSignalingStateSnapshot{
      .connection_handle = kLeConnectionHandle1,
      .transport = AclTransportType::kLe,
      .next_identifier = 42,
  });

  PW_TEST_ASSERT_OK(proxy.RecoverFromSnapshot(snapshot));

  Result<L2capCoc> coc = BuildCocWithResult(proxy,
                                            CocParameters{
                                                .handle = kLeConnectionHandle1,
                                                .local_cid = kLocalCid1,
                                                .remote_cid = kRemoteCid1,
                                            });
  PW_TEST_ASSERT_OK(coc.status());

  proxy.CompleteRecovery();

  PW_TEST_ASSERT_OK(SendLeReadBufferResponseFromController(proxy, 10));
  PW_TEST_ASSERT_OK(coc.value().SendAdditionalRxCredits(5));

  // Verify that channel was restored correctly. Expect next_identifier to be
  // 43 (incremented once).
  ASSERT_TRUE(signaling_snapshot.has_value());
  EXPECT_EQ(signaling_snapshot->connection_handle, kLeConnectionHandle1);
  EXPECT_EQ(signaling_snapshot->transport, AclTransportType::kLe);
  EXPECT_EQ(signaling_snapshot->next_identifier, 43);
}

TEST_F(L2capRecoveryTest, SnapshotRecoverFailsOnIncomplete) {
  Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/2,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator(),
                              nullptr);
  StartDispatcherOnCurrentThread(proxy);

  ProxyHostSnapshot snapshot;
  snapshot.l2cap.snapshot_incomplete = true;
  EXPECT_EQ(proxy.RecoverFromSnapshot(snapshot), Status::DataLoss());
}

TEST_F(L2capRecoveryTest, SnapshotRecoverFailsOnMissingAclConnection) {
  Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/2,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator(),
                              nullptr);
  StartDispatcherOnCurrentThread(proxy);

  // Verify that L2CAP recovery fails if the corresponding ACL connection is
  // missing from the snapshot.
  ProxyHostSnapshot snapshot;
  snapshot.l2cap.l2cap_channels.push_back(CreateL2capChannelSnapshot(
      kLeConnectionHandle1, kLocalCid1, kRemoteCid1));
  snapshot.l2cap.l2cap_signaling_states.push_back(L2capSignalingStateSnapshot{
      .connection_handle = kLeConnectionHandle1,
      .transport = AclTransportType::kLe,
  });
  EXPECT_EQ(proxy.RecoverFromSnapshot(snapshot), Status::InvalidArgument());
}

TEST_F(L2capRecoveryTest, RegisterBasicModeChannelStateUpdateCallback) {
  Vector<ProxyHostStateUpdate, 2> updates;

  Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(
      std::move(send_to_host_fn),
      std::move(send_to_controller_fn),
      /*le_acl_credits_to_reserve=*/2,
      /*br_edr_acl_credits_to_reserve=*/0,
      GetProxyHostAllocator(),
      [&updates](const ProxyHostStateUpdate& update) {
        if (std::holds_alternative<L2capSignalingStateSnapshot>(update) ||
            std::holds_alternative<L2capChannelSnapshot>(update) ||
            std::holds_alternative<L2capChannelRemoved>(update)) {
          updates.push_back(update);
        }
      });
  StartDispatcherOnCurrentThread(proxy);

  PW_TEST_ASSERT_OK(SendLeReadBufferResponseFromController(proxy, 10));

  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kLeConnectionHandle1, emboss::StatusCode::SUCCESS));
  ASSERT_EQ(updates.size(), 0u);

  L2capChannelManagerInterface::SpanReceiveFunction rx_fn =
      [](span<const std::byte>, ConnectionHandle, uint16_t, uint16_t) {
        return true;
      };
  L2capChannelManagerInterface::SpanReceiveFunction tx_fn =
      [](span<const std::byte>, ConnectionHandle, uint16_t, uint16_t) {
        return true;
      };

  // Verify that channel registration triggers a state update callback.
  PW_TEST_ASSERT_OK(
      proxy
          .InterceptBasicModeChannel(ConnectionHandle{kLeConnectionHandle1},
                                     kLocalCid1,
                                     kRemoteCid1,
                                     AclTransportType::kLe,
                                     std::move(rx_fn),
                                     std::move(tx_fn),
                                     /*event_fn=*/nullptr)
          .status());
  ASSERT_EQ(updates.size(), 1u);
  ASSERT_TRUE(std::holds_alternative<L2capChannelSnapshot>(updates[0]));
  L2capChannelSnapshot channel_snapshot =
      std::get<L2capChannelSnapshot>(updates[0]);
  EXPECT_EQ(channel_snapshot.connection_handle, kLeConnectionHandle1);
  EXPECT_EQ(channel_snapshot.local_cid, kLocalCid1);
  EXPECT_EQ(channel_snapshot.remote_cid, kRemoteCid1);

  // Verify that channel removal triggers a state update callback.
  PW_TEST_ASSERT_OK(SendL2capDisconnectRsp(proxy,
                                           Direction::kFromController,
                                           AclTransportType::kLe,
                                           kLeConnectionHandle1,
                                           kLocalCid1,
                                           kRemoteCid1));
  ASSERT_EQ(updates.size(), 2u);
  ASSERT_TRUE(std::holds_alternative<L2capChannelRemoved>(updates[1]));
  L2capChannelRemoved removed_channel =
      std::get<L2capChannelRemoved>(updates[1]);
  EXPECT_EQ(removed_channel.connection_handle, kLeConnectionHandle1);
  EXPECT_EQ(removed_channel.local_cid, kLocalCid1);
}

TEST_F(L2capRecoveryTest,
       RegisterCreditBasedFlowControlChannelStateUpdateCallback) {
#if PW_BLUETOOTH_PROXY_CONFIG_ENABLE_CREDIT_SNAPSHOT_UPDATES
  constexpr size_t kExpectedUpdates = 7;
#else
  constexpr size_t kExpectedUpdates = 3;
#endif  // PW_BLUETOOTH_PROXY_CONFIG_ENABLE_CREDIT_SNAPSHOT_UPDATES
  Vector<ProxyHostStateUpdate, kExpectedUpdates> updates;

  Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(
      std::move(send_to_host_fn),
      std::move(send_to_controller_fn),
      /*le_acl_credits_to_reserve=*/2,
      /*br_edr_acl_credits_to_reserve=*/0,
      GetProxyHostAllocator(),
      [&updates](const ProxyHostStateUpdate& update) {
        if (std::holds_alternative<L2capSignalingStateSnapshot>(update) ||
            std::holds_alternative<L2capChannelSnapshot>(update) ||
            std::holds_alternative<L2capChannelRemoved>(update)) {
          updates.push_back(update);
        }
      });
  StartDispatcherOnCurrentThread(proxy);

  PW_TEST_ASSERT_OK(SendLeReadBufferResponseFromController(proxy, 10));

  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kLeConnectionHandle1, emboss::StatusCode::SUCCESS));
  ASSERT_EQ(updates.size(), 0u);

  ConnectionOrientedChannelConfig rx_config{
      .cid = kLocalCid1, .mtu = kMtu, .mps = kMps, .credits = 5};
  ConnectionOrientedChannelConfig tx_config{
      .cid = kRemoteCid1, .mtu = kMtu, .mps = kMps, .credits = 5};
  MultiBufReceiveFunction rx_fn = [](multibuf::MultiBuf&&) {};

  // Verify that channel registration triggers a state update callback.
  auto coc_result = proxy.InterceptCreditBasedFlowControlChannel(
      ConnectionHandle{kLeConnectionHandle1},
      rx_config,
      tx_config,
      std::move(rx_fn),
      /*event_fn=*/nullptr);
  PW_TEST_ASSERT_OK(coc_result.status());
  ASSERT_EQ(updates.size(), 1u);
  ASSERT_TRUE(std::holds_alternative<L2capChannelSnapshot>(updates[0]));
  L2capChannelSnapshot channel_snapshot =
      std::get<L2capChannelSnapshot>(updates[0]);
  EXPECT_EQ(channel_snapshot.connection_handle, kLeConnectionHandle1);
  EXPECT_EQ(channel_snapshot.local_cid, kLocalCid1);
  EXPECT_EQ(channel_snapshot.remote_cid, kRemoteCid1);

#if PW_BLUETOOTH_PROXY_CONFIG_ENABLE_CREDIT_SNAPSHOT_UPDATES
  // Verify that adding rx credits triggers both a signaling and
  // channel state update callback.
  PW_TEST_ASSERT_OK(coc_result.value()->SendAdditionalRxCredits(5));
  ASSERT_EQ(updates.size(), 3u);
  ASSERT_TRUE(std::holds_alternative<L2capSignalingStateSnapshot>(updates[1]));
  L2capSignalingStateSnapshot signaling_snapshot =
      std::get<L2capSignalingStateSnapshot>(updates[1]);
  EXPECT_EQ(signaling_snapshot.connection_handle, kLeConnectionHandle1);
  EXPECT_EQ(signaling_snapshot.transport, AclTransportType::kLe);
  EXPECT_EQ(signaling_snapshot.next_identifier, 2);
  ASSERT_TRUE(std::holds_alternative<L2capChannelSnapshot>(updates[2]));
  EXPECT_EQ(
      std::get<L2capChannelSnapshot>(updates[2]).rx_engine.remaining_credits,
      10);

  // Verify that consuming rx credits triggers a state update callback.
  std::array<uint8_t, 10> rx_payload = {0x08};
  SendL2capBFrame(
      proxy, kLeConnectionHandle1, rx_payload, rx_payload.size(), kLocalCid1);
  ASSERT_EQ(updates.size(), 4u);
  ASSERT_TRUE(std::holds_alternative<L2capChannelSnapshot>(updates[3]));
  EXPECT_EQ(
      std::get<L2capChannelSnapshot>(updates[3]).rx_engine.remaining_credits,
      9);

  // Verify that adding tx credits triggers a state update callback.
  std::array<uint8_t, 8> credit_indication_payload = {
      0x16,  // FLOW_CONTROL_CREDIT_IND code
      0x01,  // Identifier
      0x04,
      0x00,  // Data length (4)
      0x50,
      0x00,  // Remote CID (kRemoteCid1 = 0x50)
      0x05,
      0x00  // Credits (5)
  };
  SendL2capBFrame(proxy,
                  kLeConnectionHandle1,
                  credit_indication_payload,
                  credit_indication_payload.size(),
                  static_cast<uint16_t>(emboss::L2capFixedCid::LE_U_SIGNALING));
  ASSERT_EQ(updates.size(), 5u);
  ASSERT_TRUE(std::holds_alternative<L2capChannelSnapshot>(updates[4]));
  EXPECT_EQ(
      std::get<L2capChannelSnapshot>(updates[4]).tx_engine.remaining_credits,
      10);

  // Verify that consuming tx credits triggers a state update callback.
  std::array<uint8_t, 10> tx_payload = {0};
  PW_TEST_ASSERT_OK(
      coc_result.value()->Write(MultiBufFromArray(tx_payload)).status);
  RunDispatcher();
  ASSERT_EQ(updates.size(), 6u);
  ASSERT_TRUE(std::holds_alternative<L2capChannelSnapshot>(updates[5]));
  EXPECT_EQ(
      std::get<L2capChannelSnapshot>(updates[5]).tx_engine.remaining_credits,
      9);
#else
  // Verify that credit mutations don't trigger a channel state update callback
  // when credit snapshot updates are disabled. Expect only a signaling state
  // update callback to be triggered.
  PW_TEST_ASSERT_OK(coc_result.value()->SendAdditionalRxCredits(5));
  EXPECT_EQ(updates.size(), 2u);
#endif  // PW_BLUETOOTH_PROXY_CONFIG_ENABLE_CREDIT_SNAPSHOT_UPDATES

  // Verify that channel removal triggers a state update callback.
  PW_TEST_ASSERT_OK(SendL2capDisconnectRsp(proxy,
                                           Direction::kFromController,
                                           AclTransportType::kLe,
                                           kLeConnectionHandle1,
                                           kLocalCid1,
                                           kRemoteCid1));
  ASSERT_EQ(updates.size(), kExpectedUpdates);
  ASSERT_TRUE(std::holds_alternative<L2capChannelRemoved>(
      updates[kExpectedUpdates - 1]));
  L2capChannelRemoved removed_channel =
      std::get<L2capChannelRemoved>(updates[kExpectedUpdates - 1]);
  EXPECT_EQ(removed_channel.connection_handle, kLeConnectionHandle1);
  EXPECT_EQ(removed_channel.local_cid, kLocalCid1);
}

TEST_F(L2capRecoveryTest, AclDisconnectionSendsChannelRemovedUpdates) {
  Vector<ProxyHostStateUpdate, 2> updates;

  Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(
      std::move(send_to_host_fn),
      std::move(send_to_controller_fn),
      /*le_acl_credits_to_reserve=*/2,
      /*br_edr_acl_credits_to_reserve=*/0,
      GetProxyHostAllocator(),
      [&updates](const ProxyHostStateUpdate& update) {
        if (std::holds_alternative<L2capSignalingStateSnapshot>(update) ||
            std::holds_alternative<L2capChannelSnapshot>(update) ||
            std::holds_alternative<L2capChannelRemoved>(update)) {
          updates.push_back(update);
        }
      });
  StartDispatcherOnCurrentThread(proxy);

  PW_TEST_ASSERT_OK(SendLeReadBufferResponseFromController(proxy, 10));

  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kLeConnectionHandle1, emboss::StatusCode::SUCCESS));
  ASSERT_EQ(updates.size(), 0u);

  Result<BasicL2capChannel> channel =
      BuildBasicL2capChannelWithResult(proxy,
                                       BasicL2capParameters{
                                           .handle = kLeConnectionHandle1,
                                           .local_cid = kLocalCid1,
                                           .remote_cid = kRemoteCid1,
                                           .transport = AclTransportType::kLe,
                                       });
  PW_TEST_ASSERT_OK(channel.status());
  ASSERT_EQ(updates.size(), 1u);

  PW_TEST_ASSERT_OK(
      SendDisconnectionCompleteEvent(proxy, kLeConnectionHandle1));
  RunDispatcher();
  ASSERT_EQ(updates.size(), 2u);
  ASSERT_TRUE(std::holds_alternative<L2capChannelRemoved>(updates[1]));
  L2capChannelRemoved removed_channel =
      std::get<L2capChannelRemoved>(updates[1]);
  EXPECT_EQ(removed_channel.connection_handle, kLeConnectionHandle1);
  EXPECT_EQ(removed_channel.local_cid, kLocalCid1);
}

TEST_F(L2capRecoveryTest, DuplicateChannelReplacementEmitsStateUpdates) {
  Vector<ProxyHostStateUpdate, 4> updates;

  Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(
      std::move(send_to_host_fn),
      std::move(send_to_controller_fn),
      /*le_acl_credits_to_reserve=*/2,
      /*br_edr_acl_credits_to_reserve=*/0,
      GetProxyHostAllocator(),
      [&updates](const ProxyHostStateUpdate& update) {
        if (std::holds_alternative<L2capSignalingStateSnapshot>(update) ||
            std::holds_alternative<L2capChannelSnapshot>(update) ||
            std::holds_alternative<L2capChannelRemoved>(update)) {
          updates.push_back(update);
        }
      });
  StartDispatcherOnCurrentThread(proxy);

  PW_TEST_ASSERT_OK(SendLeReadBufferResponseFromController(proxy, 10));

  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kLeConnectionHandle1, emboss::StatusCode::SUCCESS));
  ASSERT_EQ(updates.size(), 0u);

  // Register a channel and immediately destruct it to make it stale.
  PW_TEST_ASSERT_OK(
      BuildBasicL2capChannelWithResult(proxy,
                                       BasicL2capParameters{
                                           .handle = kLeConnectionHandle1,
                                           .local_cid = kLocalCid1,
                                           .remote_cid = kRemoteCid1,
                                           .transport = AclTransportType::kLe,
                                       })
          .status());
  ASSERT_EQ(updates.size(), 1u);
  ASSERT_TRUE(std::holds_alternative<L2capChannelSnapshot>(updates[0]));

  // Verify that registering a duplicate channel that replaces a stale one
  // triggers a state update callback twice; once for removal and once for
  // creation.
  auto channel =
      BuildBasicL2capChannelWithResult(proxy,
                                       BasicL2capParameters{
                                           .handle = kLeConnectionHandle1,
                                           .local_cid = kLocalCid1,
                                           .remote_cid = kRemoteCid1,
                                           .transport = AclTransportType::kLe,
                                       });
  PW_TEST_ASSERT_OK(channel.status());
  ASSERT_EQ(updates.size(), 3u);

  ASSERT_TRUE(std::holds_alternative<L2capChannelRemoved>(updates[1]));
  L2capChannelRemoved removed_channel =
      std::get<L2capChannelRemoved>(updates[1]);
  EXPECT_EQ(removed_channel.connection_handle, kLeConnectionHandle1);
  EXPECT_EQ(removed_channel.local_cid, kLocalCid1);

  ASSERT_TRUE(std::holds_alternative<L2capChannelSnapshot>(updates[2]));
  L2capChannelSnapshot channel_snapshot =
      std::get<L2capChannelSnapshot>(updates[2]);
  EXPECT_EQ(channel_snapshot.connection_handle, kLeConnectionHandle1);
  EXPECT_EQ(channel_snapshot.local_cid, kLocalCid1);
}

TEST_F(L2capRecoveryTest, SnapshotRecoverEstablishesL2capLinks) {
  uint8_t next_identifier = 0;

  Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(
      std::move(send_to_host_fn),
      std::move(send_to_controller_fn),
      /*le_acl_credits_to_reserve=*/2,
      /*br_edr_acl_credits_to_reserve=*/0,
      GetProxyHostAllocator(),
      [&next_identifier](const ProxyHostStateUpdate& update) {
        if (auto* signaling_snapshot =
                std::get_if<L2capSignalingStateSnapshot>(&update);
            signaling_snapshot &&
            signaling_snapshot->connection_handle == kLeConnectionHandle1) {
          next_identifier = signaling_snapshot->next_identifier;
        }
      });
  StartDispatcherOnCurrentThread(proxy);

  ProxyHostSnapshot snapshot;
  snapshot.acl.acl_connections.push_back(
      AclConnectionSnapshot{.connection_handle = kLeConnectionHandle1,
                            .transport = AclTransportType::kLe});
  snapshot.acl.acl_connections.push_back(
      AclConnectionSnapshot{.connection_handle = kBrEdrConnectionHandle,
                            .transport = AclTransportType::kBrEdr});

  // Push two channels sharing the same handle. Verify that only one link is
  // created.
  snapshot.l2cap.l2cap_channels.push_back(
      CreateL2capChannelSnapshot(kLeConnectionHandle1,
                                 kLocalCid1,
                                 kRemoteCid1,
                                 AclTransportType::kLe,
                                 L2capChannelMode::kCreditBasedFlowControl));
  snapshot.l2cap.l2cap_channels.push_back(
      CreateL2capChannelSnapshot(kLeConnectionHandle1,
                                 kLocalCid2,
                                 kRemoteCid2,
                                 AclTransportType::kLe,
                                 L2capChannelMode::kCreditBasedFlowControl));
  snapshot.l2cap.l2cap_signaling_states.push_back(L2capSignalingStateSnapshot{
      .connection_handle = kLeConnectionHandle1,
      .transport = AclTransportType::kLe,
  });

  snapshot.l2cap.l2cap_channels.push_back(
      CreateL2capChannelSnapshot(kBrEdrConnectionHandle,
                                 kLocalCid1,
                                 kRemoteCid1,
                                 AclTransportType::kBrEdr));
  snapshot.l2cap.l2cap_signaling_states.push_back(L2capSignalingStateSnapshot{
      .connection_handle = kBrEdrConnectionHandle,
      .transport = AclTransportType::kBrEdr,
  });

  PW_TEST_ASSERT_OK(proxy.RecoverFromSnapshot(snapshot));

  // Re-register the two channels sharing the same handle. This also verifies
  // that the LE connection was restored.
  Result<L2capCoc> coc1 = BuildCocWithResult(proxy,
                                             CocParameters{
                                                 .handle = kLeConnectionHandle1,
                                                 .local_cid = kLocalCid1,
                                                 .remote_cid = kRemoteCid1,
                                             });
  PW_TEST_ASSERT_OK(coc1.status());
  Result<L2capCoc> coc2 = BuildCocWithResult(proxy,
                                             CocParameters{
                                                 .handle = kLeConnectionHandle1,
                                                 .local_cid = kLocalCid2,
                                                 .remote_cid = kRemoteCid2,
                                             });
  PW_TEST_ASSERT_OK(coc2.status());

  proxy.CompleteRecovery();

  // Verify that only one logical link was created for the LE connection despite
  // two channels sharing the same handle. Expect next_identifier to be 3
  // (incremented once by each channel).
  PW_TEST_ASSERT_OK(SendLeReadBufferResponseFromController(proxy, 10));
  PW_TEST_ASSERT_OK(coc1.value().SendAdditionalRxCredits(5));
  PW_TEST_ASSERT_OK(coc2.value().SendAdditionalRxCredits(5));
  EXPECT_EQ(next_identifier, 3);

  // Verify that the BR/EDR connection was restored.
  Result<BasicL2capChannel> bredr_basic_channel =
      BuildBasicL2capChannelWithResult(
          proxy,
          BasicL2capParameters{
              .handle = kBrEdrConnectionHandle,
              .local_cid = kLocalCid1,
              .remote_cid = kRemoteCid1,
              .transport = AclTransportType::kBrEdr,
          });
  PW_TEST_EXPECT_OK(bredr_basic_channel.status());
}

TEST_F(L2capRecoveryTest, SnapshotRecoverHandlesInterruptedFrame) {
  Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/2,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator(),
                              nullptr);
  StartDispatcherOnCurrentThread(proxy);

  ProxyHostSnapshot snapshot;
  snapshot.acl.acl_connections.push_back(
      AclConnectionSnapshot{.connection_handle = kLeConnectionHandle1,
                            .transport = AclTransportType::kLe});

  // Populate snapshot with an interrupted frame.
  L2capChannelSnapshot channel_snapshot = CreateL2capChannelSnapshot();
  channel_snapshot.acl_recombination_in_progress = true;
  snapshot.l2cap.l2cap_channels.push_back(channel_snapshot);
  snapshot.l2cap.l2cap_signaling_states.push_back(L2capSignalingStateSnapshot{
      .connection_handle = kLeConnectionHandle1,
      .transport = AclTransportType::kLe,
  });

  PW_TEST_ASSERT_OK(proxy.RecoverFromSnapshot(snapshot));

  // Verify that channel registration during the recovery window fails.
  EXPECT_EQ(
      BuildBasicL2capChannelWithResult(proxy,
                                       BasicL2capParameters{
                                           .handle = kLeConnectionHandle1,
                                           .local_cid = kLocalCid1,
                                           .remote_cid = kRemoteCid1,
                                           .transport = AclTransportType::kLe,
                                       })
          .status(),
      Status::Cancelled());

  // Tear down the rejected silent channel.
  PW_TEST_ASSERT_OK(SendL2capDisconnectRsp(proxy,
                                           Direction::kFromController,
                                           AclTransportType::kLe,
                                           kLeConnectionHandle1,
                                           kLocalCid1,
                                           kRemoteCid1));

  // Complete recovery to close the recovery window.
  proxy.CompleteRecovery();

  // Verify that the saved snapshot with the interrupted frame is cleared and
  // channel registration now succeeds.
  Result<BasicL2capChannel> basic_channel =
      BuildBasicL2capChannelWithResult(proxy,
                                       BasicL2capParameters{
                                           .handle = kLeConnectionHandle1,
                                           .local_cid = kLocalCid1,
                                           .remote_cid = kRemoteCid1,
                                           .transport = AclTransportType::kLe,
                                       });
  PW_TEST_EXPECT_OK(basic_channel.status());

  // Test again to verify that snapshot recovery also works for an
  // already-established logical link.
  basic_channel->Close();
  PW_TEST_ASSERT_OK(proxy.RecoverFromSnapshot(snapshot));

  // Verify that channel registration during the recovery window fails.
  EXPECT_EQ(
      BuildBasicL2capChannelWithResult(proxy,
                                       BasicL2capParameters{
                                           .handle = kLeConnectionHandle1,
                                           .local_cid = kLocalCid1,
                                           .remote_cid = kRemoteCid1,
                                           .transport = AclTransportType::kLe,
                                       })
          .status(),
      Status::Cancelled());

  // Tear down the rejected silent channel.
  PW_TEST_ASSERT_OK(SendL2capDisconnectRsp(proxy,
                                           Direction::kFromController,
                                           AclTransportType::kLe,
                                           kLeConnectionHandle1,
                                           kLocalCid1,
                                           kRemoteCid1));

  // Complete recovery to close the recovery window.
  proxy.CompleteRecovery();

  // Verify that the saved snapshot with the interrupted frame is cleared and
  // channel registration now succeeds.
  PW_TEST_EXPECT_OK(
      BuildBasicL2capChannelWithResult(proxy,
                                       BasicL2capParameters{
                                           .handle = kLeConnectionHandle1,
                                           .local_cid = kLocalCid1,
                                           .remote_cid = kRemoteCid1,
                                           .transport = AclTransportType::kLe,
                                       })
          .status());
}

#if PW_BLUETOOTH_PROXY_CONFIG_ENABLE_CREDIT_SNAPSHOT_UPDATES
TEST_F(L2capRecoveryTest, SnapshotChannelMatchingOverridesCredits) {
  struct {
    uint16_t rx_credits = 0;
    uint16_t tx_credits = 0;
  } credit_capture;

  Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(
      std::move(send_to_host_fn),
      std::move(send_to_controller_fn),
      /*le_acl_credits_to_reserve=*/2,
      /*br_edr_acl_credits_to_reserve=*/0,
      GetProxyHostAllocator(),
      [&credit_capture](const ProxyHostStateUpdate& update) {
        if (auto* chan_snap = std::get_if<L2capChannelSnapshot>(&update);
            chan_snap && chan_snap->local_cid == kLocalCid1) {
          credit_capture.rx_credits = chan_snap->rx_engine.remaining_credits;
          credit_capture.tx_credits = chan_snap->tx_engine.remaining_credits;
        }
      });
  StartDispatcherOnCurrentThread(proxy);

  ProxyHostSnapshot snapshot;
  snapshot.acl.acl_connections.push_back(
      AclConnectionSnapshot{.connection_handle = kLeConnectionHandle1,
                            .transport = AclTransportType::kLe});

  L2capChannelSnapshot channel_snapshot = CreateL2capChannelSnapshot();
  channel_snapshot.mode = L2capChannelMode::kCreditBasedFlowControl;
  channel_snapshot.rx_engine.remaining_credits = 10;
  channel_snapshot.tx_engine.remaining_credits = 20;
  snapshot.l2cap.l2cap_channels.push_back(channel_snapshot);
  snapshot.l2cap.l2cap_signaling_states.push_back(L2capSignalingStateSnapshot{
      .connection_handle = kLeConnectionHandle1,
      .transport = AclTransportType::kLe,
  });

  PW_TEST_ASSERT_OK(proxy.RecoverFromSnapshot(snapshot));

  Result<L2capCoc> coc = BuildCocWithResult(proxy,
                                            CocParameters{
                                                .handle = kLeConnectionHandle1,
                                                .local_cid = kLocalCid1,
                                                .remote_cid = kRemoteCid1,
                                                .rx_credits = 1,
                                                .tx_credits = 1,
                                            });
  PW_TEST_ASSERT_OK(coc.status());

  proxy.CompleteRecovery();

  // Verify that snapshot credits overrode client-provided initial credits.
  // Expect rx_credits to be 15 (10 restored + 5 added).
  PW_TEST_ASSERT_OK(SendLeReadBufferResponseFromController(proxy, 10));
  PW_TEST_ASSERT_OK(coc.value().SendAdditionalRxCredits(5));
  EXPECT_EQ(credit_capture.rx_credits, 15);
  EXPECT_EQ(credit_capture.tx_credits, 20);
}
#endif  // PW_BLUETOOTH_PROXY_CONFIG_ENABLE_CREDIT_SNAPSHOT_UPDATES

TEST_F(L2capRecoveryTest, SnapshotUnmatchedChannelRegistration) {
  Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/2,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator(),
                              nullptr);
  StartDispatcherOnCurrentThread(proxy);

  ProxyHostSnapshot snapshot;
  snapshot.acl.acl_connections.push_back(
      AclConnectionSnapshot{.connection_handle = kLeConnectionHandle1,
                            .transport = AclTransportType::kLe});

  L2capChannelSnapshot channel_snapshot = CreateL2capChannelSnapshot();
  channel_snapshot.acl_recombination_in_progress = true;
  snapshot.l2cap.l2cap_channels.push_back(channel_snapshot);
  snapshot.l2cap.l2cap_signaling_states.push_back(L2capSignalingStateSnapshot{
      .connection_handle = kLeConnectionHandle1,
      .transport = AclTransportType::kLe,
  });

  PW_TEST_ASSERT_OK(proxy.RecoverFromSnapshot(snapshot));

  // Verify that registering a channel with a CID that is not in the snapshot
  // succeeds during the recovery window.
  PW_TEST_EXPECT_OK(
      BuildBasicL2capChannelWithResult(proxy,
                                       BasicL2capParameters{
                                           .handle = kLeConnectionHandle1,
                                           .local_cid = kLocalCid2,
                                           .remote_cid = kRemoteCid2,
                                           .transport = AclTransportType::kLe,
                                       })
          .status());
}

TEST_F(L2capRecoveryTest, RejectedBasicModeChannelSilentlyAbsorbsPackets) {
  Vector<L2capChannelRemoved, 1> removed_channels;

  int sent_to_host_count = 0;
  Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [&sent_to_host_count](H4PacketWithHci&&) { ++sent_to_host_count; });

  int sent_to_controller_count = 0;
  Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [&sent_to_controller_count](H4PacketWithH4&&) {
        ++sent_to_controller_count;
      });

  ProxyHost proxy = ProxyHost(
      std::move(send_to_host_fn),
      std::move(send_to_controller_fn),
      /*le_acl_credits_to_reserve=*/2,
      /*br_edr_acl_credits_to_reserve=*/0,
      GetProxyHostAllocator(),
      [&removed_channels](const ProxyHostStateUpdate& update) {
        if (auto* removed = std::get_if<L2capChannelRemoved>(&update)) {
          removed_channels.push_back(*removed);
        }
      });
  StartDispatcherOnCurrentThread(proxy);

  ProxyHostSnapshot snapshot;
  snapshot.acl.acl_connections.push_back(
      AclConnectionSnapshot{.connection_handle = kLeConnectionHandle1,
                            .transport = AclTransportType::kLe});

  // Populate snapshot with an interrupted frame.
  L2capChannelSnapshot channel_snapshot = CreateL2capChannelSnapshot();
  channel_snapshot.acl_recombination_in_progress = true;
  snapshot.l2cap.l2cap_channels.push_back(channel_snapshot);
  snapshot.l2cap.l2cap_signaling_states.push_back(L2capSignalingStateSnapshot{
      .connection_handle = kLeConnectionHandle1,
      .transport = AclTransportType::kLe,
  });

  PW_TEST_ASSERT_OK(proxy.RecoverFromSnapshot(snapshot));

  L2capChannelManagerInterface::SpanReceiveFunction rx_fn =
      [](span<const std::byte>, ConnectionHandle, uint16_t, uint16_t) {
        return true;
      };
  L2capChannelManagerInterface::SpanReceiveFunction tx_fn =
      [](span<const std::byte>, ConnectionHandle, uint16_t, uint16_t) {
        return true;
      };
  EXPECT_EQ(
      proxy
          .InterceptBasicModeChannel(ConnectionHandle{kLeConnectionHandle1},
                                     kLocalCid1,
                                     kRemoteCid1,
                                     AclTransportType::kLe,
                                     std::move(rx_fn),
                                     std::move(tx_fn),
                                     /*event_fn=*/nullptr)
          .status(),
      Status::Cancelled());

  // Verify that packets are silently absorbed in both directions.
  std::array<uint8_t, 3> payload = {1, 2, 3};
  SendL2capBFrame(
      proxy, kLeConnectionHandle1, payload, payload.size(), kLocalCid1);
  EXPECT_EQ(sent_to_host_count, 0);

  Result<BFrameWithStorage> host_packet_result =
      SetupBFrame(kLeConnectionHandle1, kRemoteCid1, 4);
  PW_TEST_ASSERT_OK(host_packet_result.status());
  proxy.HandleH4HciFromHost(
      H4PacketWithH4(host_packet_result.value().acl.h4_span()));
  EXPECT_EQ(sent_to_controller_count, 0);

  // Verify that channels are removed upon disconnection.
  PW_TEST_ASSERT_OK(SendL2capDisconnectRsp(proxy,
                                           Direction::kFromController,
                                           AclTransportType::kLe,
                                           kLeConnectionHandle1,
                                           kLocalCid1,
                                           kRemoteCid1));
  ASSERT_EQ(removed_channels.size(), 1u);
  EXPECT_EQ(removed_channels[0].connection_handle, kLeConnectionHandle1);
  EXPECT_EQ(removed_channels[0].local_cid, kLocalCid1);
}

TEST_F(L2capRecoveryTest,
       RejectedCreditBasedFlowControlChannelSilentlyAbsorbsPackets) {
  Vector<L2capChannelRemoved, 1> removed_channels;

  int sent_to_host_count = 0;
  Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [&sent_to_host_count](H4PacketWithHci&&) { ++sent_to_host_count; });

  int sent_to_controller_count = 0;
  Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [&sent_to_controller_count](H4PacketWithH4&&) {
        ++sent_to_controller_count;
      });

  ProxyHost proxy = ProxyHost(
      std::move(send_to_host_fn),
      std::move(send_to_controller_fn),
      /*le_acl_credits_to_reserve=*/2,
      /*br_edr_acl_credits_to_reserve=*/0,
      GetProxyHostAllocator(),
      [&removed_channels](const ProxyHostStateUpdate& update) {
        if (auto* removed = std::get_if<L2capChannelRemoved>(&update)) {
          removed_channels.push_back(*removed);
        }
      });
  StartDispatcherOnCurrentThread(proxy);

  // Populate snapshot with a queued packet, indicating packet loss.
  ProxyHostSnapshot snapshot;
  AclConnectionSnapshot connection_snapshot;
  connection_snapshot.connection_handle = kLeConnectionHandle1;
  connection_snapshot.transport = AclTransportType::kLe;
  connection_snapshot.num_queued_host_packets = 1;
  snapshot.acl.acl_connections.push_back(connection_snapshot);

  L2capChannelSnapshot channel_snapshot = CreateL2capChannelSnapshot();
  channel_snapshot.mode = L2capChannelMode::kCreditBasedFlowControl;
  snapshot.l2cap.l2cap_channels.push_back(channel_snapshot);
  snapshot.l2cap.l2cap_signaling_states.push_back(L2capSignalingStateSnapshot{
      .connection_handle = kLeConnectionHandle1,
      .transport = AclTransportType::kLe,
  });

  PW_TEST_ASSERT_OK(proxy.RecoverFromSnapshot(snapshot));

  ConnectionOrientedChannelConfig rx_config{
      .cid = kLocalCid1, .mtu = kMtu, .mps = kMps, .credits = 1};
  ConnectionOrientedChannelConfig tx_config{
      .cid = kRemoteCid1, .mtu = kMtu, .mps = kMps, .credits = 1};
  MultiBufReceiveFunction rx_fn = [](multibuf::MultiBuf&&) {};
  EXPECT_EQ(proxy
                .InterceptCreditBasedFlowControlChannel(
                    ConnectionHandle{kLeConnectionHandle1},
                    rx_config,
                    tx_config,
                    std::move(rx_fn),
                    /*event_fn=*/nullptr)
                .status(),
            Status::Cancelled());

  // Verify that packets are silently absorbed.
  std::array<uint8_t, 3> payload = {1, 2, 3};
  SendL2capBFrame(
      proxy, kLeConnectionHandle1, payload, payload.size(), kLocalCid1);
  EXPECT_EQ(sent_to_host_count, 0);

  // Verify that channels are removed upon disconnection.
  PW_TEST_ASSERT_OK(SendL2capDisconnectRsp(proxy,
                                           Direction::kFromController,
                                           AclTransportType::kLe,
                                           kLeConnectionHandle1,
                                           kLocalCid1,
                                           kRemoteCid1));
  ASSERT_EQ(removed_channels.size(), 1u);
  EXPECT_EQ(removed_channels[0].connection_handle, kLeConnectionHandle1);
  EXPECT_EQ(removed_channels[0].local_cid, kLocalCid1);
}

TEST_F(L2capRecoveryTest,
       RejectedAcquiredBasicModeChannelSilentlyAbsorbsPackets) {
  Vector<L2capChannelRemoved, 1> removed_channels;

  int sent_to_host_count = 0;
  Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [&sent_to_host_count](H4PacketWithHci&&) { ++sent_to_host_count; });

  int sent_to_controller_count = 0;
  Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [&sent_to_controller_count](H4PacketWithH4&&) {
        ++sent_to_controller_count;
      });

  ProxyHost proxy = ProxyHost(
      std::move(send_to_host_fn),
      std::move(send_to_controller_fn),
      /*le_acl_credits_to_reserve=*/2,
      /*br_edr_acl_credits_to_reserve=*/0,
      GetProxyHostAllocator(),
      [&removed_channels](const ProxyHostStateUpdate& update) {
        if (auto* removed = std::get_if<L2capChannelRemoved>(&update)) {
          removed_channels.push_back(*removed);
        }
      });
  StartDispatcherOnCurrentThread(proxy);

  ProxyHostSnapshot snapshot;
  snapshot.acl.acl_connections.push_back(
      AclConnectionSnapshot{.connection_handle = kLeConnectionHandle1,
                            .transport = AclTransportType::kLe});

  // Populate snapshot with an interrupted frame.
  L2capChannelSnapshot channel_snapshot = CreateL2capChannelSnapshot();
  channel_snapshot.acl_recombination_in_progress = true;
  snapshot.l2cap.l2cap_channels.push_back(channel_snapshot);
  snapshot.l2cap.l2cap_signaling_states.push_back(L2capSignalingStateSnapshot{
      .connection_handle = kLeConnectionHandle1,
      .transport = AclTransportType::kLe,
  });

  PW_TEST_ASSERT_OK(proxy.RecoverFromSnapshot(snapshot));

  EXPECT_EQ(
      BuildBasicL2capChannelWithResult(proxy,
                                       BasicL2capParameters{
                                           .handle = kLeConnectionHandle1,
                                           .local_cid = kLocalCid1,
                                           .remote_cid = kRemoteCid1,
                                           .transport = AclTransportType::kLe,
                                       })
          .status(),
      Status::Cancelled());

  // Verify that packets are silently absorbed in both directions.
  std::array<uint8_t, 3> payload = {1, 2, 3};
  SendL2capBFrame(
      proxy, kLeConnectionHandle1, payload, payload.size(), kLocalCid1);
  EXPECT_EQ(sent_to_host_count, 0);

  Result<BFrameWithStorage> host_packet_result =
      SetupBFrame(kLeConnectionHandle1, kRemoteCid1, 4);
  PW_TEST_ASSERT_OK(host_packet_result.status());
  proxy.HandleH4HciFromHost(
      H4PacketWithH4(host_packet_result.value().acl.h4_span()));
  EXPECT_EQ(sent_to_controller_count, 0);

  // Verify that channels are removed upon disconnection.
  PW_TEST_ASSERT_OK(SendL2capDisconnectRsp(proxy,
                                           Direction::kFromController,
                                           AclTransportType::kLe,
                                           kLeConnectionHandle1,
                                           kLocalCid1,
                                           kRemoteCid1));
  ASSERT_EQ(removed_channels.size(), 1u);
  EXPECT_EQ(removed_channels[0].connection_handle, kLeConnectionHandle1);
  EXPECT_EQ(removed_channels[0].local_cid, kLocalCid1);
}

TEST_F(L2capRecoveryTest,
       RejectedAcquiredCreditBasedFlowControlChannelSilentlyAbsorbsPackets) {
  Vector<L2capChannelRemoved, 1> removed_channels;

  int sent_to_host_count = 0;
  Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [&sent_to_host_count](H4PacketWithHci&&) { ++sent_to_host_count; });

  int sent_to_controller_count = 0;
  Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [&sent_to_controller_count](H4PacketWithH4&&) {
        ++sent_to_controller_count;
      });

  ProxyHost proxy = ProxyHost(
      std::move(send_to_host_fn),
      std::move(send_to_controller_fn),
      /*le_acl_credits_to_reserve=*/2,
      /*br_edr_acl_credits_to_reserve=*/0,
      GetProxyHostAllocator(),
      [&removed_channels](const ProxyHostStateUpdate& update) {
        if (auto* removed = std::get_if<L2capChannelRemoved>(&update)) {
          removed_channels.push_back(*removed);
        }
      });
  StartDispatcherOnCurrentThread(proxy);

  // Populate snapshot with a queued packet, indicating packet loss.
  ProxyHostSnapshot snapshot;
  AclConnectionSnapshot connection_snapshot;
  connection_snapshot.connection_handle = kLeConnectionHandle1;
  connection_snapshot.transport = AclTransportType::kLe;
  connection_snapshot.num_queued_host_packets = 1;
  snapshot.acl.acl_connections.push_back(connection_snapshot);

  L2capChannelSnapshot channel_snapshot = CreateL2capChannelSnapshot();
  channel_snapshot.mode = L2capChannelMode::kCreditBasedFlowControl;
  snapshot.l2cap.l2cap_channels.push_back(channel_snapshot);
  snapshot.l2cap.l2cap_signaling_states.push_back(L2capSignalingStateSnapshot{
      .connection_handle = kLeConnectionHandle1,
      .transport = AclTransportType::kLe,
  });

  PW_TEST_ASSERT_OK(proxy.RecoverFromSnapshot(snapshot));

  EXPECT_EQ(BuildCocWithResult(proxy,
                               CocParameters{
                                   .handle = kLeConnectionHandle1,
                                   .local_cid = kLocalCid1,
                                   .remote_cid = kRemoteCid1,
                                   .rx_mtu = kMtu,
                                   .rx_mps = kMps,
                                   .rx_credits = 1,
                                   .tx_mtu = kMtu,
                                   .tx_mps = kMps,
                                   .tx_credits = 1,
                               })
                .status(),
            Status::Cancelled());

  // Verify that packets are silently absorbed.
  std::array<uint8_t, 3> payload = {1, 2, 3};
  SendL2capBFrame(
      proxy, kLeConnectionHandle1, payload, payload.size(), kLocalCid1);
  EXPECT_EQ(sent_to_host_count, 0);

  // Verify that channels are removed upon disconnection.
  PW_TEST_ASSERT_OK(SendL2capDisconnectRsp(proxy,
                                           Direction::kFromController,
                                           AclTransportType::kLe,
                                           kLeConnectionHandle1,
                                           kLocalCid1,
                                           kRemoteCid1));
  ASSERT_EQ(removed_channels.size(), 1u);
  EXPECT_EQ(removed_channels[0].connection_handle, kLeConnectionHandle1);
  EXPECT_EQ(removed_channels[0].local_cid, kLocalCid1);
}

TEST_F(L2capRecoveryTest, RejectedChannelIsTornDownOnHostDisconnection) {
  Vector<L2capChannelRemoved, 1> removed_channels;

  int sent_to_host_count = 0;
  Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [&sent_to_host_count](H4PacketWithHci&&) { ++sent_to_host_count; });

  int sent_to_controller_count = 0;
  Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [&sent_to_controller_count](H4PacketWithH4&&) {
        ++sent_to_controller_count;
      });

  ProxyHost proxy = ProxyHost(
      std::move(send_to_host_fn),
      std::move(send_to_controller_fn),
      /*le_acl_credits_to_reserve=*/2,
      /*br_edr_acl_credits_to_reserve=*/0,
      GetProxyHostAllocator(),
      [&removed_channels](const ProxyHostStateUpdate& update) {
        if (auto* removed = std::get_if<L2capChannelRemoved>(&update)) {
          removed_channels.push_back(*removed);
        }
      });
  StartDispatcherOnCurrentThread(proxy);

  // Populate snapshot with a queued packet, indicating packet loss.
  ProxyHostSnapshot snapshot;
  AclConnectionSnapshot connection_snapshot;
  connection_snapshot.connection_handle = kLeConnectionHandle1;
  connection_snapshot.transport = AclTransportType::kLe;
  connection_snapshot.num_queued_host_packets = 1;
  snapshot.acl.acl_connections.push_back(connection_snapshot);

  L2capChannelSnapshot channel_snapshot = CreateL2capChannelSnapshot();
  channel_snapshot.mode = L2capChannelMode::kCreditBasedFlowControl;
  snapshot.l2cap.l2cap_channels.push_back(channel_snapshot);
  snapshot.l2cap.l2cap_signaling_states.push_back(L2capSignalingStateSnapshot{
      .connection_handle = kLeConnectionHandle1,
      .transport = AclTransportType::kLe,
  });

  PW_TEST_ASSERT_OK(proxy.RecoverFromSnapshot(snapshot));

  ConnectionOrientedChannelConfig rx_config{
      .cid = kLocalCid1, .mtu = 100, .mps = 100, .credits = 1};
  ConnectionOrientedChannelConfig tx_config{
      .cid = kRemoteCid1, .mtu = 100, .mps = 100, .credits = 1};
  MultiBufReceiveFunction rx_fn = [](multibuf::MultiBuf&&) {};
  EXPECT_EQ(proxy
                .InterceptCreditBasedFlowControlChannel(
                    ConnectionHandle{kLeConnectionHandle1},
                    rx_config,
                    tx_config,
                    std::move(rx_fn),
                    /*event_fn=*/nullptr)
                .status(),
            Status::Cancelled());

  // Verify that channels are removed upon host disconnection. The source and
  // destination CIDs are swapped because of the reversed direction.
  PW_TEST_ASSERT_OK(SendL2capDisconnectRsp(proxy,
                                           Direction::kFromHost,
                                           AclTransportType::kLe,
                                           kLeConnectionHandle1,
                                           kRemoteCid1,
                                           kLocalCid1));
  ASSERT_EQ(removed_channels.size(), 1u);
  EXPECT_EQ(removed_channels[0].connection_handle, kLeConnectionHandle1);
  EXPECT_EQ(removed_channels[0].local_cid, kLocalCid1);
}

TEST_F(L2capRecoveryTest, BasicModeChannelAllowsDataLoss) {
  Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/2,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator(),
                              nullptr);
  StartDispatcherOnCurrentThread(proxy);

  // Populate snapshot with a queued packet, indicating packet loss.
  ProxyHostSnapshot snapshot;
  AclConnectionSnapshot connection_snapshot;
  connection_snapshot.connection_handle = kLeConnectionHandle1;
  connection_snapshot.transport = AclTransportType::kLe;
  connection_snapshot.num_queued_host_packets = 1;
  snapshot.acl.acl_connections.push_back(connection_snapshot);

  L2capChannelSnapshot channel_snapshot = CreateL2capChannelSnapshot();
  channel_snapshot.mode = L2capChannelMode::kBasic;
  channel_snapshot.allow_data_loss = true;
  snapshot.l2cap.l2cap_channels.push_back(channel_snapshot);
  snapshot.l2cap.l2cap_signaling_states.push_back(L2capSignalingStateSnapshot{
      .connection_handle = kLeConnectionHandle1,
      .transport = AclTransportType::kLe,
  });

  PW_TEST_ASSERT_OK(proxy.RecoverFromSnapshot(snapshot));

  // Verify that snapshot recovery succeeds, despite the packet loss.
  Result<BasicL2capChannel> basic_channel =
      BuildBasicL2capChannelWithResult(proxy,
                                       BasicL2capParameters{
                                           .handle = kLeConnectionHandle1,
                                           .local_cid = kLocalCid1,
                                           .remote_cid = kRemoteCid1,
                                           .transport = AclTransportType::kLe,
                                       });
  PW_TEST_EXPECT_OK(basic_channel.status());
}

TEST_F(L2capRecoveryTest, InterceptedBasicModeChannelAllowsDataLoss) {
  Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/2,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator(),
                              nullptr);
  StartDispatcherOnCurrentThread(proxy);

  // Populate snapshot with an interrupted frame.
  ProxyHostSnapshot snapshot;
  snapshot.acl.acl_connections.push_back(
      AclConnectionSnapshot{.connection_handle = kLeConnectionHandle1,
                            .transport = AclTransportType::kLe});

  L2capChannelSnapshot channel_snapshot = CreateL2capChannelSnapshot();
  channel_snapshot.acl_recombination_in_progress = true;
  channel_snapshot.allow_data_loss = true;
  snapshot.l2cap.l2cap_channels.push_back(channel_snapshot);
  snapshot.l2cap.l2cap_signaling_states.push_back(L2capSignalingStateSnapshot{
      .connection_handle = kLeConnectionHandle1,
      .transport = AclTransportType::kLe,
  });

  PW_TEST_ASSERT_OK(proxy.RecoverFromSnapshot(snapshot));
  PW_TEST_ASSERT_OK(SendLeReadBufferResponseFromController(proxy, 2));

  L2capChannelManagerInterface::SpanReceiveFunction rx_fn =
      [](span<const std::byte>, ConnectionHandle, uint16_t, uint16_t) {
        return true;
      };
  L2capChannelManagerInterface::SpanReceiveFunction tx_fn =
      [](span<const std::byte>, ConnectionHandle, uint16_t, uint16_t) {
        return true;
      };
  PW_TEST_EXPECT_OK(
      proxy
          .InterceptBasicModeChannel(ConnectionHandle{kLeConnectionHandle1},
                                     kLocalCid1,
                                     kRemoteCid1,
                                     AclTransportType::kLe,
                                     std::move(rx_fn),
                                     std::move(tx_fn),
                                     /*event_fn=*/nullptr,
                                     /*allow_data_loss=*/true)
          .status());
}

TEST_F(L2capRecoveryTest, CreditBasedFlowControlChannelAllowsDataLoss) {
  Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/2,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator(),
                              nullptr);
  StartDispatcherOnCurrentThread(proxy);

  // Populate snapshot with a queued packet, indicating packet loss.
  ProxyHostSnapshot snapshot;
  AclConnectionSnapshot connection_snapshot;
  connection_snapshot.connection_handle = kLeConnectionHandle1;
  connection_snapshot.transport = AclTransportType::kLe;
  connection_snapshot.num_queued_host_packets = 1;
  snapshot.acl.acl_connections.push_back(connection_snapshot);

  L2capChannelSnapshot channel_snapshot = CreateL2capChannelSnapshot();
  channel_snapshot.mode = L2capChannelMode::kCreditBasedFlowControl;
  channel_snapshot.allow_data_loss = true;
  snapshot.l2cap.l2cap_channels.push_back(channel_snapshot);
  snapshot.l2cap.l2cap_signaling_states.push_back(L2capSignalingStateSnapshot{
      .connection_handle = kLeConnectionHandle1,
      .transport = AclTransportType::kLe,
  });

  PW_TEST_ASSERT_OK(proxy.RecoverFromSnapshot(snapshot));

  ConnectionOrientedChannelConfig rx_config{.cid = kLocalCid1,
                                            .mtu = kMtu,
                                            .mps = kMps,
                                            .credits = 1,
                                            .allow_data_loss = true};
  ConnectionOrientedChannelConfig tx_config{.cid = kRemoteCid1,
                                            .mtu = kMtu,
                                            .mps = kMps,
                                            .credits = 1,
                                            .allow_data_loss = true};
  MultiBufReceiveFunction rx_fn = [](multibuf::MultiBuf&&) {};

  PW_TEST_ASSERT_OK(SendLeReadBufferResponseFromController(proxy, 2));

  // Verify that snapshot recovery succeeds, despite the packet loss.
  PW_TEST_EXPECT_OK(proxy
                        .InterceptCreditBasedFlowControlChannel(
                            ConnectionHandle{kLeConnectionHandle1},
                            rx_config,
                            tx_config,
                            std::move(rx_fn),
                            /*event_fn=*/nullptr)
                        .status());
}

TEST_F(L2capRecoveryTest, CompleteRecoverySweepsAbandonedChannels) {
  Vector<L2capChannelRemoved, 2> removed_channels;

  Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(
      std::move(send_to_host_fn),
      std::move(send_to_controller_fn),
      /*le_acl_credits_to_reserve=*/2,
      /*br_edr_acl_credits_to_reserve=*/0,
      GetProxyHostAllocator(),
      [&removed_channels](const ProxyHostStateUpdate& update) {
        if (auto* removed = std::get_if<L2capChannelRemoved>(&update)) {
          removed_channels.push_back(*removed);
        }
      });
  StartDispatcherOnCurrentThread(proxy);

  ProxyHostSnapshot snapshot;
  snapshot.acl.acl_connections.push_back(
      AclConnectionSnapshot{.connection_handle = kLeConnectionHandle1,
                            .transport = AclTransportType::kLe});

  snapshot.l2cap.l2cap_channels.push_back(CreateL2capChannelSnapshot(
      kLeConnectionHandle1, kLocalCid1, kRemoteCid1));
  snapshot.l2cap.l2cap_channels.push_back(CreateL2capChannelSnapshot(
      kLeConnectionHandle1, kLocalCid2, kRemoteCid2));
  snapshot.l2cap.l2cap_signaling_states.push_back(L2capSignalingStateSnapshot{
      .connection_handle = kLeConnectionHandle1,
      .transport = AclTransportType::kLe,
  });

  PW_TEST_ASSERT_OK(proxy.RecoverFromSnapshot(snapshot));

  Result<BasicL2capChannel> channel1 =
      BuildBasicL2capChannelWithResult(proxy,
                                       BasicL2capParameters{
                                           .handle = kLeConnectionHandle1,
                                           .local_cid = kLocalCid1,
                                           .remote_cid = kRemoteCid1,
                                           .transport = AclTransportType::kLe,
                                       });
  PW_TEST_EXPECT_OK(channel1.status());

  proxy.CompleteRecovery();

  // Verify that the first channel was re-registered and the second channel was
  // removed.
  ASSERT_EQ(removed_channels.size(), 1u);
  EXPECT_EQ(removed_channels[0].connection_handle, kLeConnectionHandle1);
  EXPECT_EQ(removed_channels[0].local_cid, kLocalCid2);
}

TEST_F(L2capRecoveryTest, CompleteRecoveryHandlesAllChannelsAbandoned) {
  Vector<L2capChannelRemoved, 2> removed_channels;

  Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(
      std::move(send_to_host_fn),
      std::move(send_to_controller_fn),
      /*le_acl_credits_to_reserve=*/2,
      /*br_edr_acl_credits_to_reserve=*/0,
      GetProxyHostAllocator(),
      [&removed_channels](const ProxyHostStateUpdate& update) {
        if (auto* removed = std::get_if<L2capChannelRemoved>(&update)) {
          removed_channels.push_back(*removed);
        }
      });
  StartDispatcherOnCurrentThread(proxy);

  ProxyHostSnapshot snapshot;
  snapshot.acl.acl_connections.push_back(
      AclConnectionSnapshot{.connection_handle = kLeConnectionHandle1,
                            .transport = AclTransportType::kLe});

  snapshot.l2cap.l2cap_channels.push_back(CreateL2capChannelSnapshot(
      kLeConnectionHandle1, kLocalCid1, kRemoteCid1));
  snapshot.l2cap.l2cap_channels.push_back(CreateL2capChannelSnapshot(
      kLeConnectionHandle1, kLocalCid2, kRemoteCid2));
  snapshot.l2cap.l2cap_signaling_states.push_back(L2capSignalingStateSnapshot{
      .connection_handle = kLeConnectionHandle1,
      .transport = AclTransportType::kLe,
  });

  PW_TEST_ASSERT_OK(proxy.RecoverFromSnapshot(snapshot));

  proxy.CompleteRecovery();

  // Verify that both channels were removed.
  ASSERT_EQ(removed_channels.size(), 2u);
  EXPECT_EQ(removed_channels[0].local_cid, kLocalCid1);
  EXPECT_EQ(removed_channels[1].local_cid, kLocalCid2);
}

TEST_F(L2capRecoveryTest, CompleteRecoveryHandlesAllChannelsReRegistered) {
  Vector<L2capChannelRemoved, 2> removed_channels;

  Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(
      std::move(send_to_host_fn),
      std::move(send_to_controller_fn),
      /*le_acl_credits_to_reserve=*/2,
      /*br_edr_acl_credits_to_reserve=*/0,
      GetProxyHostAllocator(),
      [&removed_channels](const ProxyHostStateUpdate& update) {
        if (auto* removed = std::get_if<L2capChannelRemoved>(&update)) {
          removed_channels.push_back(*removed);
        }
      });
  StartDispatcherOnCurrentThread(proxy);

  ProxyHostSnapshot snapshot;
  snapshot.acl.acl_connections.push_back(
      AclConnectionSnapshot{.connection_handle = kLeConnectionHandle1,
                            .transport = AclTransportType::kLe});

  snapshot.l2cap.l2cap_channels.push_back(CreateL2capChannelSnapshot(
      kLeConnectionHandle1, kLocalCid1, kRemoteCid1));
  snapshot.l2cap.l2cap_channels.push_back(CreateL2capChannelSnapshot(
      kLeConnectionHandle1, kLocalCid2, kRemoteCid2));
  snapshot.l2cap.l2cap_signaling_states.push_back(L2capSignalingStateSnapshot{
      .connection_handle = kLeConnectionHandle1,
      .transport = AclTransportType::kLe,
  });

  PW_TEST_ASSERT_OK(proxy.RecoverFromSnapshot(snapshot));

  Result<BasicL2capChannel> channel1 =
      BuildBasicL2capChannelWithResult(proxy,
                                       BasicL2capParameters{
                                           .handle = kLeConnectionHandle1,
                                           .local_cid = kLocalCid1,
                                           .remote_cid = kRemoteCid1,
                                           .transport = AclTransportType::kLe,
                                       });
  PW_TEST_EXPECT_OK(channel1.status());

  Result<BasicL2capChannel> channel2 =
      BuildBasicL2capChannelWithResult(proxy,
                                       BasicL2capParameters{
                                           .handle = kLeConnectionHandle1,
                                           .local_cid = kLocalCid2,
                                           .remote_cid = kRemoteCid2,
                                           .transport = AclTransportType::kLe,
                                       });
  PW_TEST_EXPECT_OK(channel2.status());

  proxy.CompleteRecovery();

  // Verify that both channels were re-registered.
  EXPECT_TRUE(removed_channels.empty());
}

TEST_F(L2capRecoveryTest, CompleteRecoveryIsIdempotent) {
  Vector<L2capChannelRemoved, 4> removed_channels;

  Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(
      std::move(send_to_host_fn),
      std::move(send_to_controller_fn),
      /*le_acl_credits_to_reserve=*/2,
      /*br_edr_acl_credits_to_reserve=*/0,
      GetProxyHostAllocator(),
      [&removed_channels](const ProxyHostStateUpdate& update) {
        if (auto* removed = std::get_if<L2capChannelRemoved>(&update)) {
          removed_channels.push_back(*removed);
        }
      });
  StartDispatcherOnCurrentThread(proxy);

  ProxyHostSnapshot snapshot;
  snapshot.acl.acl_connections.push_back(
      AclConnectionSnapshot{.connection_handle = kLeConnectionHandle1,
                            .transport = AclTransportType::kLe});

  snapshot.l2cap.l2cap_channels.push_back(CreateL2capChannelSnapshot(
      kLeConnectionHandle1, kLocalCid1, kRemoteCid1));
  snapshot.l2cap.l2cap_channels.push_back(CreateL2capChannelSnapshot(
      kLeConnectionHandle1, kLocalCid2, kRemoteCid2));
  snapshot.l2cap.l2cap_signaling_states.push_back(L2capSignalingStateSnapshot{
      .connection_handle = kLeConnectionHandle1,
      .transport = AclTransportType::kLe,
  });

  PW_TEST_ASSERT_OK(proxy.RecoverFromSnapshot(snapshot));

  // Verify that repeated CompleteL2capRecovery() calls are safe and have no
  // additional effect.
  proxy.CompleteRecovery();
  ASSERT_EQ(removed_channels.size(), 2u);
  proxy.CompleteRecovery();
  EXPECT_EQ(removed_channels.size(), 2u);
}

TEST(ProxyHostSnapshotTest, ProxyHostSnapshotApplyStateUpdate) {
  ProxyHostSnapshot snapshot;

  AclConnectionSnapshot connection_snapshot{.connection_handle =
                                                kLeConnectionHandle1};
  PW_TEST_EXPECT_OK(snapshot.ApplyStateUpdate(connection_snapshot));
  EXPECT_EQ(snapshot.acl.acl_connections.size(), 1u);

  AclConnectionRemoved connection_removed{.connection_handle =
                                              kLeConnectionHandle1};
  PW_TEST_EXPECT_OK(snapshot.ApplyStateUpdate(connection_removed));
  EXPECT_EQ(snapshot.acl.acl_connections.size(), 0u);

  L2capSignalingStateSnapshot signaling_snapshot;
  PW_TEST_EXPECT_OK(snapshot.ApplyStateUpdate(signaling_snapshot));
  EXPECT_EQ(snapshot.l2cap.l2cap_signaling_states.size(), 1u);

  L2capChannelSnapshot channel_snapshot;
  channel_snapshot.connection_handle = kLeConnectionHandle1;
  channel_snapshot.local_cid = kLocalCid1;
  PW_TEST_EXPECT_OK(snapshot.ApplyStateUpdate(channel_snapshot));
  EXPECT_EQ(snapshot.l2cap.l2cap_channels.size(), 1u);

  L2capChannelRemoved channel_removed{.connection_handle = kLeConnectionHandle1,
                                      .local_cid = kLocalCid1};
  PW_TEST_EXPECT_OK(snapshot.ApplyStateUpdate(channel_removed));
  EXPECT_EQ(snapshot.l2cap.l2cap_channels.size(), 0u);
}

}  // namespace
}  // namespace pw::bluetooth::proxy

#endif  // PW_BLUETOOTH_PROXY_CONFIG_ENABLE_RECOVERY
