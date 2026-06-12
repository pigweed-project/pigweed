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

#include "pw_bluetooth_sapphire/internal/host/hci/connection.h"

#include <pw_assert/check.h>

#include "pw_bluetooth/hci_common.emb.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"
#include "pw_bluetooth_sapphire/internal/host/hci/bredr_connection.h"
#include "pw_bluetooth_sapphire/internal/host/hci/low_energy_connection.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/l2cap_defs.h"
#include "pw_bluetooth_sapphire/internal/host/testing/controller_test.h"
#include "pw_bluetooth_sapphire/internal/host/testing/mock_controller.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_packets.h"
#include "pw_bluetooth_sapphire/internal/host/transport/fake_acl_connection.h"

namespace bt::hci {
namespace {

const hci_spec::LEConnectionParameters kTestParams(1, 1, 1);
const DeviceAddress kLEAddress1(DeviceAddress::Type::kLEPublic, {1});
const DeviceAddress kLEAddress2(DeviceAddress::Type::kLEPublic, {2});
const DeviceAddress kACLAddress1(DeviceAddress::Type::kBREDR, {3});
const DeviceAddress kACLAddress2(DeviceAddress::Type::kBREDR, {4});

constexpr UInt128 kLTK{{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}};
constexpr uint64_t kRand = 1;
constexpr uint16_t kEDiv = 255;
constexpr hci_spec::LinkKeyType kLinkKeyType =
    hci_spec::LinkKeyType::kAuthenticatedCombination256;

const DataBufferInfo kBrEdrBufferInfo(1024, 5);
const DataBufferInfo kLeBufferInfo(1024, 1);

using bt::testing::CommandTransaction;

using TestingBase =
    bt::testing::FakeDispatcherControllerTest<bt::testing::MockController>;

const StaticByteBuffer kReadEncryptionKeySizeCommand =
    StaticByteBuffer(0x08,
                     0x14,  // opcode: HCI_ReadEncryptionKeySize
                     0x02,  // parameter size
                     0x01,
                     0x00  // connection handle: 0x0001
    );

// HCI_Disconnect (handle: 0x0001, reason: RemoteUserTerminatedConnection)
const StaticByteBuffer kDisconnectCommand(
    0x06,
    0x04,  // opcode: HCI_Disconnect
    0x03,  // parameter total size
    0x01,
    0x00,  // handle: 1
    pw::bluetooth::emboss::StatusCode::REMOTE_USER_TERMINATED_CONNECTION);

// HCI_Disconnect (handle: 0x0001, reason: RemoteUserTerminatedConnection)
const StaticByteBuffer kDisconnectCommandAuthFailure(
    0x06,
    0x04,  // opcode: HCI_Disconnect
    0x03,  // parameter total size
    0x01,
    0x00,  // handle: 1
    pw::bluetooth::emboss::StatusCode::AUTHENTICATION_FAILURE);

class ConnectionTest : public TestingBase {
 public:
  ConnectionTest() = default;
  ~ConnectionTest() override = default;

 protected:
  void SetUp() override {
    TestingBase::SetUp();
    InitializeACLDataChannel(kBrEdrBufferInfo, kLeBufferInfo);
  }

  std::unique_ptr<LowEnergyConnection> NewLEConnection(
      pw::bluetooth::emboss::ConnectionRole role =
          pw::bluetooth::emboss::ConnectionRole::CENTRAL,
      hci_spec::ConnectionHandle handle = kTestHandle) {
    return std::make_unique<LowEnergyConnection>(handle,
                                                 kLEAddress1,
                                                 kLEAddress2,
                                                 kTestParams,
                                                 role,
                                                 transport()->GetWeakPtr(),
                                                 dispatcher());
  }

  std::unique_ptr<BrEdrConnection> NewACLConnection(
      pw::bluetooth::emboss::ConnectionRole role =
          pw::bluetooth::emboss::ConnectionRole::CENTRAL,
      hci_spec::ConnectionHandle handle = kTestHandle) {
    return std::make_unique<BrEdrConnection>(handle,
                                             kACLAddress1,
                                             kACLAddress2,
                                             role,
                                             transport()->GetWeakPtr(),
                                             dispatcher());
  }
};

// Tests using this harness will be instantiated using ACL and LE link types.
// See INSTANTIATE_TEST_SUITE_P(ConnectionTest, LinkTypeConnectionTest, ...)
class LinkTypeConnectionTest
    : public ConnectionTest,
      public ::testing::WithParamInterface<bt::LinkType> {
 protected:
  std::unique_ptr<AclConnection> NewConnection(
      pw::bluetooth::emboss::ConnectionRole role =
          pw::bluetooth::emboss::ConnectionRole::CENTRAL,
      hci_spec::ConnectionHandle handle = kTestHandle) {
    const bt::LinkType ll_type = GetParam();
    switch (ll_type) {
      case bt::LinkType::kACL:
        return NewACLConnection(role, handle);
      case bt::LinkType::kLE:
        return NewLEConnection(role, handle);
      case bt::LinkType::kSCO:
      case bt::LinkType::kESCO:
        break;
    }
    PW_CRASH("Invalid link type: %u", static_cast<unsigned>(ll_type));
    return nullptr;
  }

  // Assigns the appropriate test link key based on the type of link being
  // tested.
  void SetTestLinkKey(Connection* connection) {
    const bt::LinkType ll_type = GetParam();
    if (ll_type == bt::LinkType::kLE) {
      static_cast<LowEnergyConnection*>(connection)
          ->set_ltk(hci_spec::LinkKey(kLTK, kRand, kEDiv));
    } else {
      static_cast<BrEdrConnection*>(connection)
          ->set_link_key(hci_spec::LinkKey(kLTK, 0, 0), kLinkKeyType);
    }
  }
};

using HCI_ConnectionTest = ConnectionTest;

TEST_F(ConnectionTest, Getters) {
  std::unique_ptr<LowEnergyConnection> connection = NewLEConnection();

  EXPECT_EQ(kTestHandle, connection->handle());
  EXPECT_EQ(pw::bluetooth::emboss::ConnectionRole::CENTRAL, connection->role());
  EXPECT_EQ(kTestParams, connection->low_energy_parameters());
  EXPECT_EQ(kLEAddress1, connection->local_address());
  EXPECT_EQ(kLEAddress2, connection->peer_address());

  EXPECT_EQ(std::nullopt, connection->ltk());
  connection->set_ltk(hci_spec::LinkKey());
  ASSERT_TRUE(connection->ltk().has_value());
  EXPECT_EQ(hci_spec::LinkKey(), connection->ltk().value());

  EXPECT_CMD_PACKET_OUT(test_device(),
                        bt::testing::DisconnectPacket(kTestHandle));
}

TEST_F(ConnectionTest, AclLinkKeyAndTypeAccessors) {
  std::unique_ptr<BrEdrConnection> connection = NewACLConnection();

  EXPECT_EQ(std::nullopt, connection->ltk());
  EXPECT_EQ(std::nullopt, connection->ltk_type());
  connection->set_link_key(hci_spec::LinkKey(), kLinkKeyType);
  ASSERT_TRUE(connection->ltk().has_value());
  EXPECT_EQ(hci_spec::LinkKey(), connection->ltk().value());
  ASSERT_TRUE(connection->ltk_type().has_value());
  EXPECT_EQ(kLinkKeyType, connection->ltk_type().value());

  EXPECT_CMD_PACKET_OUT(test_device(),
                        bt::testing::DisconnectPacket(kTestHandle));
}

TEST_P(LinkTypeConnectionTest, Disconnect) {
  // clang-format off

  // Respond with Command Status and Disconnection Complete.
  StaticByteBuffer cmd_status_bytes(
      hci_spec::kCommandStatusEventCode, 0x04, pw::bluetooth::emboss::StatusCode::SUCCESS, 1,
      0x06, 0x04);

  StaticByteBuffer disc_cmpl_bytes(
      hci_spec::kDisconnectionCompleteEventCode, 0x04,
      pw::bluetooth::emboss::StatusCode::SUCCESS, 0x01, 0x00,
      pw::bluetooth::emboss::StatusCode::CONNECTION_TERMINATED_BY_LOCAL_HOST);

  // clang-format on

  EXPECT_CMD_PACKET_OUT(
      test_device(), kDisconnectCommand, &cmd_status_bytes, &disc_cmpl_bytes);

  bool callback_called = false;
  test_device()->SetTransactionCallback(
      [&callback_called] { callback_called = true; });

  auto connection = NewConnection();

  size_t disconn_cb_count = 0;
  auto disconn_complete_cb = [&](const Connection&, auto reason) {
    disconn_cb_count++;
    EXPECT_EQ(
        reason,
        pw::bluetooth::emboss::StatusCode::CONNECTION_TERMINATED_BY_LOCAL_HOST);
  };
  connection->set_peer_disconnect_callback(disconn_complete_cb);

  connection->Disconnect(
      pw::bluetooth::emboss::StatusCode::REMOTE_USER_TERMINATED_CONNECTION);

  RunUntilIdle();
  EXPECT_TRUE(callback_called);
  EXPECT_EQ(1u, disconn_cb_count);
}

TEST_P(LinkTypeConnectionTest, LinkRegistrationAndLocalDisconnection) {
  const bt::LinkType ll_type = GetParam();
  const hci_spec::ConnectionHandle kHandle0 = 0x0001;
  const hci_spec::ConnectionHandle kHandle1 = 0x0002;

  const auto& kBufferInfo =
      ll_type == bt::LinkType::kACL ? kBrEdrBufferInfo : kLeBufferInfo;

  // Should register connection with ACL Data Channel.
  FakeAclConnection acl_connection_0(acl_data_channel(), kHandle0, ll_type);
  FakeAclConnection acl_connection_1(acl_data_channel(), kHandle1, ll_type);

  acl_data_channel()->RegisterConnection(acl_connection_0.GetWeakPtr());
  acl_data_channel()->RegisterConnection(acl_connection_1.GetWeakPtr());

  // HCI Connections corresponding to respective |acl_connection_*|
  auto hci_connection_0 =
      NewConnection(pw::bluetooth::emboss::ConnectionRole::CENTRAL, kHandle0);
  auto hci_connection_1 =
      NewConnection(pw::bluetooth::emboss::ConnectionRole::CENTRAL, kHandle1);

  // Fill up BR/EDR controller buffer
  for (size_t i = 0; i < kBufferInfo.max_num_packets(); i++) {
    // Connection handle should have been registered with ACL Data Channel.
    const StaticByteBuffer kPacket(
        // ACL data header (handle: 0, length 1)
        LowerBits(kHandle0),
        UpperBits(kHandle0),
        // payload length
        0x01,
        0x00,
        // payload
        static_cast<uint8_t>(i));
    EXPECT_ACL_PACKET_OUT(test_device(), kPacket);
    // Create packet to send on |acl_connection_0|
    ACLDataPacketPtr packet =
        ACLDataPacket::New(kHandle0,
                           hci_spec::ACLPacketBoundaryFlag::kFirstNonFlushable,
                           hci_spec::ACLBroadcastFlag::kPointToPoint,
                           /*payload_size=*/1);
    packet->mutable_view()->mutable_payload_data()[0] = static_cast<uint8_t>(i);
    acl_connection_0.QueuePacket(std::move(packet));
    RunUntilIdle();
  }
  // Create packet to send on |acl_connection_1|
  ACLDataPacketPtr packet =
      ACLDataPacket::New(kHandle1,
                         hci_spec::ACLPacketBoundaryFlag::kFirstNonFlushable,
                         hci_spec::ACLBroadcastFlag::kPointToPoint,
                         /*payload_size=*/1);
  packet->mutable_view()->mutable_payload_data()[0] = static_cast<uint8_t>(1);
  acl_connection_1.QueuePacket(std::move(packet));
  RunUntilIdle();

  // Packet for |acl_connection_1| should not have been sent because controller
  // buffer is full
  EXPECT_EQ(acl_connection_0.queued_packets().size(), 0u);
  EXPECT_EQ(acl_connection_1.queued_packets().size(), 1u);
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  const auto disconnect_status_rsp =
      bt::testing::DisconnectStatusResponsePacket();
  EXPECT_CMD_PACKET_OUT(test_device(),
                        bt::testing::DisconnectPacket(kHandle0),
                        &disconnect_status_rsp);
  hci_connection_0->Disconnect(
      pw::bluetooth::emboss::StatusCode::REMOTE_USER_TERMINATED_CONNECTION);
  RunUntilIdle();

  acl_data_channel()->UnregisterConnection(kHandle0);

  // Controller packet counts for |kHandle0| should not have been cleared after
  // disconnect. Disconnection Complete handler should clear controller packet
  // counts, so packet for |kHandle1| should be sent.
  DynamicByteBuffer disconnection_complete(
      bt::testing::DisconnectionCompletePacket(kHandle0));
  test_device()->SendCommandChannelPacket(disconnection_complete);

  // Send out last packet
  EXPECT_ACL_PACKET_OUT(test_device(),
                        StaticByteBuffer(
                            // ACL data header (handle: 0, length 1)
                            LowerBits(kHandle1),
                            UpperBits(kHandle1),
                            // payload length
                            0x01,
                            0x00,
                            // payload
                            1));
  RunUntilIdle();

  // Connection handle |kHandle0| should have been unregistered with ACL Data
  // Channel. Since controller packet count was cleared, packet for |kHandle1|
  // should have been sent.
  EXPECT_EQ(acl_connection_0.queued_packets().size(), 0u);
  EXPECT_EQ(acl_connection_1.queued_packets().size(), 0u);
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  // |acl_connection_1| is destroyed in test destructor
  EXPECT_CMD_PACKET_OUT(test_device(), bt::testing::DisconnectPacket(kHandle1));
}

// In remote disconnection, Connection::Disconnect is not called. Instead,
// Connection::OnDisconnectionComplete is invoked and handles all cleanup.
TEST_P(LinkTypeConnectionTest, LinkRegistrationAndRemoteDisconnection) {
  const bt::LinkType ll_type = GetParam();
  const hci_spec::ConnectionHandle kHandle0 = 0x0001;
  const hci_spec::ConnectionHandle kHandle1 = 0x0002;

  const auto& kBufferInfo =
      ll_type == bt::LinkType::kACL ? kBrEdrBufferInfo : kLeBufferInfo;

  // Should register connection with ACL Data Channel.
  FakeAclConnection acl_connection_0(acl_data_channel(), kHandle0, ll_type);
  FakeAclConnection acl_connection_1(acl_data_channel(), kHandle1, ll_type);

  acl_data_channel()->RegisterConnection(acl_connection_0.GetWeakPtr());
  acl_data_channel()->RegisterConnection(acl_connection_1.GetWeakPtr());

  // HCI Connections corresponding to respective |acl_connection_*|
  auto hci_connection_0 =
      NewConnection(pw::bluetooth::emboss::ConnectionRole::CENTRAL, kHandle0);
  auto hci_connection_1 =
      NewConnection(pw::bluetooth::emboss::ConnectionRole::CENTRAL, kHandle1);

  // Fill up BR/EDR controller buffer
  for (size_t i = 0; i < kBufferInfo.max_num_packets(); i++) {
    // Connection handle should have been registered with ACL Data Channel.
    const StaticByteBuffer kPacket(
        // ACL data header (handle: 0, length 1)
        LowerBits(kHandle0),
        UpperBits(kHandle0),
        // payload length
        0x01,
        0x00,
        // payload
        static_cast<uint8_t>(i));
    EXPECT_ACL_PACKET_OUT(test_device(), kPacket);
    // Create packet to send on |acl_connection_0|
    ACLDataPacketPtr packet =
        ACLDataPacket::New(kHandle0,
                           hci_spec::ACLPacketBoundaryFlag::kFirstNonFlushable,
                           hci_spec::ACLBroadcastFlag::kPointToPoint,
                           /*payload_size=*/1);
    packet->mutable_view()->mutable_payload_data()[0] = static_cast<uint8_t>(i);
    acl_connection_0.QueuePacket(std::move(packet));
    RunUntilIdle();
  }
  // Create packet to send on |acl_connection_1|
  ACLDataPacketPtr packet =
      ACLDataPacket::New(kHandle1,
                         hci_spec::ACLPacketBoundaryFlag::kFirstNonFlushable,
                         hci_spec::ACLBroadcastFlag::kPointToPoint,
                         /*payload_size=*/1);
  packet->mutable_view()->mutable_payload_data()[0] = static_cast<uint8_t>(1);
  acl_connection_1.QueuePacket(std::move(packet));
  RunUntilIdle();

  // Packet for |acl_connection_1| should not have been sent because controller
  // buffer is full
  EXPECT_EQ(acl_connection_0.queued_packets().size(), 0u);
  EXPECT_EQ(acl_connection_1.queued_packets().size(), 1u);
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  size_t disconn_cb_count = 0;
  auto disconn_complete_cb = [&](const Connection& cb_conn, auto /*reason*/) {
    EXPECT_EQ(kHandle0, cb_conn.handle());
    disconn_cb_count++;
  };
  hci_connection_0->set_peer_disconnect_callback(disconn_complete_cb);

  acl_data_channel()->UnregisterConnection(kHandle0);

  // Disconnection Complete handler should clear controller packet counts, so
  // packet for |kHandle1| should be sent.
  DynamicByteBuffer disconnection_complete(
      bt::testing::DisconnectionCompletePacket(kHandle0));
  test_device()->SendCommandChannelPacket(disconnection_complete);

  // Send out last packet
  EXPECT_ACL_PACKET_OUT(test_device(),
                        StaticByteBuffer(
                            // ACL data header (handle: 0, length 1)
                            LowerBits(kHandle1),
                            UpperBits(kHandle1),
                            // payload length
                            0x01,
                            0x00,
                            // payload
                            1));
  test_device()->SendCommandChannelPacket(
      bt::testing::NumberOfCompletedPacketsPacket(kHandle0, 10));
  RunUntilIdle();

  // Connection handle |kHandle0| should have been unregistered with ACL Data
  // Channel. Since controller packet count was cleared, packet for |kHandle1|
  // should have been sent.
  EXPECT_EQ(acl_connection_0.queued_packets().size(), 0u);
  EXPECT_EQ(acl_connection_1.queued_packets().size(), 0u);
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  // |acl_connection_1| is destroyed in test destructor
  EXPECT_CMD_PACKET_OUT(test_device(), bt::testing::DisconnectPacket(kHandle1));
}
// This test verifies that in a multi-connection scenario, the disconnection
// complete callback for a connection is not prematurely destroyed when a
// different connection disconnects.
//
// The bug was that the global event handler for Disconnection Complete captured
// the callback by value and moved it into the handler function. If the event
// was for a different connection, the handler returned early, destroying the
// callback and leaving the connection with a null callback. When that
// connection eventually disconnected, its callback (which clears controller
// packet counts) was not run, leaking controller buffer space.
//
// We verify this by registering 3 connections, triggering a disconnect on Conn
// 0 (which would destroy Conn 1 & 2's callbacks if the bug is present), and
// then verifying that Conn 1's subsequent disconnect still successfully clears
// its packet count and allows Conn 2's blocked packets to be sent.
TEST_P(LinkTypeConnectionTest,
       MultiConnectionDisconnectionCallbackDestruction) {
  const bt::LinkType ll_type = GetParam();
  const hci_spec::ConnectionHandle kHandle0 = 0x0001;
  const hci_spec::ConnectionHandle kHandle1 = 0x0002;
  const hci_spec::ConnectionHandle kHandle2 = 0x0003;

  const auto& kBufferInfo =
      ll_type == bt::LinkType::kACL ? kBrEdrBufferInfo : kLeBufferInfo;

  // 1. Register three connections (Conn 0, Conn 1, Conn 2).
  FakeAclConnection acl_connection_0(acl_data_channel(), kHandle0, ll_type);
  FakeAclConnection acl_connection_1(acl_data_channel(), kHandle1, ll_type);
  FakeAclConnection acl_connection_2(acl_data_channel(), kHandle2, ll_type);

  acl_data_channel()->RegisterConnection(acl_connection_0.GetWeakPtr());
  acl_data_channel()->RegisterConnection(acl_connection_1.GetWeakPtr());
  acl_data_channel()->RegisterConnection(acl_connection_2.GetWeakPtr());

  auto hci_connection_0 =
      NewConnection(pw::bluetooth::emboss::ConnectionRole::CENTRAL, kHandle0);
  auto hci_connection_1 =
      NewConnection(pw::bluetooth::emboss::ConnectionRole::CENTRAL, kHandle1);
  auto hci_connection_2 =
      NewConnection(pw::bluetooth::emboss::ConnectionRole::CENTRAL, kHandle2);

  // 2. Fill up controller buffer using Conn 0
  for (size_t i = 0; i < kBufferInfo.max_num_packets(); i++) {
    const StaticByteBuffer kPacket(LowerBits(kHandle0),
                                   UpperBits(kHandle0),
                                   0x01,
                                   0x00,
                                   static_cast<uint8_t>(i));
    EXPECT_ACL_PACKET_OUT(test_device(), kPacket);

    ACLDataPacketPtr packet =
        ACLDataPacket::New(kHandle0,
                           hci_spec::ACLPacketBoundaryFlag::kFirstNonFlushable,
                           hci_spec::ACLBroadcastFlag::kPointToPoint,
                           /*payload_size=*/1);
    packet->mutable_view()->mutable_payload_data()[0] = static_cast<uint8_t>(i);
    acl_connection_0.QueuePacket(std::move(packet));
    RunUntilIdle();
  }

  // 3. Queue a packet on Conn 1 (which gets blocked).
  ACLDataPacketPtr packet_1 =
      ACLDataPacket::New(kHandle1,
                         hci_spec::ACLPacketBoundaryFlag::kFirstNonFlushable,
                         hci_spec::ACLBroadcastFlag::kPointToPoint,
                         /*payload_size=*/1);
  packet_1->mutable_view()->mutable_payload_data()[0] = 0xff;
  acl_connection_1.QueuePacket(std::move(packet_1));
  RunUntilIdle();

  // Packet for |acl_connection_1| should not have been sent because controller
  // buffer is full
  EXPECT_EQ(acl_connection_0.queued_packets().size(), 0u);
  EXPECT_EQ(acl_connection_1.queued_packets().size(), 1u);
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  // Unregister Conn 0.
  acl_data_channel()->UnregisterConnection(kHandle0);

  // 4. Remotely disconnect Conn 0.
  // Disconnection Complete handler should clear controller packet counts, so
  // packet for |kHandle1| should be sent.
  // This also triggers the bug: Conn 1 and Conn 2's disconnection callbacks
  // are destroyed.
  DynamicByteBuffer disconnection_complete_0(
      bt::testing::DisconnectionCompletePacket(kHandle0));

  // We expect Conn 1's packet to be sent now.
  EXPECT_ACL_PACKET_OUT(
      test_device(),
      StaticByteBuffer(
          LowerBits(kHandle1), UpperBits(kHandle1), 0x01, 0x00, 0xff));

  test_device()->SendCommandChannelPacket(disconnection_complete_0);
  RunUntilIdle();

  // 5. Verifies Conn 1's packet is sent (buffer freed).
  EXPECT_EQ(acl_connection_1.queued_packets().size(), 0u);
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  // 6. Queues packets on Conn 2 until blocked (1 packet remains queued).
  // Buffer has 1 packet (Conn 1's packet). Fill it up.
  size_t remaining_space = kBufferInfo.max_num_packets() - 1;
  for (size_t i = 0; i < remaining_space; i++) {
    const StaticByteBuffer kPacket(LowerBits(kHandle1),
                                   UpperBits(kHandle1),
                                   0x01,
                                   0x00,
                                   static_cast<uint8_t>(i));
    EXPECT_ACL_PACKET_OUT(test_device(), kPacket);

    ACLDataPacketPtr p =
        ACLDataPacket::New(kHandle1,
                           hci_spec::ACLPacketBoundaryFlag::kFirstNonFlushable,
                           hci_spec::ACLBroadcastFlag::kPointToPoint,
                           /*payload_size=*/1);
    p->mutable_view()->mutable_payload_data()[0] = static_cast<uint8_t>(i);
    acl_connection_1.QueuePacket(std::move(p));
    RunUntilIdle();
  }

  // Queue 1 packet on Conn 2 (should be blocked).
  ACLDataPacketPtr packet_2 =
      ACLDataPacket::New(kHandle2,
                         hci_spec::ACLPacketBoundaryFlag::kFirstNonFlushable,
                         hci_spec::ACLBroadcastFlag::kPointToPoint,
                         /*payload_size=*/1);
  packet_2->mutable_view()->mutable_payload_data()[0] = 0xee;
  acl_connection_2.QueuePacket(std::move(packet_2));
  RunUntilIdle();

  EXPECT_EQ(acl_connection_2.queued_packets().size(), 1u);
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  // Unregister Conn 1.
  acl_data_channel()->UnregisterConnection(kHandle1);

  // 7. Remotely disconnect Conn 1.
  // We EXPECT Conn 2's packet to be sent.
  EXPECT_ACL_PACKET_OUT(
      test_device(),
      StaticByteBuffer(
          LowerBits(kHandle2), UpperBits(kHandle2), 0x01, 0x00, 0xee));

  DynamicByteBuffer disconnection_complete_1(
      bt::testing::DisconnectionCompletePacket(kHandle1));
  test_device()->SendCommandChannelPacket(disconnection_complete_1);
  RunUntilIdle();

  // 8. Verification:
  // If bug is fixed, Conn 2's packet is sent (queue size 0).
  EXPECT_EQ(acl_connection_2.queued_packets().size(), 0u);
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  // Cleanup Conn 2.
  acl_data_channel()->UnregisterConnection(kHandle2);
  EXPECT_CMD_PACKET_OUT(test_device(), bt::testing::DisconnectPacket(kHandle2));
}

TEST_F(ConnectionTest, StartEncryptionFailsAsLowEnergyPeripheral) {
  auto conn =
      NewLEConnection(pw::bluetooth::emboss::ConnectionRole::PERIPHERAL);
  conn->set_ltk(hci_spec::LinkKey());
  EXPECT_FALSE(conn->StartEncryption());
  EXPECT_CMD_PACKET_OUT(test_device(),
                        bt::testing::DisconnectPacket(kTestHandle));
}

TEST_F(ConnectionTest, StartEncryptionSucceedsAsLowEnergyCentral) {
  auto conn = NewLEConnection(pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  auto ltk = hci_spec::LinkKey();
  conn->set_ltk(ltk);
  EXPECT_TRUE(conn->StartEncryption());
  EXPECT_CMD_PACKET_OUT(test_device(),
                        bt::testing::LEStartEncryptionPacket(
                            kTestHandle, ltk.rand(), ltk.ediv(), ltk.value()));
}

TEST_F(ConnectionTest, StartEncryptionSucceedsWithBrEdrLinkKeyType) {
  auto conn = NewACLConnection();
  conn->set_link_key(hci_spec::LinkKey(), kLinkKeyType);
  EXPECT_TRUE(conn->StartEncryption());
  EXPECT_CMD_PACKET_OUT(
      test_device(),
      bt::testing::SetConnectionEncryption(kTestHandle, /*enable=*/true));
}

TEST_P(LinkTypeConnectionTest, DisconnectError) {
  // clang-format off

  // Respond with Command Status and Disconnection Complete.
  StaticByteBuffer cmd_status_bytes(
      hci_spec::kCommandStatusEventCode, 0x04, pw::bluetooth::emboss::StatusCode::SUCCESS, 1,
      0x06, 0x04);

  StaticByteBuffer disc_cmpl_bytes(
      hci_spec::kDisconnectionCompleteEventCode, 0x04,
      pw::bluetooth::emboss::StatusCode::COMMAND_DISALLOWED, 0x01, 0x00,
      pw::bluetooth::emboss::StatusCode::CONNECTION_TERMINATED_BY_LOCAL_HOST);

  // clang-format on

  EXPECT_CMD_PACKET_OUT(
      test_device(), kDisconnectCommand, &cmd_status_bytes, &disc_cmpl_bytes);

  // The callback should get called regardless of the procedure status.
  bool callback_called = false;
  test_device()->SetTransactionCallback(
      [&callback_called] { callback_called = true; });

  auto connection = NewConnection();

  connection->Disconnect(
      pw::bluetooth::emboss::StatusCode::REMOTE_USER_TERMINATED_CONNECTION);

  RunUntilIdle();
  EXPECT_TRUE(callback_called);
}

TEST_P(LinkTypeConnectionTest, StartEncryptionNoLinkKey) {
  auto conn = NewConnection();
  EXPECT_FALSE(conn->StartEncryption());
  EXPECT_CMD_PACKET_OUT(test_device(),
                        bt::testing::DisconnectPacket(kTestHandle));
}

// HCI Command Status event is received with an error status.
TEST_F(ConnectionTest, LEStartEncryptionFailsAtStatus) {
  // clang-format off
  StaticByteBuffer kExpectedCommand(
    0x19, 0x20,  // HCI_LE_Start_Encryption
    28,          // parameter total size
    0x01, 0x00,  // connection handle: 1
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // rand: 1
    0xFF, 0x00,  // ediv: 255
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16  // LTK
  );
  StaticByteBuffer kErrorStatus(
    0x0F,       // HCI Command Status event code
    4,          // parameter total size
    0x0C,       // "Command Disallowed" error
    1,          // num_hci_command_packets
    0x19, 0x20  // opcode: HCI_LE_Start_Encryption
  );
  // clang-format on

  EXPECT_CMD_PACKET_OUT(test_device(), kExpectedCommand, &kErrorStatus);

  bool callback = false;
  auto conn = NewLEConnection();
  conn->set_ltk(hci_spec::LinkKey(kLTK, kRand, kEDiv));
  conn->set_encryption_change_callback([&](Result<bool> result) {
    ASSERT_TRUE(result.is_error());
    EXPECT_TRUE(result.error_value().is(
        pw::bluetooth::emboss::StatusCode::COMMAND_DISALLOWED));
    callback = true;
  });

  EXPECT_TRUE(conn->StartEncryption());

  RunUntilIdle();
  EXPECT_TRUE(callback);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        bt::testing::DisconnectPacket(kTestHandle));
}

TEST_F(ConnectionTest, LEStartEncryptionSendsSetLeConnectionEncryptionCommand) {
  StaticByteBuffer kExpectedCommand(0x19,
                                    0x20,  // HCI_LE_Start_Encryption
                                    28,    // parameter total size
                                    0x01,
                                    0x00,  // connection handle: 1
                                    0x01,
                                    0x00,
                                    0x00,
                                    0x00,
                                    0x00,
                                    0x00,
                                    0x00,
                                    0x00,  // rand: 1
                                    0xFF,
                                    0x00,  // ediv: 255
                                    1,
                                    2,
                                    3,
                                    4,
                                    5,
                                    6,
                                    7,
                                    8,
                                    9,
                                    10,
                                    11,
                                    12,
                                    13,
                                    14,
                                    15,
                                    16  // LTK
  );
  StaticByteBuffer kStatus(0x0F,  // HCI Command Status event code
                           4,     // parameter total size
                           0x00,  // success status
                           1,     // num_hci_command_packets
                           0x19,
                           0x20  // opcode: HCI_LE_Start_Encryption
  );

  EXPECT_CMD_PACKET_OUT(test_device(), kExpectedCommand, &kStatus);

  bool callback = false;
  auto conn = NewLEConnection();
  conn->set_ltk(hci_spec::LinkKey(kLTK, kRand, kEDiv));
  conn->set_encryption_change_callback([&](auto) { callback = true; });

  EXPECT_TRUE(conn->StartEncryption());

  // Callback shouldn't be called until the controller sends an encryption
  // changed event.
  RunUntilIdle();
  EXPECT_FALSE(callback);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        bt::testing::DisconnectPacket(kTestHandle));
}

// HCI Command Status event is received with an error status.
TEST_F(ConnectionTest, AclStartEncryptionFailsAtStatus) {
  StaticByteBuffer kExpectedCommand(0x13,
                                    0x04,  // HCI_Set_Connection_Encryption
                                    3,     // parameter total size
                                    0x01,
                                    0x00,  // connection handle
                                    0x01   // encryption enable
  );
  StaticByteBuffer kErrorStatus(0x0F,  // HCI Command Status event code
                                4,     // parameter total size
                                0x0C,  // "Command Disallowed" error
                                1,     // num_hci_command_packets
                                0x13,
                                0x04  // opcode: HCI_Set_Connection_Encryption
  );

  EXPECT_CMD_PACKET_OUT(test_device(), kExpectedCommand, &kErrorStatus);

  bool callback = false;
  auto conn = NewACLConnection();
  conn->set_link_key(hci_spec::LinkKey(kLTK, 0, 0), kLinkKeyType);
  conn->set_encryption_change_callback([&](Result<bool> result) {
    ASSERT_TRUE(result.is_error());
    EXPECT_TRUE(result.error_value().is(
        pw::bluetooth::emboss::StatusCode::COMMAND_DISALLOWED));
    callback = true;
  });

  EXPECT_TRUE(conn->StartEncryption());

  RunUntilIdle();
  EXPECT_TRUE(callback);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        bt::testing::DisconnectPacket(kTestHandle));
}

TEST_F(ConnectionTest, AclStartEncryptionSendsSetConnectionEncryptionCommand) {
  StaticByteBuffer kExpectedCommand(0x13,
                                    0x04,  // HCI_Set_Connection_Encryption
                                    3,     // parameter total size
                                    0x01,
                                    0x00,  // connection handle
                                    0x01   // encryption enable
  );
  StaticByteBuffer kStatus(0x0F,  // HCI Command Status event code
                           4,     // parameter total size
                           0x00,  // success status
                           1,     // num_hci_command_packets
                           0x13,
                           0x04  // opcode: HCI_Set_Connection_Encryption
  );

  EXPECT_CMD_PACKET_OUT(test_device(), kExpectedCommand, &kStatus);

  bool callback = false;
  auto conn = NewACLConnection();
  conn->set_link_key(hci_spec::LinkKey(kLTK, 0, 0), kLinkKeyType);
  conn->set_encryption_change_callback([&](auto) { callback = true; });

  EXPECT_TRUE(conn->StartEncryption());

  // Callback shouldn't be called until the controller sends an encryption
  // changed event.
  RunUntilIdle();
  EXPECT_FALSE(callback);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        bt::testing::DisconnectPacket(kTestHandle));
}

TEST_P(LinkTypeConnectionTest, EncryptionChangeIgnoredEvents) {
  // clang-format off
  StaticByteBuffer kEncChangeMalformed(
    0x08,       // HCI Encryption Change event code
    3,          // parameter total size
    0x00,       // status
    0x01, 0x00  // connection handle: 1
    // Last byte missing
  );
  StaticByteBuffer kEncChangeWrongHandle(
    0x08,        // HCI Encryption Change event code
    4,           // parameter total size
    0x00,        // status
    0x02, 0x00,  // connection handle: 2
    0x01         // encryption enabled
  );
  // clang-format on

  bool callback = false;
  auto conn = NewConnection();
  SetTestLinkKey(conn.get());
  conn->set_encryption_change_callback([&](auto) { callback = true; });

  test_device()->SendCommandChannelPacket(kEncChangeMalformed);
  test_device()->SendCommandChannelPacket(kEncChangeWrongHandle);

  RunUntilIdle();
  EXPECT_FALSE(callback);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        bt::testing::DisconnectPacket(kTestHandle));
}

TEST_P(LinkTypeConnectionTest, EncryptionChangeEvents) {
  // clang-format off
  StaticByteBuffer kEncryptionChangeEventDisabled(
    0x08,        // HCI Encryption Change event code
    4,           // parameter total size
    0x00,        // status
    0x01, 0x00,  // connection handle: 1
    0x00         // encryption disabled
  );
  StaticByteBuffer kEncryptionChangeEventFailed(
    0x08,        // HCI Encryption Change event code
    4,           // parameter total size
    0x06,        // status: Pin or Key missing
    0x01, 0x00,  // connection handle: 1
    0x00         // encryption disabled
  );

  StaticByteBuffer kKeySizeComplete(
    0x0E,        // event code: Command Complete
    0x07,        // parameters total size
    0xFF,        // num command packets allowed (255)
    0x08, 0x14,  // original opcode

    // return parameters
    0x00,        // status (success)
    0x01, 0x00,  // connection handle: 0x0001
    0x10         // encryption key size: 16
  );
  // clang-format on

  int callback_count = 0;
  auto conn = NewConnection();

  Result<bool> result = fit::error(Error(HostError::kFailed));
  conn->set_encryption_change_callback([&](Result<bool> cb_result) {
    callback_count++;
    result = cb_result;
  });

  if (GetParam() == bt::LinkType::kACL) {
    // The host tries to validate the size of key used to encrypt ACL links.
    EXPECT_CMD_PACKET_OUT(
        test_device(), kReadEncryptionKeySizeCommand, &kKeySizeComplete);
  }

  test_device()->SendCommandChannelPacket(
      bt::testing::EncryptionChangeEventPacket(
          pw::bluetooth::emboss::StatusCode::SUCCESS,
          kTestHandle,
          hci_spec::EncryptionStatus::kOn));
  RunUntilIdle();

  EXPECT_EQ(1, callback_count);
  EXPECT_EQ(fit::ok(), result);
  EXPECT_TRUE(result.value_or(false));

  test_device()->SendCommandChannelPacket(kEncryptionChangeEventDisabled);
  RunUntilIdle();

  EXPECT_EQ(2, callback_count);
  EXPECT_EQ(fit::ok(), result);
  EXPECT_FALSE(result.value_or(true));

  // The host should disconnect the link if encryption fails.
  EXPECT_CMD_PACKET_OUT(test_device(), kDisconnectCommandAuthFailure);
  test_device()->SendCommandChannelPacket(kEncryptionChangeEventFailed);
  RunUntilIdle();

  EXPECT_EQ(3, callback_count);
  EXPECT_EQ(ToResult(pw::bluetooth::emboss::StatusCode::PIN_OR_KEY_MISSING)
                .error_value(),
            result);
}

TEST_F(ConnectionTest, EncryptionFailureNotifiesPeerDisconnectCallback) {
  bool peer_disconnect_callback_received = false;
  auto conn = NewLEConnection();
  conn->set_peer_disconnect_callback([&](const auto& self, auto /*reason*/) {
    EXPECT_EQ(conn.get(), &self);
    peer_disconnect_callback_received = true;
  });

  // Send the encryption change failure. The host should disconnect the link as
  // a result.
  EXPECT_CMD_PACKET_OUT(test_device(), kDisconnectCommandAuthFailure);
  test_device()->SendCommandChannelPacket(
      bt::testing::EncryptionChangeEventPacket(
          pw::bluetooth::emboss::StatusCode::CONNECTION_TERMINATED_MIC_FAILURE,
          kTestHandle,
          hci_spec::EncryptionStatus::kOff));
  RunUntilIdle();
  EXPECT_FALSE(peer_disconnect_callback_received);

  // Send the disconnection complete resulting from the encryption failure (this
  // usually does not correspond to the Disconnect command sent by
  // hci::Connection, which will cause a later subsequent event).
  test_device()->SendCommandChannelPacket(
      bt::testing::DisconnectionCompletePacket(
          kTestHandle,
          pw::bluetooth::emboss::StatusCode::
              CONNECTION_TERMINATED_MIC_FAILURE));
  RunUntilIdle();
  EXPECT_TRUE(peer_disconnect_callback_received);
}

TEST_F(ConnectionTest, AclEncryptionEnableCanNotReadKeySizeClosesLink) {
  StaticByteBuffer kKeySizeComplete(0x0E,  // event code: Command Complete
                                    0x07,  // parameters total size
                                    0xFF,  // num command packets allowed (255)
                                    0x08,
                                    0x14,  // original opcode

                                    // return parameters
                                    0x2F,  // status (insufficient security)
                                    0x01,
                                    0x00,  // connection handle: 0x0001
                                    0x10   // encryption key size: 16
  );

  int callback_count = 0;
  auto conn = NewACLConnection();
  conn->set_encryption_change_callback([&callback_count](Result<bool> result) {
    callback_count++;
    EXPECT_TRUE(result.is_error());
  });

  EXPECT_CMD_PACKET_OUT(
      test_device(), kReadEncryptionKeySizeCommand, &kKeySizeComplete);
  EXPECT_CMD_PACKET_OUT(test_device(), kDisconnectCommandAuthFailure);
  test_device()->SendCommandChannelPacket(
      bt::testing::EncryptionChangeEventPacket(
          pw::bluetooth::emboss::StatusCode::SUCCESS,
          kTestHandle,
          hci_spec::EncryptionStatus::kOn));
  RunUntilIdle();

  EXPECT_EQ(1, callback_count);
}

TEST_F(ConnectionTest, AclEncryptionEnableKeySizeOneByteClosesLink) {
  StaticByteBuffer kKeySizeComplete(0x0E,  // event code: Command Complete
                                    0x07,  // parameters total size
                                    0xFF,  // num command packets allowed (255)
                                    0x08,
                                    0x14,  // original opcode

                                    // return parameters
                                    0x00,  // status (success)
                                    0x01,
                                    0x00,  // connection handle: 0x0001
                                    0x01   // encryption key size: 1
  );

  int callback_count = 0;
  auto conn = NewACLConnection();
  conn->set_encryption_change_callback([&callback_count](Result<bool> result) {
    callback_count++;
    EXPECT_TRUE(result.is_error());
  });

  EXPECT_CMD_PACKET_OUT(
      test_device(), kReadEncryptionKeySizeCommand, &kKeySizeComplete);
  EXPECT_CMD_PACKET_OUT(test_device(), kDisconnectCommandAuthFailure);
  test_device()->SendCommandChannelPacket(
      bt::testing::EncryptionChangeEventPacket(
          pw::bluetooth::emboss::StatusCode::SUCCESS,
          kTestHandle,
          hci_spec::EncryptionStatus::kOn));
  RunUntilIdle();

  EXPECT_EQ(1, callback_count);
}

TEST_F(ConnectionTest, SecureConnectionsSucceedsWithAESEncryptionAlgorithm) {
  StaticByteBuffer kKeySizeComplete(0x0E,  // event code: Command Complete
                                    0x07,  // parameters total size
                                    0xFF,  // num command packets allowed (255)
                                    0x08,
                                    0x14,  // original opcode

                                    // return parameters
                                    0x00,  // status (success)
                                    0x01,
                                    0x00,  // connection handle: 0x0001
                                    0x10   // encryption key size: 16
  );

  std::unique_ptr<BrEdrConnection> connection = NewACLConnection(
      pw::bluetooth::emboss::ConnectionRole::CENTRAL, kTestHandle);

  int callback_count = 0;
  Result<bool> result = fit::error(Error(HostError::kFailed));
  connection->set_encryption_change_callback([&](Result<bool> cb_result) {
    callback_count++;
    result = cb_result;
  });

  EXPECT_CMD_PACKET_OUT(
      test_device(), kReadEncryptionKeySizeCommand, &kKeySizeComplete);
  test_device()->SendCommandChannelPacket(
      bt::testing::EncryptionChangeEventPacket(
          pw::bluetooth::emboss::StatusCode::SUCCESS,
          kTestHandle,
          hci_spec::EncryptionStatus::kBredrSecureConnections));
  RunUntilIdle();

  EXPECT_EQ(1, callback_count);
  EXPECT_EQ(fit::ok(), result);
  EXPECT_TRUE(result.value_or(false));

  EXPECT_CMD_PACKET_OUT(test_device(), kDisconnectCommand);
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
}

TEST_F(ConnectionTest, EncryptionSecureConnectionsWrongAlgorithmClosesLink) {
  std::unique_ptr<BrEdrConnection> connection = NewACLConnection(
      pw::bluetooth::emboss::ConnectionRole::CENTRAL, kTestHandle);

  connection->set_use_secure_connections(true);

  int callback_count = 0;
  Result<bool> result = fit::error(Error(HostError::kFailed));
  connection->set_encryption_change_callback([&](Result<bool> cb_result) {
    ASSERT_TRUE(cb_result.is_error());
    EXPECT_TRUE(cb_result.error_value().is(HostError::kInsufficientSecurity));
    callback_count++;
    result = cb_result;
  });

  // The host should disconnect the link if encryption fails.
  EXPECT_CMD_PACKET_OUT(test_device(), kDisconnectCommandAuthFailure);
  test_device()->SendCommandChannelPacket(
      bt::testing::EncryptionChangeEventPacket(
          pw::bluetooth::emboss::StatusCode::SUCCESS,
          kTestHandle,
          hci_spec::EncryptionStatus::kOn));
  RunUntilIdle();

  EXPECT_EQ(1, callback_count);
  EXPECT_TRUE(result.is_error());

  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());
}

TEST_P(LinkTypeConnectionTest, EncryptionKeyRefreshEvents) {
  // clang-format off
  StaticByteBuffer kEncryptionKeyRefresh(
    0x30,       // HCI Encryption Key Refresh Complete event
    3,          // parameter total size
    0x00,       // status
    0x01, 0x00  // connection handle: 1
  );
  StaticByteBuffer kEncryptionKeyRefreshFailed(
    0x30,       // HCI Encryption Key Refresh Complete event
    3,          // parameter total size
    0x06,       // status: Pin or Key missing
    0x01, 0x00  // connection handle: 1
  );
  // clang-format on

  int callback_count = 0;
  auto conn = NewConnection();

  Result<bool> result = fit::error(Error(HostError::kFailed));
  conn->set_encryption_change_callback([&](Result<bool> cb_result) {
    callback_count++;
    result = cb_result;
  });

  test_device()->SendCommandChannelPacket(kEncryptionKeyRefresh);
  RunUntilIdle();

  EXPECT_EQ(1, callback_count);
  ASSERT_EQ(fit::ok(), result);
  EXPECT_TRUE(result.value());

  // The host should disconnect the link if encryption fails.
  EXPECT_CMD_PACKET_OUT(test_device(), kDisconnectCommandAuthFailure);
  test_device()->SendCommandChannelPacket(kEncryptionKeyRefreshFailed);
  RunUntilIdle();

  EXPECT_EQ(2, callback_count);
  EXPECT_EQ(ToResult(pw::bluetooth::emboss::StatusCode::PIN_OR_KEY_MISSING)
                .error_value(),
            result);
}

TEST_F(ConnectionTest, LELongTermKeyRequestIgnoredEvent) {
  // clang-format off
  StaticByteBuffer kMalformed(
    0x3E,        // LE Meta Event code
    12,          // parameter total size
    0x05,        // LE LTK Request subevent code
    0x01, 0x00,  // connection handle: 1

    // rand:
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    // ediv: (missing 1 byte)
    0x00
  );
  StaticByteBuffer kWrongHandle(
    0x3E,        // LE Meta Event code
    13,          // parameter total size
    0x05,        // LE LTK Request subevent code
    0x02, 0x00,  // connection handle: 2 (wrong)

    // rand: 0
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    // ediv: 0
    0x00, 0x00
  );
  // clang-format on

  auto conn = NewLEConnection();
  conn->set_ltk(hci_spec::LinkKey(kLTK, 0, 0));

  test_device()->SendCommandChannelPacket(kMalformed);
  test_device()->SendCommandChannelPacket(kWrongHandle);

  RunUntilIdle();

  // Test will fail if the connection sends a response without ignoring these
  // events.
  EXPECT_CMD_PACKET_OUT(test_device(),
                        bt::testing::DisconnectPacket(kTestHandle));
}

TEST_F(ConnectionTest, LELongTermKeyRequestNoKey) {
  // clang-format off
  StaticByteBuffer kEvent(
    0x3E,        // LE Meta Event code
    13,          // parameter total size
    0x05,        // LE LTK Request subevent code
    0x01, 0x00,  // connection handle: 2 (wrong)

    // rand: 0
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    // ediv: 0
    0x00, 0x00
  );
  StaticByteBuffer kResponse(
    0x1B, 0x20,  // opcode: HCI_LE_Long_Term_Key_Request_Negative_Reply
    2,           // parameter total size
    0x01, 0x00   // connection handle: 1
  );
  // clang-format on

  // The request should be rejected since there is no LTK.
  EXPECT_CMD_PACKET_OUT(test_device(), kResponse);
  auto conn = NewLEConnection();

  test_device()->SendCommandChannelPacket(kEvent);
  RunUntilIdle();
}

// There is a link key but EDiv and Rand values don't match.
TEST_F(ConnectionTest, LELongTermKeyRequestNoMatchinKey) {
  // clang-format off
  StaticByteBuffer kEvent(
    0x3E,        // LE Meta Event code
    13,          // parameter total size
    0x05,        // LE LTK Request subevent code
    0x01, 0x00,  // connection handle: 2 (wrong)

    // rand: 0
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    // ediv: 0
    0x00, 0x00
  );
  StaticByteBuffer kResponse(
    0x1B, 0x20,  // opcode: HCI_LE_Long_Term_Key_Request_Negative_Reply
    2,           // parameter total size
    0x01, 0x00   // connection handle: 1
  );
  // clang-format on

  // The request should be rejected since there is no LTK.
  EXPECT_CMD_PACKET_OUT(test_device(), kResponse);
  auto conn = NewLEConnection();
  conn->set_ltk(hci_spec::LinkKey(kLTK, 1, 1));

  test_device()->SendCommandChannelPacket(kEvent);
  RunUntilIdle();
}

TEST_F(ConnectionTest, LELongTermKeyRequestReply) {
  // clang-format off
  StaticByteBuffer kEvent(
    0x3E,        // LE Meta Event code
    13,          // parameter total size
    0x05,        // LE LTK Request subevent code
    0x01, 0x00,  // connection handle: 2 (wrong)

    // rand: 0x8899AABBCCDDEEFF
    0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0x99, 0x88,
    // ediv: 0xBEEF
    0xEF, 0xBE
  );
  StaticByteBuffer kResponse(
    0x1A, 0x20,  // opcode: HCI_LE_Long_Term_Key_Request_Reply
    18,          // parameter total size
    0x01, 0x00,  // connection handle: 1

    // LTK:
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16
  );
  // clang-format on

  // The request should be rejected since there is no LTK.
  EXPECT_CMD_PACKET_OUT(test_device(), kResponse);
  auto conn = NewLEConnection();
  conn->set_ltk(hci_spec::LinkKey(kLTK, 0x8899AABBCCDDEEFF, 0xBEEF));

  test_device()->SendCommandChannelPacket(kEvent);
  RunUntilIdle();
}

TEST_F(
    ConnectionTest,
    QueuedPacketsGetDroppedOnDisconnectionCompleteAndStalePacketsAreNotSentOnHandleReuse) {
  const hci_spec::ConnectionHandle kHandle = 0x0001;

  // Should register connection with ACL Data Channel.
  FakeAclConnection acl_connection_0(
      acl_data_channel(), kHandle, bt::LinkType::kACL);

  acl_data_channel()->RegisterConnection(acl_connection_0.GetWeakPtr());

  // HCI Connection corresponding to |acl_connection_0|
  auto hci_connection_0 =
      NewACLConnection(pw::bluetooth::emboss::ConnectionRole::CENTRAL, kHandle);

  // Fill up BR/EDR controller buffer then queue one additional packet
  for (size_t i = 0; i < kBrEdrBufferInfo.max_num_packets() + 1; i++) {
    // Last packet should remain queued
    if (i < kBrEdrBufferInfo.max_num_packets()) {
      const StaticByteBuffer kPacket(
          // ACL data header (handle: 0, length 1)
          LowerBits(kHandle),
          UpperBits(kHandle),
          // payload length
          0x01,
          0x00,
          // payload
          static_cast<uint8_t>(i));
      EXPECT_ACL_PACKET_OUT(test_device(), kPacket);
    }
    // Create packet to send
    ACLDataPacketPtr packet =
        ACLDataPacket::New(kHandle,
                           hci_spec::ACLPacketBoundaryFlag::kFirstNonFlushable,
                           hci_spec::ACLBroadcastFlag::kPointToPoint,
                           /*payload_size=*/1);
    packet->mutable_view()->mutable_payload_data()[0] = static_cast<uint8_t>(i);
    acl_connection_0.QueuePacket(std::move(packet));
    RunUntilIdle();
  }
  // Run until the data is flushed out to the MockController.
  RunUntilIdle();

  // Only packets that fit in buffer should have been received.
  EXPECT_EQ(acl_connection_0.queued_packets().size(), 1u);
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  acl_data_channel()->UnregisterConnection(kHandle);

  // All future packets received should be for the next connection.
  const auto disconnect_status_rsp =
      bt::testing::DisconnectStatusResponsePacket();
  DynamicByteBuffer disconnection_complete(
      bt::testing::DisconnectionCompletePacket(kHandle));
  EXPECT_CMD_PACKET_OUT(test_device(),
                        bt::testing::DisconnectPacket(kHandle),
                        &disconnect_status_rsp,
                        &disconnection_complete);

  // Disconnect |hci_connection_0| by destroying it. The received disconnection
  // complete event will cause the handler to clear pending packets.
  hci_connection_0.reset();
  RunUntilIdle();

  // Register connection with same handle.
  FakeAclConnection acl_connection_1(
      acl_data_channel(), kHandle, bt::LinkType::kACL);

  acl_data_channel()->RegisterConnection(acl_connection_1.GetWeakPtr());

  // HCI Connection corresponding to |acl_connection_1|
  auto hci_connection_1 =
      NewACLConnection(pw::bluetooth::emboss::ConnectionRole::CENTRAL, kHandle);

  // Fill up BR/EDR controller buffer then queue one additional packet
  for (size_t i = 0; i < kBrEdrBufferInfo.max_num_packets() + 1; i++) {
    // Last packet should remain queued
    if (i < kBrEdrBufferInfo.max_num_packets()) {
      const StaticByteBuffer kPacket(
          // ACL data header (handle: 0, length 1)
          LowerBits(kHandle),
          UpperBits(kHandle),
          // payload length
          0x01,
          0x00,
          // payload
          static_cast<uint8_t>(i));
      EXPECT_ACL_PACKET_OUT(test_device(), kPacket);
    }
    // Create packet to send
    ACLDataPacketPtr packet =
        ACLDataPacket::New(kHandle,
                           hci_spec::ACLPacketBoundaryFlag::kFirstNonFlushable,
                           hci_spec::ACLBroadcastFlag::kPointToPoint,
                           /*payload_size=*/1);
    packet->mutable_view()->mutable_payload_data()[0] = static_cast<uint8_t>(i);
    acl_connection_1.QueuePacket(std::move(packet));
    RunUntilIdle();
  }
  // Run until the data is flushed out to the MockController.
  RunUntilIdle();

  EXPECT_EQ(acl_connection_1.queued_packets().size(), 1u);
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  acl_data_channel()->UnregisterConnection(kHandle);

  // Disconnect |hci_connection_1| by destroying it. The received disconnection
  // complete event will cause the handler to clear pending packets.
  hci_connection_1.reset();
  EXPECT_CMD_PACKET_OUT(test_device(),
                        bt::testing::DisconnectPacket(kHandle),
                        &disconnect_status_rsp,
                        &disconnection_complete);
  RunUntilIdle();
}

TEST_F(ConnectionTest, PeerDisconnectCallback) {
  const hci_spec::ConnectionHandle kHandle = 0x0001;

  auto conn =
      NewACLConnection(pw::bluetooth::emboss::ConnectionRole::CENTRAL, kHandle);

  size_t cb_count = 0;
  auto disconn_complete_cb = [&](const Connection&, auto /*reason*/) {
    cb_count++;

    // Should be safe to destroy connection from this callback, as a connection
    // manager does.
    conn.reset();
  };
  conn->set_peer_disconnect_callback(disconn_complete_cb);

  RunUntilIdle();
  EXPECT_EQ(0u, cb_count);

  DynamicByteBuffer disconnection_complete(
      bt::testing::DisconnectionCompletePacket(kHandle));
  test_device()->SendCommandChannelPacket(disconnection_complete);
  RunUntilIdle();

  EXPECT_EQ(1u, cb_count);
}

// Test connection handling cases for all types of links.
INSTANTIATE_TEST_SUITE_P(ConnectionTest,
                         LinkTypeConnectionTest,
                         ::testing::Values(bt::LinkType::kACL,
                                           bt::LinkType::kLE));

}  // namespace
}  // namespace bt::hci
