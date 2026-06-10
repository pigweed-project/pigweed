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

#include "pw_bluetooth_sapphire/internal/host/transport/iso_data_channel.h"

#include <cstdint>
#include <memory>

#include "pw_bluetooth/hci_data.emb.h"
#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/testing/controller_test.h"
#include "pw_bluetooth_sapphire/internal/host/testing/mock_controller.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_packets.h"

namespace bt::hci {

constexpr size_t kDefaultMaxDataLength = 128;
constexpr size_t kDefaultMaxNumPackets = 4;
constexpr size_t kTestSduSize = 15;

const DataBufferInfo kDefaultIsoBufferInfo(kDefaultMaxDataLength,
                                           kDefaultMaxNumPackets);

DynamicByteBuffer MakeIsoPacket(hci_spec::ConnectionHandle handle,
                                uint16_t seq) {
  std::vector<uint8_t> sdu = testing::GenDataBlob(kTestSduSize, seq);
  return testing::IsoDataPacket(
      /*connection_handle = */ handle,
      /*pb_flag = */ pw::bluetooth::emboss::IsoDataPbFlag::COMPLETE_SDU,
      /*time_stamp = */ 0x00000000,
      /*packet_sequence_number = */ seq,
      /*iso_sdu_length = */ sdu.size(),
      /*status_flag = */
      pw::bluetooth::emboss::IsoDataPacketStatus::VALID_DATA,
      /*sdu_data = */ sdu);
}

using TestBase = testing::FakeDispatcherControllerTest<testing::MockController>;

class IsoDataChannelTests : public TestBase {
 public:
  void SetUp() override {
    TestBase::SetUp(pw::bluetooth::Controller::FeaturesBits::kHciIso);
    ASSERT_TRUE(transport()->InitializeIsoDataChannel(kDefaultIsoBufferInfo));
  }

  IsoDataChannel* iso_data_channel() { return transport()->iso_data_channel(); }
};

// Placeholder (for now)
class IsoMockConnectionInterface : public IsoDataChannel::ConnectionInterface {
 public:
  IsoMockConnectionInterface(
      IsoDataChannel& iso_data_channel,
      pw::bluetooth_sapphire::testing::FakeLeaseProvider& lease_provider)
      : lease_provider_(lease_provider),
        iso_data_channel_(iso_data_channel),
        weak_self_(this) {}
  ~IsoMockConnectionInterface() override = default;

  void SendData(DynamicByteBuffer pdu) {
    send_queue_.emplace(std::move(pdu));
    iso_data_channel_.TrySendPackets();
  }

  std::queue<pw::span<const std::byte>>* received_packets() {
    return &received_packets_;
  }

  using WeakPtr = WeakSelf<IsoMockConnectionInterface>::WeakPtr;
  IsoMockConnectionInterface::WeakPtr GetWeakPtr() {
    return weak_self_.GetWeakPtr();
  }

 private:
  void ReceiveInboundPacket(pw::span<const std::byte> packet) override {
    received_packets_.emplace(packet);
  }

  std::optional<DynamicByteBuffer> GetNextOutboundPdu() override {
    EXPECT_NE(lease_provider_.lease_count(), 0u);
    if (send_queue_.empty()) {
      return std::nullopt;
    }
    DynamicByteBuffer pdu = std::move(send_queue_.front());
    send_queue_.pop();
    return pdu;
  }
  pw::bluetooth_sapphire::testing::FakeLeaseProvider& lease_provider_;
  IsoDataChannel& iso_data_channel_;
  std::queue<pw::span<const std::byte>> received_packets_;
  std::queue<DynamicByteBuffer> send_queue_;
  WeakSelf<IsoMockConnectionInterface> weak_self_;
};

// Verify that we can register and unregister connections
TEST_F(IsoDataChannelTests, RegisterConnections) {
  ASSERT_NE(iso_data_channel(), nullptr);
  IsoMockConnectionInterface mock_iface(*iso_data_channel(), lease_provider());
  constexpr hci_spec::ConnectionHandle kIsoHandle1 = 0x123;
  EXPECT_TRUE(iso_data_channel()->RegisterConnection(kIsoHandle1,
                                                     mock_iface.GetWeakPtr()));

  constexpr hci_spec::ConnectionHandle kIsoHandle2 = 0x456;
  EXPECT_TRUE(iso_data_channel()->RegisterConnection(kIsoHandle2,
                                                     mock_iface.GetWeakPtr()));

  // Attempt to re-register a handle fails
  EXPECT_FALSE(iso_data_channel()->RegisterConnection(kIsoHandle1,
                                                      mock_iface.GetWeakPtr()));

  // Can unregister connections that were previously registered
  EXPECT_TRUE(iso_data_channel()->UnregisterConnection(kIsoHandle2));
  EXPECT_TRUE(iso_data_channel()->UnregisterConnection(kIsoHandle1));

  // Cannot unregister connections that never been registered, or that have
  // already been unregistered
  constexpr hci_spec::ConnectionHandle kIsoHandle3 = 0x789;
  EXPECT_FALSE(iso_data_channel()->UnregisterConnection(kIsoHandle3));
  EXPECT_FALSE(iso_data_channel()->UnregisterConnection(kIsoHandle2));
  EXPECT_FALSE(iso_data_channel()->UnregisterConnection(kIsoHandle1));
}

// Verify that data gets directed to the correct connection
TEST_F(IsoDataChannelTests, DataDemuxification) {
  ASSERT_NE(iso_data_channel(), nullptr);

  constexpr uint32_t kNumRegisteredInterfaces = 2;
  constexpr uint32_t kNumUnregisteredInterfaces = 1;
  constexpr uint32_t kNumTotalInterfaces =
      kNumRegisteredInterfaces + kNumUnregisteredInterfaces;
  constexpr hci_spec::ConnectionHandle connection_handles[kNumTotalInterfaces] =
      {0x123, 0x456, 0x789};
  std::vector<IsoMockConnectionInterface> interfaces;
  interfaces.reserve(kNumTotalInterfaces);
  for (uint32_t i = 0; i < kNumTotalInterfaces; i++) {
    interfaces.emplace_back(*iso_data_channel(), lease_provider());
  }
  size_t expected_packet_count[kNumTotalInterfaces] = {0};

  // Register interfaces
  for (uint32_t iface_num = 0; iface_num < kNumRegisteredInterfaces;
       iface_num++) {
    ASSERT_TRUE(iso_data_channel()->RegisterConnection(
        connection_handles[iface_num], interfaces[iface_num].GetWeakPtr()));
    ASSERT_EQ(interfaces[iface_num].received_packets()->size(), 0u);
  }

  constexpr size_t kNumTestPackets = 8;
  struct {
    size_t sdu_fragment_size;
    size_t connection_num;
  } test_vector[kNumTestPackets] = {
      {100, 0},
      {120, 1},
      {140, 2},
      {160, 0},
      {180, 0},
      {200, 1},
      {220, 1},
      {240, 2},
  };

  // Send frames and verify that they are sent to the correct interfaces (or not
  // sent at all if the connection handle is unregistered).
  for (size_t test_num = 0; test_num < kNumTestPackets; test_num++) {
    size_t sdu_fragment_size = test_vector[test_num].sdu_fragment_size;
    size_t connection_num = test_vector[test_num].connection_num;
    ASSERT_TRUE(connection_num < kNumTotalInterfaces);

    std::vector<uint8_t> sdu =
        testing::GenDataBlob(sdu_fragment_size, /*starting_value=*/test_num);
    DynamicByteBuffer frame = testing::IsoDataPacket(
        /*connection_handle=*/connection_handles[connection_num],
        pw::bluetooth::emboss::IsoDataPbFlag::COMPLETE_SDU,
        /*time_stamp=*/std::nullopt,
        /*packet_sequence_number=*/123,
        /*iso_sdu_length=*/sdu_fragment_size,
        pw::bluetooth::emboss::IsoDataPacketStatus::VALID_DATA,
        sdu);
    pw::span<const std::byte> frame_as_span = frame.subspan();

    if (connection_num < kNumRegisteredInterfaces) {
      expected_packet_count[connection_num]++;
    }
    test_device()->SendIsoDataChannelPacket(frame_as_span);

    // Check that each of the connection queues has the expected number of
    // packets
    for (size_t interface_num = 0; interface_num < kNumTotalInterfaces;
         interface_num++) {
      EXPECT_EQ(interfaces[interface_num].received_packets()->size(),
                expected_packet_count[interface_num]);
    }
  }
}

TEST_F(IsoDataChannelTests, ShortRxPacket) {
  ASSERT_NE(iso_data_channel(), nullptr);
  constexpr hci_spec::ConnectionHandle kIsoHandle = 0x123;
  IsoMockConnectionInterface connection(*iso_data_channel(), lease_provider());
  iso_data_channel()->RegisterConnection(kIsoHandle, connection.GetWeakPtr());

  // Send an undersized packet and verify that it gets dropped.
  std::vector<uint8_t> short_packet = {0x01};
  test_device()->SendIsoDataChannelPacket(
      pw::span(reinterpret_cast<const std::byte*>(short_packet.data()),
               short_packet.size()));
  EXPECT_EQ(connection.received_packets()->size(), 0u);
}

TEST_F(IsoDataChannelTests, SendData) {
  ASSERT_NE(iso_data_channel(), nullptr);
  constexpr hci_spec::ConnectionHandle kIsoHandle1 = 0x123;
  IsoMockConnectionInterface connection(*iso_data_channel(), lease_provider());
  iso_data_channel()->RegisterConnection(kIsoHandle1, connection.GetWeakPtr());

  std::vector<uint8_t> sdu = testing::GenDataBlob(9, 0);

  constexpr hci_spec::ConnectionHandle kIsoHandle = 0x123;
  DynamicByteBuffer packet = testing::IsoDataPacket(
      /*handle = */ kIsoHandle,
      /*pb_flag = */ pw::bluetooth::emboss::IsoDataPbFlag::COMPLETE_SDU,
      /*timestamp = */ 0x00000000,
      /*sequence_number = */ 0x0000,
      /*iso_sdu_length = */ sdu.size(),
      /*status_flag = */
      pw::bluetooth::emboss::IsoDataPacketStatus::VALID_DATA,
      sdu);

  EXPECT_ISO_PACKET_OUT(test_device(), packet);
  connection.SendData(std::move(packet));
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());
  iso_data_channel()->UnregisterConnection(kIsoHandle1);
}

TEST_F(IsoDataChannelTests, SendDataExhaustBuffers) {
  ASSERT_NE(iso_data_channel(), nullptr);
  constexpr hci_spec::ConnectionHandle kIsoHandle = 0x123;
  IsoMockConnectionInterface connection(*iso_data_channel(), lease_provider());
  iso_data_channel()->RegisterConnection(kIsoHandle, connection.GetWeakPtr());

  for (size_t i = 0; i < kDefaultMaxNumPackets; ++i) {
    std::vector<uint8_t> sdu = testing::GenDataBlob(10, i);
    DynamicByteBuffer packet = testing::IsoDataPacket(
        /*handle = */ kIsoHandle,
        /*pb_flag = */ pw::bluetooth::emboss::IsoDataPbFlag::COMPLETE_SDU,
        /*timestamp = */ 0x00000000,
        /*sequence_number = */ i,
        /*iso_sdu_length = */ sdu.size(),
        /*status_flag = */
        pw::bluetooth::emboss::IsoDataPacketStatus::VALID_DATA,
        sdu);
    EXPECT_ISO_PACKET_OUT(test_device(), packet);
    connection.SendData(std::move(packet));
  }

  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());
}

TEST_F(IsoDataChannelTests, SendDataExceedBuffers) {
  constexpr size_t kNumExtraPacketsWithValidCompletedEvent = 2;
  constexpr size_t kNumExtraPacketsWithInvalidCompletedEvent = 2;
  constexpr size_t kNumPackets = kDefaultMaxNumPackets +
                                 kNumExtraPacketsWithValidCompletedEvent +
                                 kNumExtraPacketsWithInvalidCompletedEvent;
  constexpr size_t kSduSize = 15;
  ASSERT_NE(iso_data_channel(), nullptr);

  constexpr hci_spec::ConnectionHandle kIsoHandle = 0x123;
  constexpr hci_spec::ConnectionHandle kOtherHandle = 0x456;
  // Mock interface is not used, only registered to make sure the data channel
  // is aware that the connection is in fact an ISO connection.
  IsoMockConnectionInterface mock_iface(*iso_data_channel(), lease_provider());
  EXPECT_TRUE(iso_data_channel()->RegisterConnection(kIsoHandle,
                                                     mock_iface.GetWeakPtr()));
  size_t num_sent = 0;
  size_t num_expectations = 0;

  for (; num_sent < kNumPackets; ++num_sent) {
    std::vector<uint8_t> sdu = testing::GenDataBlob(kSduSize, num_sent);
    DynamicByteBuffer packet = testing::IsoDataPacket(
        /*handle = */ kIsoHandle,
        /*pb_flag = */ pw::bluetooth::emboss::IsoDataPbFlag::COMPLETE_SDU,
        /*timestamp = */ 0x00000000,
        /*sequence_number = */ num_sent,
        /*iso_sdu_length = */ sdu.size(),
        /*status_flag = */
        pw::bluetooth::emboss::IsoDataPacketStatus::VALID_DATA,
        /*sdu_data = */ sdu);
    if (num_sent < kDefaultMaxNumPackets) {
      ++num_expectations;
      EXPECT_ISO_PACKET_OUT(test_device(), packet);
    }
    mock_iface.SendData(std::move(packet));
  }

  EXPECT_EQ(num_sent, kNumPackets);
  EXPECT_EQ(num_expectations, kDefaultIsoBufferInfo.max_num_packets());
  EXPECT_FALSE(test_device()->AllExpectedIsoPacketsSent());

  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());

  for (size_t i = 0; i < kNumExtraPacketsWithValidCompletedEvent; ++i) {
    std::vector<uint8_t> sdu = testing::GenDataBlob(kSduSize, num_expectations);
    DynamicByteBuffer packet = testing::IsoDataPacket(
        /*handle = */ kIsoHandle,
        /*pb_flag = */ pw::bluetooth::emboss::IsoDataPbFlag::COMPLETE_SDU,
        /*timestamp = */ 0x00000000,
        /*sequence_number = */ num_expectations,
        /*iso_sdu_length = */ sdu.size(),
        /*status_flag = */
        pw::bluetooth::emboss::IsoDataPacketStatus::VALID_DATA,
        sdu);
    EXPECT_ISO_PACKET_OUT(test_device(), packet);
    ++num_expectations;
  }

  EXPECT_EQ(num_expectations,
            kDefaultMaxNumPackets + kNumExtraPacketsWithValidCompletedEvent);
  RunUntilIdle();
  EXPECT_FALSE(test_device()->AllExpectedIsoPacketsSent());

  // Send a Number_Of_Completed_Packets event for a different connection first.
  test_device()->SendCommandChannelPacket(
      testing::NumberOfCompletedPacketsPacket(kOtherHandle, 4));

  // Ensure that did not affect available buffers in IsoDataChannel.
  RunUntilIdle();
  EXPECT_FALSE(test_device()->AllExpectedIsoPacketsSent());

  // Send the event for the ISO connection.
  test_device()->SendCommandChannelPacket(
      testing::NumberOfCompletedPacketsPacket(
          kIsoHandle, kNumExtraPacketsWithValidCompletedEvent));

  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());

  // Repeat the above with a Number_Of_Completed_Packets event that has a count
  // larger than expected, to ensure it isn't ignored.
  for (; num_expectations < kNumPackets; ++num_expectations) {
    std::vector<uint8_t> sdu = testing::GenDataBlob(kSduSize, num_expectations);
    DynamicByteBuffer packet = testing::IsoDataPacket(
        /*handle = */ kIsoHandle,
        /*pb_flag = */ pw::bluetooth::emboss::IsoDataPbFlag::COMPLETE_SDU,
        /*timestamp = */ 0x00000000,
        /*sequence_number = */ num_expectations,
        /*iso_sdu_length = */ sdu.size(),
        /*status_flag = */
        pw::bluetooth::emboss::IsoDataPacketStatus::VALID_DATA,
        sdu);
    EXPECT_ISO_PACKET_OUT(test_device(), packet);
  }

  EXPECT_EQ(num_expectations, kNumPackets);

  // Send the event for the ISO connection.
  test_device()->SendCommandChannelPacket(
      testing::NumberOfCompletedPacketsPacketWithInvalidSize(
          kIsoHandle, kNumExtraPacketsWithInvalidCompletedEvent));

  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());
}

TEST_F(IsoDataChannelTests, OversizedPackets) {
  ASSERT_NE(iso_data_channel(), nullptr);

  constexpr hci_spec::ConnectionHandle kIsoHandle = 0x42;
  IsoMockConnectionInterface connection(*iso_data_channel(), lease_provider());
  EXPECT_TRUE(iso_data_channel()->RegisterConnection(kIsoHandle,
                                                     connection.GetWeakPtr()));

  constexpr size_t kTimestampSize = 4;
  constexpr size_t kSduHeaderSize = 4;

  constexpr size_t kMaxSizeWithOptional =
      kDefaultMaxDataLength - kTimestampSize - kSduHeaderSize;
  constexpr size_t kMaxSizeNoTimestamp = kDefaultMaxDataLength - kSduHeaderSize;
  constexpr size_t kMaxSizeNoOptional = kDefaultMaxDataLength;

  {
    // Create a packet that is as large as possible and ensure it can be sent.
    // With all possible optional fields.
    std::vector<uint8_t> sdu = testing::GenDataBlob(kMaxSizeWithOptional, 100);
    DynamicByteBuffer packet = testing::IsoDataPacket(
        /*handle = */ kIsoHandle,
        /*pb_flag = */ pw::bluetooth::emboss::IsoDataPbFlag::COMPLETE_SDU,
        /*timestamp = */ 0x12345678,
        /*sequence_number = */ 0,
        /*iso_sdu_length = */ sdu.size(),
        /*status_flag = */
        pw::bluetooth::emboss::IsoDataPacketStatus::VALID_DATA,
        sdu);
    EXPECT_ISO_PACKET_OUT(test_device(), packet);
    connection.SendData(std::move(packet));
  }

  {
    // Create a packet that is as large as possible and ensure it can be sent.
    // Without timestamp.
    std::vector<uint8_t> sdu = testing::GenDataBlob(kMaxSizeNoTimestamp, 107);
    DynamicByteBuffer packet = testing::IsoDataPacket(
        /*handle = */ kIsoHandle,
        /*pb_flag = */ pw::bluetooth::emboss::IsoDataPbFlag::COMPLETE_SDU,
        /*timestamp = */ std::nullopt,
        /*sequence_number = */ 0,
        /*iso_sdu_length = */ sdu.size(),
        /*status_flag = */
        pw::bluetooth::emboss::IsoDataPacketStatus::VALID_DATA,
        sdu);
    EXPECT_ISO_PACKET_OUT(test_device(), packet);
    connection.SendData(std::move(packet));
  }

  {
    // Create a packet that is as large as possible and ensure it can be sent.
    // Without any optional field (non-first/complete fragment).
    std::vector<uint8_t> sdu = testing::GenDataBlob(kMaxSizeNoOptional, 106);
    DynamicByteBuffer packet = testing::IsoDataPacket(
        /*handle = */ kIsoHandle,
        /*pb_flag = */ pw::bluetooth::emboss::IsoDataPbFlag::LAST_FRAGMENT,
        /*timestamp = */ std::nullopt,
        /*sequence_number = */ std::nullopt,
        /*iso_sdu_length = */ std::nullopt,
        /*status_flag = */ std::nullopt,
        sdu);
    EXPECT_ISO_PACKET_OUT(test_device(), packet);
    connection.SendData(std::move(packet));
  }

  {
    // Create a packet that is one byte too large.
    // With all possible optional fields.
    std::vector<uint8_t> sdu =
        testing::GenDataBlob(kMaxSizeWithOptional + 1, 54);
    DynamicByteBuffer packet = testing::IsoDataPacket(
        /*handle = */ kIsoHandle,
        /*pb_flag = */ pw::bluetooth::emboss::IsoDataPbFlag::COMPLETE_SDU,
        /*timestamp = */ 0x12345678,
        /*sequence_number = */ 0,
        /*iso_sdu_length = */ sdu.size(),
        /*status_flag = */
        pw::bluetooth::emboss::IsoDataPacketStatus::VALID_DATA,
        sdu);
    connection.SendData(std::move(packet));
  }

  {
    // Create a packet that is one byte too large.
    // Without timestamp.
    std::vector<uint8_t> sdu =
        testing::GenDataBlob(kMaxSizeNoTimestamp + 1, 55);
    DynamicByteBuffer packet = testing::IsoDataPacket(
        /*handle = */ kIsoHandle,
        /*pb_flag = */ pw::bluetooth::emboss::IsoDataPbFlag::COMPLETE_SDU,
        /*timestamp = */ std::nullopt,
        /*sequence_number = */ 0,
        /*iso_sdu_length = */ sdu.size(),
        /*status_flag = */
        pw::bluetooth::emboss::IsoDataPacketStatus::VALID_DATA,
        sdu);
    connection.SendData(std::move(packet));
  }

  {
    // Create a packet that is one byte too large.
    // Without any optional field (non-first/complete fragment).
    std::vector<uint8_t> sdu = testing::GenDataBlob(kMaxSizeNoOptional + 1, 56);
    DynamicByteBuffer packet = testing::IsoDataPacket(
        /*handle = */ kIsoHandle,
        /*pb_flag = */ pw::bluetooth::emboss::IsoDataPbFlag::LAST_FRAGMENT,
        /*timestamp = */ std::nullopt,
        /*sequence_number = */ std::nullopt,
        /*iso_sdu_length=*/std::nullopt,
        /*status_flag = */ std::nullopt,
        sdu);
    connection.SendData(std::move(packet));
  }

  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());
}

TEST_F(IsoDataChannelTests, SendDataMultipleConnections) {
  ASSERT_NE(iso_data_channel(), nullptr);
  constexpr hci_spec::ConnectionHandle kIsoHandle1 = 0x0001;
  IsoMockConnectionInterface connection1(*iso_data_channel(), lease_provider());
  iso_data_channel()->RegisterConnection(kIsoHandle1, connection1.GetWeakPtr());
  constexpr hci_spec::ConnectionHandle kIsoHandle2 = 0x0002;
  IsoMockConnectionInterface connection2(*iso_data_channel(), lease_provider());
  iso_data_channel()->RegisterConnection(kIsoHandle2, connection2.GetWeakPtr());
  EXPECT_EQ(lease_provider().lease_count(), 0u);

  size_t num_sent = 0;
  // First send a packet on connection2.
  {
    DynamicByteBuffer packet = MakeIsoPacket(kIsoHandle2, /*seq=*/num_sent);
    EXPECT_ISO_PACKET_OUT(test_device(), packet);
    connection2.SendData(std::move(packet));
    ++num_sent;
  }
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());
  EXPECT_NE(lease_provider().lease_count(), 0u);

  // Fill rest of controller buffer with connection1 packets.
  for (; num_sent < kDefaultMaxNumPackets; ++num_sent) {
    DynamicByteBuffer packet = MakeIsoPacket(kIsoHandle1, /*seq=*/num_sent);
    EXPECT_ISO_PACKET_OUT(test_device(), packet);
    connection1.SendData(std::move(packet));
  }
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());

  // Queue 2 packets in connection2.
  for (; num_sent < kDefaultMaxNumPackets + 2; ++num_sent) {
    DynamicByteBuffer packet = MakeIsoPacket(kIsoHandle2, /*seq=*/num_sent);
    connection2.SendData(std::move(packet));
  }

  // Queue 2 packets in connection1.
  for (; num_sent < kDefaultMaxNumPackets + 4; ++num_sent) {
    DynamicByteBuffer packet = MakeIsoPacket(kIsoHandle1, /*seq=*/num_sent);
    connection1.SendData(std::move(packet));
  }
  // No packets should be sent.
  RunUntilIdle();

  // The next queued connection2 packet should be sent after NOCP event.
  {
    DynamicByteBuffer expected_packet =
        MakeIsoPacket(kIsoHandle2, /*seq=*/kDefaultMaxNumPackets);
    EXPECT_ISO_PACKET_OUT(test_device(), expected_packet);
  }
  test_device()->SendCommandChannelPacket(
      testing::NumberOfCompletedPacketsPacket(kIsoHandle2, 1));
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());
  EXPECT_NE(lease_provider().lease_count(), 0u);

  // The next queued connection1 packet should be sent after NOCP event.
  {
    DynamicByteBuffer expected_packet =
        MakeIsoPacket(kIsoHandle1, /*seq=*/kDefaultMaxNumPackets + 2);
    EXPECT_ISO_PACKET_OUT(test_device(), expected_packet);
  }
  test_device()->SendCommandChannelPacket(
      testing::NumberOfCompletedPacketsPacket(kIsoHandle1, 1));
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());

  // The next queued connection2 packet and connection 1 packet should be sent
  // after NOCP event acknowledging 2 packets.
  {
    DynamicByteBuffer expected_packet =
        MakeIsoPacket(kIsoHandle2, /*seq=*/kDefaultMaxNumPackets + 1);
    EXPECT_ISO_PACKET_OUT(test_device(), expected_packet);
  }
  {
    DynamicByteBuffer expected_packet =
        MakeIsoPacket(kIsoHandle1, /*seq=*/kDefaultMaxNumPackets + 3);
    EXPECT_ISO_PACKET_OUT(test_device(), expected_packet);
  }
  test_device()->SendCommandChannelPacket(
      testing::NumberOfCompletedPacketsPacket(kIsoHandle1, 2));
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());
  EXPECT_NE(lease_provider().lease_count(), 0u);

  // Nothing else should be sent.
  test_device()->SendCommandChannelPacket(
      testing::NumberOfCompletedPacketsPacket(kIsoHandle1, 2));
  RunUntilIdle();
  EXPECT_NE(lease_provider().lease_count(), 0u);

  test_device()->SendCommandChannelPacket(
      testing::NumberOfCompletedPacketsPacket(kIsoHandle2, 2));
  RunUntilIdle();
  EXPECT_EQ(lease_provider().lease_count(), 0u);
}

TEST_F(IsoDataChannelTests, SendDataBeforeRegistering) {
  ASSERT_NE(iso_data_channel(), nullptr);
  constexpr hci_spec::ConnectionHandle kIsoHandle = 0x123;
  IsoMockConnectionInterface connection(*iso_data_channel(), lease_provider());

  DynamicByteBuffer packet = MakeIsoPacket(kIsoHandle, /*seq=*/0);
  EXPECT_ISO_PACKET_OUT(test_device(), packet);
  connection.SendData(std::move(packet));
  RunUntilIdle();
  EXPECT_FALSE(test_device()->AllExpectedIsoPacketsSent());

  iso_data_channel()->RegisterConnection(kIsoHandle, connection.GetWeakPtr());
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());

  iso_data_channel()->UnregisterConnection(kIsoHandle);
}

TEST_F(IsoDataChannelTests,
       ClearControllerPacketCountIncreasesAvailableBuffersAndSendsPacket) {
  constexpr hci_spec::ConnectionHandle kIsoHandle1 = 0x0001;
  IsoMockConnectionInterface connection1(*iso_data_channel(), lease_provider());
  iso_data_channel()->RegisterConnection(kIsoHandle1, connection1.GetWeakPtr());
  constexpr hci_spec::ConnectionHandle kIsoHandle2 = 0x0002;
  IsoMockConnectionInterface connection2(*iso_data_channel(), lease_provider());
  iso_data_channel()->RegisterConnection(kIsoHandle2, connection2.GetWeakPtr());
  EXPECT_EQ(lease_provider().lease_count(), 0u);

  // Fill controller buffer with connection1 packets.
  for (size_t num_sent = 0; num_sent < kDefaultMaxNumPackets; ++num_sent) {
    DynamicByteBuffer packet = MakeIsoPacket(kIsoHandle1, /*seq=*/num_sent);
    EXPECT_ISO_PACKET_OUT(test_device(), packet);
    connection1.SendData(std::move(packet));
  }
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());
  EXPECT_NE(lease_provider().lease_count(), 0u);

  // Queue 1 packet in connection2.
  DynamicByteBuffer packet =
      MakeIsoPacket(kIsoHandle2, /*seq=*/kDefaultMaxNumPackets);
  EXPECT_ISO_PACKET_OUT(test_device(), packet);
  connection2.SendData(std::move(packet));
  RunUntilIdle();
  EXPECT_FALSE(test_device()->AllExpectedIsoPacketsSent());

  iso_data_channel()->UnregisterConnection(kIsoHandle1);
  RunUntilIdle();
  EXPECT_FALSE(test_device()->AllExpectedIsoPacketsSent());
  EXPECT_NE(lease_provider().lease_count(), 0u);

  // Clearing connection1 pending packet count should allow connection2 packet
  // to be sent.
  iso_data_channel()->ClearControllerPacketCount(kIsoHandle1);
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());
  EXPECT_NE(lease_provider().lease_count(), 0u);

  iso_data_channel()->UnregisterConnection(kIsoHandle2);
  EXPECT_NE(lease_provider().lease_count(), 0u);
  iso_data_channel()->ClearControllerPacketCount(kIsoHandle2);
  RunUntilIdle();
  EXPECT_EQ(lease_provider().lease_count(), 0u);
}

TEST_F(IsoDataChannelTests, ClearControllerPacketCountUnknownHandleIgnored) {
  iso_data_channel()->ClearControllerPacketCount(0x0009);
}

TEST_F(IsoDataChannelTests,
       NocpAfterUnregisterAndBeforeClearControllerPacketCount) {
  constexpr hci_spec::ConnectionHandle kIsoHandle1 = 0x0001;
  IsoMockConnectionInterface connection1(*iso_data_channel(), lease_provider());
  iso_data_channel()->RegisterConnection(kIsoHandle1, connection1.GetWeakPtr());
  constexpr hci_spec::ConnectionHandle kIsoHandle2 = 0x0002;
  IsoMockConnectionInterface connection2(*iso_data_channel(), lease_provider());
  iso_data_channel()->RegisterConnection(kIsoHandle2, connection2.GetWeakPtr());

  // Fill controller buffer with connection1 packets.
  for (size_t num_sent = 0; num_sent < kDefaultMaxNumPackets; ++num_sent) {
    DynamicByteBuffer packet = MakeIsoPacket(kIsoHandle1, /*seq=*/num_sent);
    EXPECT_ISO_PACKET_OUT(test_device(), packet);
    connection1.SendData(std::move(packet));
  }
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());

  // Queue 1 packet in connection2.
  DynamicByteBuffer packet =
      MakeIsoPacket(kIsoHandle2, /*seq=*/kDefaultMaxNumPackets);
  EXPECT_ISO_PACKET_OUT(test_device(), packet);
  connection2.SendData(std::move(packet));
  RunUntilIdle();
  EXPECT_FALSE(test_device()->AllExpectedIsoPacketsSent());

  iso_data_channel()->UnregisterConnection(kIsoHandle1);
  RunUntilIdle();
  EXPECT_FALSE(test_device()->AllExpectedIsoPacketsSent());

  test_device()->SendCommandChannelPacket(
      testing::NumberOfCompletedPacketsPacket(kIsoHandle1, 1));
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());

  iso_data_channel()->ClearControllerPacketCount(kIsoHandle1);
  RunUntilIdle();
}

TEST_F(IsoDataChannelTests, NocpExceedsPendingPacketCount) {
  constexpr hci_spec::ConnectionHandle kIsoHandle1 = 0x0001;
  IsoMockConnectionInterface connection1(*iso_data_channel(), lease_provider());
  iso_data_channel()->RegisterConnection(kIsoHandle1, connection1.GetWeakPtr());

  // Fill controller buffer with connection1 packets.
  size_t num_sent = 0;
  for (; num_sent < kDefaultMaxNumPackets; ++num_sent) {
    DynamicByteBuffer packet = MakeIsoPacket(kIsoHandle1, /*seq=*/num_sent);
    EXPECT_ISO_PACKET_OUT(test_device(), packet);
    connection1.SendData(std::move(packet));
  }
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());

  // Queue a full buffer's worth of packets.
  for (; num_sent < kDefaultMaxNumPackets * 2; ++num_sent) {
    DynamicByteBuffer packet = MakeIsoPacket(kIsoHandle1, /*seq=*/num_sent);
    EXPECT_ISO_PACKET_OUT(test_device(), packet);
    connection1.SendData(std::move(packet));
  }
  RunUntilIdle();
  EXPECT_FALSE(test_device()->AllExpectedIsoPacketsSent());

  // Receive NOCP with invalid number of packets.
  test_device()->SendCommandChannelPacket(
      testing::NumberOfCompletedPacketsPacket(kIsoHandle1,
                                              kDefaultMaxNumPackets + 1));
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());

  // Next frame should not be sent until an additional NOCP.
  DynamicByteBuffer packet = MakeIsoPacket(kIsoHandle1, /*seq=*/num_sent);
  EXPECT_ISO_PACKET_OUT(test_device(), packet);
  connection1.SendData(std::move(packet));
  RunUntilIdle();
  EXPECT_FALSE(test_device()->AllExpectedIsoPacketsSent());

  test_device()->SendCommandChannelPacket(
      testing::NumberOfCompletedPacketsPacket(kIsoHandle1, 1));
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());
}

// Test fixture that does not initialize the transport with the default ISO
// buffer.
class UninitializedIsoDataChannelTest : public TestBase {
 public:
  void SetUp() override {
    TestBase::SetUp(pw::bluetooth::Controller::FeaturesBits::kHciIso);
  }

  IsoDataChannel* iso_data_channel() { return transport()->iso_data_channel(); }
};

TEST_F(UninitializedIsoDataChannelTest,
       RejectUndersizedMaxDataLengthDuringInitialization) {
  // Verify that transport initialization fails if max_data_length is too small
  // to hold the minimum required SDU header size
  DataBufferInfo invalid_buffer_info(hci_spec::kMinIsoDataPacketLength - 1,
                                     kDefaultMaxNumPackets);
  EXPECT_FALSE(transport()->InitializeIsoDataChannel(invalid_buffer_info));
}

}  // namespace bt::hci
