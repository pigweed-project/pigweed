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

class AclRecoveryTest : public ProxyHostTest {
 protected:
  static AclConnectionSnapshot CreateAclConnectionSnapshot(
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

  Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/2,
                              /*br_edr_acl_credits_to_reserve=*/2,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  AclSnapshot snapshot =
      CreateAclSnapshot(kLeMaxAclCredits, kBrEdrMaxAclCredits);
  snapshot.acl_connections.push_back(
      CreateAclConnectionSnapshot(kLeConnectionHandle1,
                                  AclTransportType::kLe,
                                  /*num_proxy_pending=*/kLePendingAclCredits,
                                  /*num_host_pending=*/2,
                                  /*num_queued_host=*/0));
  snapshot.acl_connections.push_back(
      CreateAclConnectionSnapshot(kBrEdrConnectionHandle,
                                  AclTransportType::kBrEdr,
                                  /*num_proxy_pending=*/kBrEdrPendingAclCredits,
                                  /*num_host_pending=*/1,
                                  /*num_queued_host=*/0));

  PW_TEST_ASSERT_OK(proxy.RecoverAclFromSnapshot(snapshot));

  std::optional<AclConnectionSnapshot> connection_snapshot;
  proxy.RegisterAclStateUpdateCallback(
      [&connection_snapshot](const AclStateUpdate& update) {
        connection_snapshot = update.connection;
      });

  Result<AclFrameWithStorage> acl_frame = SetupAcl(kLeConnectionHandle1, 10);
  ASSERT_TRUE(acl_frame.ok());
  proxy.HandleH4HciFromHost(
      H4PacketWithH4(emboss::H4PacketType::ACL_DATA, acl_frame->h4_span()));

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
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  AclSnapshot snapshot;
  snapshot.snapshot_incomplete = true;
  EXPECT_EQ(proxy.RecoverAclFromSnapshot(snapshot), Status::DataLoss());
}

TEST_F(AclRecoveryTest, RegisterStateUpdateCallback) {
  Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/2,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  PW_TEST_ASSERT_OK(SendLeReadBufferResponseFromController(proxy, 10));

  struct {
    uint32_t updates_sent = 0;
    AclStateUpdate last_update;
  } update_capture;
  proxy.RegisterAclStateUpdateCallback(
      [&update_capture](const AclStateUpdate& update) {
        update_capture.updates_sent++;
        update_capture.last_update = update;
      });

  // Verify that connection creation triggers a state update callback.
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kLeConnectionHandle1, emboss::StatusCode::SUCCESS));
  EXPECT_EQ(update_capture.updates_sent, 1u);
  AclConnectionSnapshot connection_snapshot =
      update_capture.last_update.connection;
  EXPECT_EQ(connection_snapshot.connection_handle, kLeConnectionHandle1);
  EXPECT_EQ(connection_snapshot.num_proxy_pending_packets, 0);
  EXPECT_EQ(connection_snapshot.num_host_pending_packets, 0);

  // Verify that sending a packet from the host triggers a state update
  // callback.
  Result<AclFrameWithStorage> acl_frame = SetupAcl(kLeConnectionHandle1, 10);
  ASSERT_TRUE(acl_frame.ok());
  H4PacketWithH4 h4_packet(emboss::H4PacketType::ACL_DATA,
                           acl_frame->h4_span());
  proxy.HandleH4HciFromHost(std::move(h4_packet));
  EXPECT_EQ(update_capture.updates_sent, 2u);
  connection_snapshot = update_capture.last_update.connection;
  EXPECT_EQ(connection_snapshot.connection_handle, kLeConnectionHandle1);
  EXPECT_EQ(connection_snapshot.num_host_pending_packets, 1);

  // Verify that reclaiming credits via NOCP event triggers a state update
  // callback.
  PW_TEST_ASSERT_OK(
      SendNumberOfCompletedPackets(proxy, {{kLeConnectionHandle1, 1}}));
  EXPECT_EQ(update_capture.updates_sent, 3u);
  connection_snapshot = update_capture.last_update.connection;
  EXPECT_EQ(connection_snapshot.connection_handle, kLeConnectionHandle1);
  EXPECT_EQ(connection_snapshot.num_host_pending_packets, 0);
}

TEST_F(AclRecoveryTest, CreditResynchronizationDefersAndSends) {
  constexpr uint16_t kQueuedPackets1 = 3;
  constexpr uint16_t kQueuedPackets2 = 5;

  struct {
    std::optional<H4PacketWithHci> captured_packet;
    Allocator* allocator_ptr;
  } send_capture;
  send_capture.allocator_ptr = GetProxyHostAllocator();
  Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [&send_capture](H4PacketWithHci&& packet) {
        send_capture.captured_packet =
            H4PacketWithHci::CopyFrom(*send_capture.allocator_ptr, packet)
                .value();
      });

  Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/2,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  // Create a snapshot where connections had queued packets that were lost.
  AclSnapshot snapshot = CreateAclSnapshot();
  snapshot.acl_connections.push_back(
      CreateAclConnectionSnapshot(kLeConnectionHandle1,
                                  AclTransportType::kLe,
                                  /*num_proxy_pending=*/0,
                                  /*num_host_pending=*/0,
                                  kQueuedPackets1));
  snapshot.acl_connections.push_back(
      CreateAclConnectionSnapshot(kLeConnectionHandle2,
                                  AclTransportType::kLe,
                                  /*num_proxy_pending=*/0,
                                  /*num_host_pending=*/0,
                                  kQueuedPackets2));

  PW_TEST_ASSERT_OK(proxy.RecoverAclFromSnapshot(snapshot));

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
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  // Connection snapshot has 5 host pending packets (which would exceed
  // proxy_max = 2 if treated as proxy pending) and 0 proxy pending packets.
  AclSnapshot snapshot = CreateAclSnapshot(/*le_max=*/10, /*br_edr_max=*/0);
  snapshot.acl_connections.push_back(
      CreateAclConnectionSnapshot(kLeConnectionHandle1,
                                  AclTransportType::kLe,
                                  /*num_proxy_pending=*/0,
                                  /*num_host_pending=*/5,
                                  /*num_queued_host=*/0));

  PW_TEST_ASSERT_OK(proxy.RecoverAclFromSnapshot(snapshot));

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
                              *GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  // In dynamic credit sharing, both proxy and host in-flight packets are
  // counted towards total pending credits against controller max capacity.
  AclSnapshot snapshot = CreateAclSnapshot(/*le_max=*/10, /*br_edr_max=*/0);
  snapshot.acl_connections.push_back(
      CreateAclConnectionSnapshot(kLeConnectionHandle1,
                                  AclTransportType::kLe,
                                  /*num_proxy_pending=*/2,
                                  /*num_host_pending=*/3,
                                  /*num_queued_host=*/0));

  PW_TEST_ASSERT_OK(proxy.RecoverAclFromSnapshot(snapshot));

  // Controller max is 10, total pending is 2 + 3 = 5.
  // Verify that remaining credits is 10 - 5 = 5.
  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 5);
}

}  // namespace
}  // namespace pw::bluetooth::proxy

#endif  // PW_BLUETOOTH_PROXY_CONFIG_ENABLE_RECOVERY
