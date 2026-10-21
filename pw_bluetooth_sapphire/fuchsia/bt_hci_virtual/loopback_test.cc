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

#include "loopback.h"

#include <lib/component/outgoing/cpp/outgoing_directory.h>

#include <memory>
#include <string>

#include "gtest/gtest.h"
#include "pw_bluetooth_sapphire/fuchsia/bt_hci_virtual/outgoing_loop_test_fixture.h"

namespace fhb = fuchsia_hardware_bluetooth;

namespace bt_hci_virtual {
namespace {

// Vendor service instance name used by the tests that publish a device.
constexpr char kServiceDeviceName[] = "loopback-service";

class LoopbackTest : public testing::OutgoingLoopTestFixture,
                     public fidl::AsyncEventHandler<fhb::HciTransport>,
                     public fidl::AsyncEventHandler<fhb::Snoop> {
 public:
  void SetUp() override {
    ASSERT_NO_FATAL_FAILURE(OutgoingLoopTestFixture::SetUp());
    zx::channel loopback_channel_device_end;
    zx::channel::create(0, &loopback_chan_, &loopback_channel_device_end);
    zx_status_t status = loopback_device_.Initialize(
        std::move(loopback_channel_device_end), "loopback");
    ASSERT_EQ(status, ZX_OK);

    loopback_chan_wait_.set_object(loopback_chan_.get());
    loopback_chan_wait_.set_trigger(ZX_CHANNEL_READABLE |
                                    ZX_CHANNEL_PEER_CLOSED);
    zx_status_t wait_begin_status = loopback_chan_wait_.Begin(dispatcher());
    ASSERT_EQ(wait_begin_status, ZX_OK);

    ASSERT_NO_FATAL_FAILURE(OpenHciTransport());
    ASSERT_NO_FATAL_FAILURE(OpenSnoop());

    RunUntilIdle();
  }

  void TearDown() override {
    loopback_chan_wait_.Cancel();
    RunUntilIdle();
  }

  LoopbackDevice& device() { return loopback_device_; }

  zx::channel& chan() { return loopback_chan_; }

  fidl::Client<fhb::HciTransport>& hci_client() { return hci_client_; }

  fidl::Client<fhb::Snoop>& snoop_client() { return snoop_client_; }

  const std::vector<fidl::Event<fhb::Snoop::OnObservePacket>>& snoop_packets()
      const {
    return snoop_packets_;
  }

  const std::vector<fidl::Event<fhb::Snoop::OnDroppedPackets>>&
  dropped_snoop_packets() {
    return dropped_snoop_packets_;
  }

  // Packets received on the loopback channel (i.e. packets sent by
  // LoopbackDevice).
  const std::vector<std::vector<uint8_t>>& sent_packets() const {
    return sent_packets_;
  }

  const std::vector<fhb::ReceivedPacket>& received_packets() const {
    return received_packets_;
  }

  // Creates and initializes a LoopbackDevice in `device`. Returns ZX_OK on
  // success, or the first error encountered otherwise.
  zx_status_t CreateLoopbackDevice(component::OutgoingDirectory* outgoing,
                                   std::unique_ptr<LoopbackDevice>& device) {
    zx::channel loopback_channel_device_end;
    zx_status_t status = zx::channel::create(
        0, &service_loopback_chan_, &loopback_channel_device_end);
    if (status != ZX_OK) {
      return status;
    }
    device = std::make_unique<LoopbackDevice>(dispatcher());
    return device->Initialize(
        std::move(loopback_channel_device_end), kServiceDeviceName, outgoing);
  }

 private:
  // fidl::AsyncEventHandler<fhb::HciTransport> overrides:
  void OnReceive(
      ::fidl::Event<::fuchsia_hardware_bluetooth::HciTransport::OnReceive>&
          event) override {
    received_packets_.emplace_back(std::move(event));
  }
  void handle_unknown_event(
      fidl::UnknownEventMetadata<fhb::HciTransport> metadata) override {}

  // fidl::AsyncEventHandler<fhb::Snoop> overrides:
  void OnObservePacket(
      fidl::Event<fhb::Snoop::OnObservePacket>& event) override {
    snoop_packets_.emplace_back(std::move(event));
  }
  void OnDroppedPackets(
      fidl::Event<fhb::Snoop::OnDroppedPackets>& event) override {
    dropped_snoop_packets_.emplace_back(std::move(event));
  }
  void handle_unknown_event(
      fidl::UnknownEventMetadata<fhb::Snoop> metadata) override {}

  // Connects a Vendor client to `loopback_device_` and opens `hci_client_`.
  // Wrap calls in ASSERT_NO_FATAL_FAILURE so that the test stops on failure.
  void OpenHciTransport() {
    auto vendor_endpoints = fidl::CreateEndpoints<fhb::Vendor>();
    loopback_device_.Connect(std::move(vendor_endpoints->server));
    vendor_client_.Bind(std::move(vendor_endpoints->client), dispatcher());

    RunUntilIdle();
    std::optional<fidl::Result<fhb::Vendor::GetFeatures>> features;
    vendor_client_->GetFeatures().Then(
        [&features](fidl::Result<fhb::Vendor::GetFeatures>& result) {
          features = std::move(result);
        });
    RunUntilIdle();
    ASSERT_TRUE(features.has_value());
    ASSERT_TRUE(features->is_ok());
    EXPECT_EQ(features->value(), fhb::VendorFeatures());

    vendor_client_->OpenHciTransport().Then(
        [this](fidl::Result<fhb::Vendor::OpenHciTransport>& result) {
          ASSERT_TRUE(result.is_ok());
          hci_client_.Bind(std::move(result.value().channel()),
                           dispatcher(),
                           /*event_handler=*/this);
        });
    RunUntilIdle();
    ASSERT_TRUE(hci_client_.is_valid());
  }

  // Connects a Vendor client to `loopback_device_` and opens `snoop_client_`.
  // Wrap calls in ASSERT_NO_FATAL_FAILURE so that the test stops on failure.
  void OpenSnoop() {
    auto vendor_endpoints = fidl::CreateEndpoints<fhb::Vendor>();
    loopback_device_.Connect(std::move(vendor_endpoints->server));
    snoop_vendor_client_.Bind(std::move(vendor_endpoints->client),
                              dispatcher());
    snoop_vendor_client_->OpenSnoop().Then(
        [this](fidl::Result<fhb::Vendor::OpenSnoop>& result) {
          ASSERT_TRUE(result.is_ok());
          snoop_client_.Bind(std::move(result.value().channel()),
                             dispatcher(),
                             /*event_handler=*/this);
        });
    RunUntilIdle();
    ASSERT_TRUE(snoop_vendor_client_.is_valid());

    // Simulate bt-snoop dropping the Vendor client after getting a Snoop
    // client.
    auto _ = snoop_vendor_client_.UnbindMaybeGetEndpoint();
  }

  void OnChannelReady(async_dispatcher_t*,
                      async::WaitBase* wait,
                      zx_status_t status,
                      const zx_packet_signal_t* signal) {
    if (status == ZX_ERR_CANCELED) {
      return;
    }
    ASSERT_EQ(status, ZX_OK) << zx_status_get_string(status);

    if (signal->observed & ZX_CHANNEL_READABLE) {
      // Make byte buffer arbitrarily large enough to hold test packets.
      std::vector<uint8_t> bytes(255);
      uint32_t actual_bytes;
      zx_status_t read_status = loopback_chan_.read(
          /*flags=*/0,
          bytes.data(),
          /*handles=*/nullptr,
          static_cast<uint32_t>(bytes.size()),
          /*num_handles=*/0,
          &actual_bytes,
          /*actual_handles=*/nullptr);
      ASSERT_EQ(read_status, ZX_OK) << zx_status_get_string(read_status);
      bytes.resize(actual_bytes);
      sent_packets_.push_back(std::move(bytes));
    }

    if (signal->observed & ZX_CHANNEL_PEER_CLOSED) {
      return;
    }

    // The wait needs to be restarted.
    zx_status_t wait_begin_status = loopback_chan_wait_.Begin(dispatcher());
    ASSERT_EQ(wait_begin_status, ZX_OK)
        << zx_status_get_string(wait_begin_status);
  }

  zx::channel loopback_chan_;
  zx::channel service_loopback_chan_;
  LoopbackDevice loopback_device_{dispatcher()};

  fidl::Client<fhb::Vendor> vendor_client_;
  fidl::Client<fhb::HciTransport> hci_client_;

  fidl::Client<fhb::Vendor> snoop_vendor_client_;
  fidl::Client<fhb::Snoop> snoop_client_;
  std::vector<fidl::Event<fhb::Snoop::OnObservePacket>> snoop_packets_;
  std::vector<fidl::Event<fhb::Snoop::OnDroppedPackets>> dropped_snoop_packets_;

  std::vector<std::vector<uint8_t>> sent_packets_;
  std::vector<fhb::ReceivedPacket> received_packets_;
  async::WaitMethod<LoopbackTest, &LoopbackTest::OnChannelReady>
      loopback_chan_wait_{this};
};

TEST_F(LoopbackTest, SendManyCommandPackets) {
  int send_cb_count = 0;
  for (uint8_t i = 0; i < 10; i++) {
    fidl::Request<fhb::HciTransport::Send> request =
        fidl::Request<fhb::HciTransport::Send>::WithCommand({i, 0x07, 0x08});
    hci_client()->Send(request).Then(
        [&send_cb_count](fidl::Result<fhb::HciTransport::Send>& result) {
          send_cb_count++;
        });
  }
  RunUntilIdle();
  EXPECT_EQ(send_cb_count, 10);

  ASSERT_EQ(sent_packets().size(), 10u);
  for (uint8_t i = 0; i < 10; i++) {
    auto& packet = sent_packets()[i];
    std::vector<uint8_t> expected = {0x01,  // packet indicator (command)
                                     i,
                                     0x07,
                                     0x08};
    EXPECT_EQ(packet, expected);
  }

  ASSERT_EQ(snoop_packets().size(), 10u);
  for (uint8_t i = 0; i < 10; i++) {
    auto& packet = snoop_packets()[i];
    EXPECT_EQ(packet.direction(), fhb::PacketDirection::kHostToController);
    EXPECT_EQ(packet.sequence(), i + 1);
    ASSERT_EQ(packet.packet()->Which(), fhb::SnoopPacket::Tag::kCommand);
    std::vector<uint8_t> expected = {i, 0x07, 0x08};
    EXPECT_EQ(packet.packet()->command().value(), expected);
  }
}

TEST_F(LoopbackTest, SendManyAclPackets) {
  int send_cb_count = 0;
  for (uint8_t i = 0; i < 10; i++) {
    fidl::Request<fhb::HciTransport::Send> request =
        fidl::Request<fhb::HciTransport::Send>::WithAcl({i, 0x07, 0x08});
    hci_client()->Send(request).Then(
        [&send_cb_count](fidl::Result<fhb::HciTransport::Send>& result) {
          send_cb_count++;
        });
  }
  RunUntilIdle();
  EXPECT_EQ(send_cb_count, 10);

  ASSERT_EQ(sent_packets().size(), 10u);
  for (uint8_t i = 0; i < 10; i++) {
    auto& packet = sent_packets()[i];
    std::vector<uint8_t> expected = {0x02,  // packet indicator (ACL)
                                     i,
                                     0x07,
                                     0x08};
    EXPECT_EQ(packet, expected);
  }

  ASSERT_EQ(snoop_packets().size(), 10u);
  for (uint8_t i = 0; i < 10; i++) {
    auto& packet = snoop_packets()[i];
    EXPECT_EQ(packet.direction(), fhb::PacketDirection::kHostToController);
    EXPECT_EQ(packet.sequence(), i + 1);
    ASSERT_EQ(packet.packet()->Which(), fhb::SnoopPacket::Tag::kAcl);
    std::vector<uint8_t> expected = {i, 0x07, 0x08};
    EXPECT_EQ(packet.packet()->acl().value(), expected);
  }
}

TEST_F(LoopbackTest, ReceiveManyEventPackets) {
  for (uint8_t i = 0; i < LoopbackDevice::kMaxReceiveUnackedPackets; i++) {
    std::array<uint8_t, 3> packet = {0x04,  // packet indicator (event),
                                     i,
                                     0x05};
    zx_status_t status = chan().write(/*flags=*/0,
                                      packet.data(),
                                      static_cast<uint32_t>(packet.size()),
                                      /*handles=*/nullptr,
                                      /*num_handles=*/0);
    ASSERT_EQ(status, ZX_OK);
  }

  RunUntilIdle();

  ASSERT_EQ(received_packets().size(),
            LoopbackDevice::kMaxReceiveUnackedPackets);
  for (uint8_t i = 0; i < LoopbackDevice::kMaxReceiveUnackedPackets; i++) {
    auto& packet = received_packets()[i];
    ASSERT_EQ(packet.Which(), fhb::ReceivedPacket::Tag::kEvent);
    std::vector<uint8_t> expected = {i, 0x05};
    EXPECT_EQ(packet.event().value(), expected);
  }

  ASSERT_EQ(snoop_packets().size(), LoopbackDevice::kMaxReceiveUnackedPackets);
  for (uint8_t i = 0; i < LoopbackDevice::kMaxReceiveUnackedPackets; i++) {
    auto& packet = snoop_packets()[i];
    EXPECT_EQ(packet.direction(), fhb::PacketDirection::kControllerToHost);
    EXPECT_EQ(packet.sequence(), i + 1);
    ASSERT_EQ(packet.packet()->Which(), fhb::SnoopPacket::Tag::kEvent);
    std::vector<uint8_t> expected = {i, 0x05};
    EXPECT_EQ(packet.packet()->event().value(), expected);
  }
}

TEST_F(LoopbackTest, ReceiveAndQueueAndAckManyAclPackets) {
  const uint8_t arbitrary_value = 0x05;
  // 2 packets should be queued
  const size_t num_packets_sent = LoopbackDevice::kMaxReceiveUnackedPackets + 2;
  for (uint8_t i = 0; i < num_packets_sent; i++) {
    std::array<uint8_t, 3> packet = {0x02,  // packet indicator (acl),
                                     i,
                                     arbitrary_value};
    zx_status_t status = chan().write(/*flags=*/0,
                                      packet.data(),
                                      static_cast<uint32_t>(packet.size()),
                                      /*handles=*/nullptr,
                                      /*num_handles=*/0);
    ASSERT_EQ(status, ZX_OK);
  }

  RunUntilIdle();

  ASSERT_EQ(received_packets().size(),
            LoopbackDevice::kMaxReceiveUnackedPackets);
  for (uint8_t i = 0; i < LoopbackDevice::kMaxReceiveUnackedPackets; i++) {
    auto& packet = received_packets()[i];
    ASSERT_EQ(packet.Which(), fhb::ReceivedPacket::Tag::kAcl);
    std::vector<uint8_t> expected = {i, arbitrary_value};
    EXPECT_EQ(packet.acl().value(), expected);
  }

  ASSERT_EQ(snoop_packets().size(), LoopbackDevice::kMaxSnoopUnackedPackets);
  for (uint8_t i = 0; i < LoopbackDevice::kMaxSnoopUnackedPackets; i++) {
    auto& packet = snoop_packets()[i];
    EXPECT_EQ(packet.direction(), fhb::PacketDirection::kControllerToHost);
    EXPECT_EQ(packet.sequence(), i + 1);
    ASSERT_EQ(packet.packet()->Which(), fhb::SnoopPacket::Tag::kAcl);
    std::vector<uint8_t> expected = {i, arbitrary_value};
    EXPECT_EQ(packet.packet()->acl().value(), expected);
  }

  // Ack 2x so that the 2 queued packets are sent.
  fit::result<fidl::OneWayError> result = hci_client()->AckReceive();
  ASSERT_TRUE(result.is_ok());
  result = hci_client()->AckReceive();
  ASSERT_TRUE(result.is_ok());
  RunUntilIdle();

  ASSERT_EQ(received_packets().size(), num_packets_sent);
  for (uint8_t i = 0; i < num_packets_sent; i++) {
    auto& packet = received_packets()[i];
    ASSERT_EQ(packet.Which(), fhb::ReceivedPacket::Tag::kAcl);
    std::vector<uint8_t> expected = {i, arbitrary_value};
    EXPECT_EQ(packet.acl().value(), expected);
  }

  // Ack 2x so that the 2 queued packets are sent.
  result = snoop_client()->AcknowledgePackets(
      fhb::SnoopAcknowledgePacketsRequest(2));
  ASSERT_TRUE(result.is_ok());
  RunUntilIdle();

  ASSERT_EQ(snoop_packets().size(), num_packets_sent);
  for (uint8_t i = 0; i < num_packets_sent; i++) {
    auto& packet = snoop_packets()[i];
    EXPECT_EQ(packet.direction(), fhb::PacketDirection::kControllerToHost);
    EXPECT_EQ(packet.sequence(), i + 1);
    ASSERT_EQ(packet.packet()->Which(), fhb::SnoopPacket::Tag::kAcl);
    std::vector<uint8_t> expected = {i, arbitrary_value};
    EXPECT_EQ(packet.packet()->acl().value(), expected);
  }
}

TEST_F(LoopbackTest, DropSnoopPackets) {
  // 2 packets should get dropped
  const size_t num_dropped = 2;
  const size_t num_packets_sent = LoopbackDevice::kMaxSnoopUnackedPackets +
                                  LoopbackDevice::kMaxSnoopQueueSize +
                                  num_dropped;
  size_t send_cb_count = 0;
  for (uint8_t i = 0; i < num_packets_sent; i++) {
    fidl::Request<fhb::HciTransport::Send> request =
        fidl::Request<fhb::HciTransport::Send>::WithAcl({i, 0x07, 0x08});
    hci_client()->Send(request).Then(
        [&send_cb_count](fidl::Result<fhb::HciTransport::Send>& result) {
          send_cb_count++;
        });
  }
  RunUntilIdle();
  EXPECT_EQ(send_cb_count, num_packets_sent);

  ASSERT_EQ(snoop_packets().size(), LoopbackDevice::kMaxSnoopUnackedPackets);

  fit::result<fidl::OneWayError> result =
      snoop_client()->AcknowledgePackets(fhb::SnoopAcknowledgePacketsRequest(
          LoopbackDevice::kMaxSnoopUnackedPackets));
  ASSERT_TRUE(result.is_ok());
  RunUntilIdle();
  ASSERT_EQ(snoop_packets().size(),
            2 * LoopbackDevice::kMaxSnoopUnackedPackets - num_dropped);
  for (uint8_t i = LoopbackDevice::kMaxSnoopUnackedPackets;
       static_cast<size_t>(i) < snoop_packets().size();
       i++) {
    auto& packet = snoop_packets()[i];
    EXPECT_EQ(packet.direction(), fhb::PacketDirection::kHostToController);
    EXPECT_EQ(packet.sequence(), i + 1 + num_dropped);
    ASSERT_EQ(packet.packet()->Which(), fhb::SnoopPacket::Tag::kAcl);
    std::vector<uint8_t> expected = {
        static_cast<uint8_t>(i + num_dropped), 0x07, 0x08};
    EXPECT_EQ(packet.packet()->acl().value(), expected);
  }

  ASSERT_EQ(dropped_snoop_packets().size(), 1u);
  EXPECT_EQ(dropped_snoop_packets()[0].sent(), 2u);
  EXPECT_EQ(dropped_snoop_packets()[0].received(), 0u);
}

// Initialize() publishes the vendor service instance named after the device.
TEST_F(LoopbackTest, PublishService) {
  std::unique_ptr<LoopbackDevice> device;
  ASSERT_EQ(CreateLoopbackDevice(outgoing(), device), ZX_OK);
  EXPECT_TRUE(VendorServiceIsServed(kServiceDeviceName));
}

// The vendor service instance goes away with the device.
TEST_F(LoopbackTest, DestroyDeviceUnpublishesService) {
  std::unique_ptr<LoopbackDevice> device;
  ASSERT_EQ(CreateLoopbackDevice(outgoing(), device), ZX_OK);
  device.reset();
  EXPECT_FALSE(VendorServiceIsServed(kServiceDeviceName));
}

}  // namespace
}  // namespace bt_hci_virtual
