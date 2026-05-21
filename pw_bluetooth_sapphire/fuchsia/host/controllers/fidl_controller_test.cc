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

#include "pw_bluetooth_sapphire/fuchsia/host/controllers/fidl_controller.h"

#include <optional>

#include "gmock/gmock.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/fake_vendor_server.h"
#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"
#include "pw_bluetooth_sapphire/internal/host/testing/gtest_helpers.h"
#include "pw_bluetooth_sapphire/internal/host/testing/loop_fixture.h"

namespace fhbt = fuchsia_hardware_bluetooth;

namespace bt::controllers {

namespace {

constexpr hci_spec::ConnectionHandle kConnectionHandle = 0x0001;
const StaticByteBuffer kSetAclPriorityNormalCommand(0x00);
const StaticByteBuffer kSetAclPrioritySourceCommand(0x01);
const StaticByteBuffer kSetAclPrioritySinkCommand(0x02);

}  // namespace

class FidlControllerTest : public bt::testing::TestLoopFixture {
 public:
  void SetUp() override {
    auto [vendor_client_end, vendor_server_end] =
        ::fidl::Endpoints<fhbt::Vendor>::Create();

    fake_vendor_server_.emplace(std::move(vendor_server_end), dispatcher());
    fidl_controller_.emplace(std::move(vendor_client_end), dispatcher());
  }

  void InitializeController() {
    controller()->Initialize(
        [&](pw::Status cb_complete_status) {
          complete_status_ = cb_complete_status;
        },
        [&](pw::Status cb_error) { controller_error_ = cb_error; });
    ASSERT_FALSE(complete_status_.has_value());
    ASSERT_FALSE(controller_error_.has_value());
  }

  FidlController* controller() { return &fidl_controller_.value(); }

  fidl::testing::FakeHciTransportServer* hci_server() {
    return fake_vendor_server_->hci_server();
  }

  fidl::testing::FakeVendorServer* vendor_server() {
    return &fake_vendor_server_.value();
  }

  std::optional<pw::Status> complete_status() const { return complete_status_; }

  std::optional<pw::Status> controller_error() const {
    return controller_error_;
  }

 private:
  std::optional<pw::Status> complete_status_;
  std::optional<pw::Status> controller_error_;
  std::optional<fidl::testing::FakeVendorServer> fake_vendor_server_;
  std::optional<FidlController> fidl_controller_;
};

TEST_F(FidlControllerTest, SendAndReceiveAclPackets) {
  RETURN_IF_FATAL(InitializeController());
  RunLoopUntilIdle();
  ASSERT_THAT(complete_status(), ::testing::Optional(PW_STATUS_OK));

  const StaticByteBuffer acl_packet_0(0x00, 0x01, 0x02, 0x03);
  controller()->SendAclData(acl_packet_0.subspan());
  RunLoopUntilIdle();
  ASSERT_EQ(hci_server()->acl_packets_received().size(), 1u);
  EXPECT_THAT(hci_server()->acl_packets_received()[0], BufferEq(acl_packet_0));

  const StaticByteBuffer acl_packet_1(0x04, 0x05, 0x06, 0x07);
  controller()->SendAclData(acl_packet_1.subspan());
  RunLoopUntilIdle();
  ASSERT_EQ(hci_server()->acl_packets_received().size(), 2u);
  EXPECT_THAT(hci_server()->acl_packets_received()[1], BufferEq(acl_packet_1));

  std::vector<DynamicByteBuffer> received_acl;
  controller()->SetReceiveAclFunction([&](pw::span<const std::byte> buffer) {
    received_acl.emplace_back(BufferView(buffer.data(), buffer.size()));
  });

  hci_server()->SendAcl(acl_packet_0.view());
  RunLoopUntilIdle();
  ASSERT_EQ(received_acl.size(), 1u);
  EXPECT_THAT(received_acl[0], BufferEq(acl_packet_0));
  EXPECT_EQ(hci_server()->acks_received(), 1u);

  hci_server()->SendAcl(acl_packet_1.view());
  RunLoopUntilIdle();
  ASSERT_EQ(received_acl.size(), 2u);
  EXPECT_THAT(received_acl[1], BufferEq(acl_packet_1));
  EXPECT_EQ(hci_server()->acks_received(), 2u);

  std::optional<pw::Status> close_status;
  controller()->Close([&](pw::Status status) { close_status = status; });
  ASSERT_TRUE(close_status.has_value());
  EXPECT_EQ(close_status.value(), PW_STATUS_OK);
}

TEST_F(FidlControllerTest, SendCommandsAndReceiveEvents) {
  RETURN_IF_FATAL(InitializeController());
  RunLoopUntilIdle();
  ASSERT_THAT(complete_status(), ::testing::Optional(PW_STATUS_OK));

  const StaticByteBuffer packet_0(0x00, 0x01, 0x02, 0x03);
  controller()->SendCommand(packet_0.subspan());
  RunLoopUntilIdle();
  ASSERT_EQ(hci_server()->commands_received().size(), 1u);
  EXPECT_THAT(hci_server()->commands_received()[0], BufferEq(packet_0));

  const StaticByteBuffer packet_1(0x04, 0x05, 0x06, 0x07);
  controller()->SendCommand(packet_1.subspan());
  RunLoopUntilIdle();
  ASSERT_EQ(hci_server()->commands_received().size(), 2u);
  EXPECT_THAT(hci_server()->commands_received()[1], BufferEq(packet_1));

  std::vector<DynamicByteBuffer> events;
  controller()->SetEventFunction([&](pw::span<const std::byte> buffer) {
    events.emplace_back(BufferView(buffer.data(), buffer.size()));
  });

  hci_server()->SendEvent(packet_1.view());
  RunLoopUntilIdle();
  ASSERT_EQ(events.size(), 1u);
  EXPECT_THAT(events[0], BufferEq(packet_1));
  EXPECT_EQ(hci_server()->acks_received(), 1u);

  hci_server()->SendEvent(packet_1.view());
  RunLoopUntilIdle();
  ASSERT_EQ(events.size(), 2u);
  EXPECT_THAT(events[1], BufferEq(packet_1));
  EXPECT_EQ(hci_server()->acks_received(), 2u);

  std::optional<pw::Status> close_status;
  controller()->Close([&](pw::Status status) { close_status = status; });
  ASSERT_TRUE(close_status.has_value());
  EXPECT_EQ(close_status.value(), PW_STATUS_OK);
}

TEST_F(FidlControllerTest, SendScoWhenNotConfigured) {
  RETURN_IF_FATAL(InitializeController());
  RunLoopUntilIdle();
  ASSERT_THAT(complete_status(), ::testing::Optional(PW_STATUS_OK));

  const StaticByteBuffer packet_0(0x00, 0x01, 0x02, 0x03);
  controller()->SendScoData(packet_0.subspan());
  RunLoopUntilIdle();
  ASSERT_EQ(hci_server()->sco_packets_received().size(), 0u);
  ASSERT_THAT(controller_error(), ::testing::Optional(PW_STATUS_UNKNOWN));
}

TEST_F(FidlControllerTest, SendAndReceiveSco) {
  RETURN_IF_FATAL(InitializeController());
  RunLoopUntilIdle();
  ASSERT_THAT(complete_status(), ::testing::Optional(PW_STATUS_OK));

  int controller_cb_count = 0;
  controller()->ConfigureSco(pw::bluetooth::Controller::ScoCodingFormat::kCvsd,
                             pw::bluetooth::Controller::ScoEncoding::k8Bits,
                             pw::bluetooth::Controller::ScoSampleRate::k8Khz,
                             [&controller_cb_count](pw::Status status) {
                               controller_cb_count++;
                               EXPECT_EQ(status, PW_STATUS_OK);
                             });
  EXPECT_EQ(controller_cb_count, 1);

  const StaticByteBuffer packet_0(0x00, 0x01, 0x02, 0x03);
  controller()->SendScoData(packet_0.subspan());
  RunLoopUntilIdle();
  ASSERT_EQ(hci_server()->sco_packets_received().size(), 1u);
  EXPECT_THAT(hci_server()->sco_packets_received()[0], BufferEq(packet_0));

  const StaticByteBuffer packet_1(0x04, 0x05, 0x06, 0x07);
  controller()->SendScoData(packet_1.subspan());
  RunLoopUntilIdle();
  ASSERT_EQ(hci_server()->sco_packets_received().size(), 2u);
  EXPECT_THAT(hci_server()->sco_packets_received()[1], BufferEq(packet_1));

  std::vector<DynamicByteBuffer> received_sco;
  controller()->SetReceiveScoFunction([&](pw::span<const std::byte> buffer) {
    received_sco.emplace_back(BufferView(buffer.data(), buffer.size()));
  });

  hci_server()->SendSco(packet_1.view());
  RunLoopUntilIdle();
  ASSERT_EQ(received_sco.size(), 1u);
  EXPECT_THAT(received_sco[0], BufferEq(packet_1));
  EXPECT_EQ(hci_server()->sco_acks_received(), 1u);
  EXPECT_EQ(hci_server()->acks_received(), 0u);

  hci_server()->SendSco(packet_1.view());
  RunLoopUntilIdle();
  ASSERT_EQ(received_sco.size(), 2u);
  EXPECT_THAT(received_sco[1], BufferEq(packet_1));
  EXPECT_EQ(hci_server()->sco_acks_received(), 2u);

  std::optional<pw::Status> close_status;
  controller()->Close([&](pw::Status status) { close_status = status; });
  ASSERT_TRUE(close_status.has_value());
  EXPECT_EQ(close_status.value(), PW_STATUS_OK);
}

TEST_F(FidlControllerTest, SendAndReceiveIso) {
  RETURN_IF_FATAL(InitializeController());
  RunLoopUntilIdle();
  ASSERT_THAT(complete_status(), ::testing::Optional(PW_STATUS_OK));

  const StaticByteBuffer iso_packet_0(0x00, 0x01, 0x02, 0x03);
  controller()->SendIsoData(iso_packet_0.subspan());
  RunLoopUntilIdle();
  ASSERT_EQ(hci_server()->iso_packets_received().size(), 1u);
  EXPECT_THAT(hci_server()->iso_packets_received()[0], BufferEq(iso_packet_0));

  const StaticByteBuffer iso_packet_1(0x04, 0x05, 0x06, 0x07);
  controller()->SendIsoData(iso_packet_1.subspan());
  RunLoopUntilIdle();
  ASSERT_EQ(hci_server()->iso_packets_received().size(), 2u);
  EXPECT_THAT(hci_server()->iso_packets_received()[1], BufferEq(iso_packet_1));

  std::vector<DynamicByteBuffer> received_iso;
  controller()->SetReceiveIsoFunction([&](pw::span<const std::byte> buffer) {
    received_iso.emplace_back(BufferView(buffer.data(), buffer.size()));
  });

  hci_server()->SendIso(iso_packet_0.view());
  RunLoopUntilIdle();
  ASSERT_EQ(received_iso.size(), 1u);
  EXPECT_THAT(received_iso[0], BufferEq(iso_packet_0));
  EXPECT_EQ(hci_server()->acks_received(), 1u);

  hci_server()->SendIso(iso_packet_1.view());
  RunLoopUntilIdle();
  ASSERT_EQ(received_iso.size(), 2u);
  EXPECT_THAT(received_iso[1], BufferEq(iso_packet_1));
  EXPECT_EQ(hci_server()->acks_received(), 2u);

  std::optional<pw::Status> close_status;
  controller()->Close([&](pw::Status status) { close_status = status; });
  ASSERT_TRUE(close_status.has_value());
  EXPECT_EQ(close_status.value(), PW_STATUS_OK);
}

TEST_F(FidlControllerTest,
       ConfigureScoWithFormatCvsdEncoding8BitsSampleRate8Khz) {
  RETURN_IF_FATAL(InitializeController());
  RunLoopUntilIdle();
  ASSERT_THAT(complete_status(), ::testing::Optional(PW_STATUS_OK));

  int device_cb_count = 0;
  hci_server()->set_check_configure_sco(
      [&device_cb_count](fhbt::ScoCodingFormat format,
                         fhbt::ScoEncoding encoding,
                         fhbt::ScoSampleRate rate) mutable {
        device_cb_count++;
        EXPECT_EQ(format, fhbt::ScoCodingFormat::kCvsd);
        EXPECT_EQ(encoding, fhbt::ScoEncoding::kBits8);
        EXPECT_EQ(rate, fhbt::ScoSampleRate::kKhz8);
      });

  int controller_cb_count = 0;
  controller()->ConfigureSco(pw::bluetooth::Controller::ScoCodingFormat::kCvsd,
                             pw::bluetooth::Controller::ScoEncoding::k8Bits,
                             pw::bluetooth::Controller::ScoSampleRate::k8Khz,
                             [&controller_cb_count](pw::Status status) {
                               controller_cb_count++;
                               EXPECT_EQ(status, PW_STATUS_OK);
                             });

  EXPECT_EQ(device_cb_count, 0);
  EXPECT_EQ(controller_cb_count, 1);
  RunLoopUntilIdle();
  EXPECT_EQ(controller_cb_count, 1);
  EXPECT_EQ(device_cb_count, 1);
}

TEST_F(FidlControllerTest,
       ConfigureScoWithFormatCvsdEncoding16BitsSampleRate8Khz) {
  RETURN_IF_FATAL(InitializeController());
  RunLoopUntilIdle();
  ASSERT_THAT(complete_status(), ::testing::Optional(PW_STATUS_OK));

  hci_server()->set_check_configure_sco([](fhbt::ScoCodingFormat format,
                                           fhbt::ScoEncoding encoding,
                                           fhbt::ScoSampleRate rate) {
    EXPECT_EQ(format, fhbt::ScoCodingFormat::kCvsd);
    EXPECT_EQ(encoding, fhbt::ScoEncoding::kBits16);
    EXPECT_EQ(rate, fhbt::ScoSampleRate::kKhz8);
  });

  int config_cb_count = 0;
  controller()->ConfigureSco(pw::bluetooth::Controller::ScoCodingFormat::kCvsd,
                             pw::bluetooth::Controller::ScoEncoding::k16Bits,
                             pw::bluetooth::Controller::ScoSampleRate::k8Khz,
                             [&](pw::Status status) {
                               config_cb_count++;
                               EXPECT_EQ(status, PW_STATUS_OK);
                             });
  RunLoopUntilIdle();
  EXPECT_EQ(config_cb_count, 1);
}

TEST_F(FidlControllerTest,
       ConfigureScoWithFormatCvsdEncoding16BitsSampleRate16Khz) {
  RETURN_IF_FATAL(InitializeController());
  RunLoopUntilIdle();
  ASSERT_THAT(complete_status(), ::testing::Optional(PW_STATUS_OK));

  hci_server()->set_check_configure_sco([](fhbt::ScoCodingFormat format,
                                           fhbt::ScoEncoding encoding,
                                           fhbt::ScoSampleRate rate) {
    EXPECT_EQ(format, fhbt::ScoCodingFormat::kCvsd);
    EXPECT_EQ(encoding, fhbt::ScoEncoding::kBits16);
    EXPECT_EQ(rate, fhbt::ScoSampleRate::kKhz16);
  });

  int config_cb_count = 0;
  controller()->ConfigureSco(pw::bluetooth::Controller::ScoCodingFormat::kCvsd,
                             pw::bluetooth::Controller::ScoEncoding::k16Bits,
                             pw::bluetooth::Controller::ScoSampleRate::k16Khz,
                             [&](pw::Status status) {
                               config_cb_count++;
                               EXPECT_EQ(status, PW_STATUS_OK);
                             });
  RunLoopUntilIdle();
  EXPECT_EQ(config_cb_count, 1);
}

TEST_F(FidlControllerTest,
       ConfigureScoWithFormatMsbcEncoding16BitsSampleRate16Khz) {
  RETURN_IF_FATAL(InitializeController());
  RunLoopUntilIdle();
  ASSERT_THAT(complete_status(), ::testing::Optional(PW_STATUS_OK));

  hci_server()->set_check_configure_sco([](fhbt::ScoCodingFormat format,
                                           fhbt::ScoEncoding encoding,
                                           fhbt::ScoSampleRate rate) {
    EXPECT_EQ(format, fhbt::ScoCodingFormat::kMsbc);
    EXPECT_EQ(encoding, fhbt::ScoEncoding::kBits16);
    EXPECT_EQ(rate, fhbt::ScoSampleRate::kKhz16);
  });

  int config_cb_count = 0;
  controller()->ConfigureSco(pw::bluetooth::Controller::ScoCodingFormat::kMsbc,
                             pw::bluetooth::Controller::ScoEncoding::k16Bits,
                             pw::bluetooth::Controller::ScoSampleRate::k16Khz,
                             [&](pw::Status status) {
                               config_cb_count++;
                               EXPECT_EQ(status, PW_STATUS_OK);
                             });
  RunLoopUntilIdle();
  EXPECT_EQ(config_cb_count, 1);
}

TEST_F(FidlControllerTest, ConfigureAndResetScoTwice) {
  RETURN_IF_FATAL(InitializeController());
  RunLoopUntilIdle();
  ASSERT_THAT(complete_status(), ::testing::Optional(PW_STATUS_OK));

  int device_cb_count = 0;
  hci_server()->set_reset_sco_callback(
      [&device_cb_count]() { device_cb_count++; });

  int config_cb_count_0 = 0;
  controller()->ConfigureSco(pw::bluetooth::Controller::ScoCodingFormat::kMsbc,
                             pw::bluetooth::Controller::ScoEncoding::k16Bits,
                             pw::bluetooth::Controller::ScoSampleRate::k16Khz,
                             [&](pw::Status status) {
                               config_cb_count_0++;
                               EXPECT_EQ(status, PW_STATUS_OK);
                             });
  EXPECT_EQ(config_cb_count_0, 1);

  int reset_cb_count_0 = 0;
  controller()->ResetSco([&](pw::Status status) {
    reset_cb_count_0++;
    EXPECT_EQ(status, PW_STATUS_OK);
  });

  EXPECT_EQ(device_cb_count, 0);
  EXPECT_EQ(reset_cb_count_0, 0);
  RunLoopUntilIdle();
  EXPECT_EQ(device_cb_count, 1);
  EXPECT_EQ(reset_cb_count_0, 1);

  int config_cb_count_1 = 0;
  controller()->ConfigureSco(pw::bluetooth::Controller::ScoCodingFormat::kMsbc,
                             pw::bluetooth::Controller::ScoEncoding::k16Bits,
                             pw::bluetooth::Controller::ScoSampleRate::k16Khz,
                             [&](pw::Status status) {
                               config_cb_count_1++;
                               EXPECT_EQ(status, PW_STATUS_OK) << status.str();
                             });
  EXPECT_EQ(config_cb_count_1, 1);

  int reset_cb_count_1 = 0;
  controller()->ResetSco([&](pw::Status status) {
    reset_cb_count_1++;
    EXPECT_EQ(status, PW_STATUS_OK);
  });

  EXPECT_EQ(reset_cb_count_1, 0);
  RunLoopUntilIdle();
  EXPECT_EQ(device_cb_count, 2);
  EXPECT_EQ(reset_cb_count_0, 1);
  EXPECT_EQ(reset_cb_count_1, 1);
}

TEST_F(FidlControllerTest, CloseUnbindsHciProtocol) {
  RETURN_IF_FATAL(InitializeController());
  RunLoopUntilIdle();
  ASSERT_THAT(complete_status(), ::testing::Optional(PW_STATUS_OK));

  std::optional<pw::Status> close_status;
  controller()->Close([&](pw::Status status) { close_status = status; });
  RunLoopUntilIdle();
  ASSERT_TRUE(close_status.has_value());
  EXPECT_EQ(close_status.value(), PW_STATUS_OK);
  EXPECT_FALSE(hci_server()->bound());
}

TEST_F(FidlControllerTest, HciServerClosesProtocol) {
  RETURN_IF_FATAL(InitializeController());
  RunLoopUntilIdle();
  ASSERT_THAT(complete_status(), ::testing::Optional(PW_STATUS_OK));

  hci_server()->Unbind();
  RunLoopUntilIdle();
  ASSERT_THAT(controller_error(),
              ::testing::Optional(pw::Status::Unavailable()));
}

TEST_F(FidlControllerTest, VendorGetFeatures) {
  RETURN_IF_FATAL(InitializeController());
  RunLoopUntilIdle();
  ASSERT_THAT(complete_status(), ::testing::Optional(PW_STATUS_OK));

  std::optional<FidlController::FeaturesBits> features;
  controller()->GetFeatures(
      [&](FidlController::FeaturesBits bits) { features = bits; });
  RunLoopUntilIdle();
  ASSERT_TRUE(features.has_value());
  EXPECT_TRUE(*features & FidlController::FeaturesBits::kSetAclPriorityCommand);
  EXPECT_TRUE(*features & FidlController::FeaturesBits::kHciSco);

  std::optional<pw::Status> close_status;
  controller()->Close([&](pw::Status status) { close_status = status; });
  ASSERT_TRUE(close_status.has_value());
  EXPECT_EQ(close_status.value(), PW_STATUS_OK);
}

TEST_F(FidlControllerTest, VendorEncodeSetAclPriorityCommandNormal) {
  RETURN_IF_FATAL(InitializeController());
  RunLoopUntilIdle();
  ASSERT_THAT(complete_status(), ::testing::Optional(PW_STATUS_OK));

  pw::bluetooth::SetAclPriorityCommandParameters params;
  params.connection_handle = kConnectionHandle;
  params.priority = pw::bluetooth::AclPriority::kNormal;

  std::optional<DynamicByteBuffer> buffer;
  controller()->EncodeVendorCommand(
      params, [&](pw::Result<pw::span<const std::byte>> result) {
        ASSERT_TRUE(result.ok());
        buffer.emplace(
            BufferView(result.value().data(), result.value().size()));
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(buffer);
  EXPECT_THAT(*buffer, BufferEq(kSetAclPriorityNormalCommand));
}

TEST_F(FidlControllerTest, VendorEncodeSetAclPriorityCommandSink) {
  RETURN_IF_FATAL(InitializeController());
  RunLoopUntilIdle();
  ASSERT_THAT(complete_status(), ::testing::Optional(PW_STATUS_OK));

  pw::bluetooth::SetAclPriorityCommandParameters params;
  params.connection_handle = kConnectionHandle;
  params.priority = pw::bluetooth::AclPriority::kSink;

  std::optional<DynamicByteBuffer> buffer;
  controller()->EncodeVendorCommand(
      params, [&](pw::Result<pw::span<const std::byte>> result) {
        ASSERT_TRUE(result.ok());
        buffer.emplace(
            BufferView(result.value().data(), result.value().size()));
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(buffer);
  EXPECT_THAT(*buffer, BufferEq(kSetAclPrioritySinkCommand));
}

TEST_F(FidlControllerTest, VendorEncodeSetAclPriorityCommandSource) {
  RETURN_IF_FATAL(InitializeController());
  RunLoopUntilIdle();
  ASSERT_THAT(complete_status(), ::testing::Optional(PW_STATUS_OK));

  pw::bluetooth::SetAclPriorityCommandParameters params;
  params.connection_handle = kConnectionHandle;
  params.priority = pw::bluetooth::AclPriority::kSource;

  std::optional<DynamicByteBuffer> buffer;
  controller()->EncodeVendorCommand(
      params, [&](pw::Result<pw::span<const std::byte>> result) {
        ASSERT_TRUE(result.ok());
        buffer.emplace(
            BufferView(result.value().data(), result.value().size()));
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(buffer);
  EXPECT_THAT(*buffer, BufferEq(kSetAclPrioritySourceCommand));
}

TEST_F(FidlControllerTest, VendorServerClosesChannelBeforeOpenHci) {
  RETURN_IF_FATAL(InitializeController());
  ASSERT_THAT(complete_status(), std::nullopt);
  ASSERT_THAT(controller_error(), std::nullopt);

  vendor_server()->Unbind();
  RunLoopUntilIdle();
  ASSERT_THAT(complete_status(),
              ::testing::Optional(pw::Status::Unavailable()));
  ASSERT_THAT(controller_error(), std::nullopt);
}

TEST_F(FidlControllerTest, VendorServerClosesProtocolBeforeInitialize) {
  vendor_server()->Unbind();
  RunLoopUntilIdle();

  RETURN_IF_FATAL(InitializeController());
  RunLoopUntilIdle();
  ASSERT_THAT(complete_status(),
              ::testing::Optional(pw::Status::Unavailable()));
  ASSERT_THAT(controller_error(), std::nullopt);
}

TEST_F(FidlControllerTest, VendorOpenHciError) {
  // Make OpenHci() return error during controller initialization
  vendor_server()->set_open_hci_error(true);

  RETURN_IF_FATAL(InitializeController());
  RunLoopUntilIdle();
  ASSERT_THAT(complete_status(), ::testing::Optional(pw::Status::Internal()));
  ASSERT_THAT(controller_error(), std::nullopt);
}

TEST_F(FidlControllerTest, VendorServerClosesProtocol) {
  RETURN_IF_FATAL(InitializeController());
  RunLoopUntilIdle();
  ASSERT_THAT(complete_status(), ::testing::Optional(PW_STATUS_OK));

  vendor_server()->Unbind();
  RunLoopUntilIdle();
  ASSERT_THAT(controller_error(),
              ::testing::Optional(pw::Status::Unavailable()));
}

// Attempting to send a command once the HciTransport protocol has been closed
// from the server end may trigger the driver to terminate. Verify that a clean
// shutdown will still occur.
TEST_F(FidlControllerTest, EventClosesDriver) {
  RETURN_IF_FATAL(InitializeController());
  RunLoopUntilIdle();
  ASSERT_THAT(complete_status(), ::testing::Optional(PW_STATUS_OK));

  const StaticByteBuffer kCommandPacket(0x00, 0x01, 0x02, 0x03);
  const StaticByteBuffer kEventPacket(0x04, 0x05, 0x06, 0x07);
  controller()->SetEventFunction([&](pw::span<const std::byte> /* buffer */) {
    hci_server()->Unbind();
    controller()->SendCommand(kCommandPacket.subspan());
  });
  hci_server()->SendEvent(kEventPacket.view());
  RunLoopUntilIdle();
}

TEST_F(FidlControllerTest, ScoServerClosesProtocolUnexpectedly) {
  RETURN_IF_FATAL(InitializeController());
  RunLoopUntilIdle();
  ASSERT_THAT(complete_status(), ::testing::Optional(PW_STATUS_OK));

  int config_cb_count = 0;
  controller()->ConfigureSco(pw::bluetooth::Controller::ScoCodingFormat::kMsbc,
                             pw::bluetooth::Controller::ScoEncoding::k16Bits,
                             pw::bluetooth::Controller::ScoSampleRate::k16Khz,
                             [&](pw::Status status) {
                               config_cb_count++;
                               EXPECT_EQ(status, PW_STATUS_OK);
                             });
  RunLoopUntilIdle();
  EXPECT_EQ(config_cb_count, 1);

  EXPECT_TRUE(hci_server()->UnbindSco());
  RunLoopUntilIdle();
  EXPECT_THAT(controller_error(), ::testing::Optional(pw::Status::Unknown()));
}

TEST_F(FidlControllerTest, ConfigureScoAlreadyConfigured) {
  RETURN_IF_FATAL(InitializeController());
  RunLoopUntilIdle();
  ASSERT_THAT(complete_status(), ::testing::Optional(PW_STATUS_OK));

  int config_cb_count_0 = 0;
  controller()->ConfigureSco(pw::bluetooth::Controller::ScoCodingFormat::kMsbc,
                             pw::bluetooth::Controller::ScoEncoding::k16Bits,
                             pw::bluetooth::Controller::ScoSampleRate::k16Khz,
                             [&](pw::Status status) {
                               config_cb_count_0++;
                               EXPECT_EQ(status, PW_STATUS_OK);
                             });
  int config_cb_count_1 = 0;
  controller()->ConfigureSco(pw::bluetooth::Controller::ScoCodingFormat::kMsbc,
                             pw::bluetooth::Controller::ScoEncoding::k16Bits,
                             pw::bluetooth::Controller::ScoSampleRate::k16Khz,
                             [&](pw::Status status) {
                               config_cb_count_1++;
                               EXPECT_EQ(status, PW_STATUS_ALREADY_EXISTS);
                             });
  EXPECT_EQ(config_cb_count_0, 1);
  EXPECT_EQ(config_cb_count_1, 1);
  RunLoopUntilIdle();
  EXPECT_EQ(config_cb_count_0, 1);
  EXPECT_EQ(config_cb_count_1, 1);
}

TEST_F(FidlControllerTest, ResetScoWhenNotConfiguredFails) {
  RETURN_IF_FATAL(InitializeController());
  RunLoopUntilIdle();
  ASSERT_THAT(complete_status(), ::testing::Optional(PW_STATUS_OK));

  int device_cb_count = 0;
  hci_server()->set_reset_sco_callback(
      [&device_cb_count]() { device_cb_count++; });

  int reset_cb_count = 0;
  controller()->ResetSco([&](pw::Status status) {
    reset_cb_count++;
    EXPECT_EQ(status, PW_STATUS_FAILED_PRECONDITION);
  });

  EXPECT_EQ(device_cb_count, 0);
  EXPECT_EQ(reset_cb_count, 1);
  RunLoopUntilIdle();
  EXPECT_EQ(device_cb_count, 0);
  EXPECT_EQ(reset_cb_count, 1);
}

TEST_F(FidlControllerTest, ResetScoAlreadyPending) {
  RETURN_IF_FATAL(InitializeController());
  RunLoopUntilIdle();
  ASSERT_THAT(complete_status(), ::testing::Optional(PW_STATUS_OK));

  int device_cb_count = 0;
  hci_server()->set_reset_sco_callback(
      [&device_cb_count]() { device_cb_count++; });

  int config_cb_count_0 = 0;
  controller()->ConfigureSco(pw::bluetooth::Controller::ScoCodingFormat::kMsbc,
                             pw::bluetooth::Controller::ScoEncoding::k16Bits,
                             pw::bluetooth::Controller::ScoSampleRate::k16Khz,
                             [&](pw::Status status) {
                               config_cb_count_0++;
                               EXPECT_EQ(status, PW_STATUS_OK);
                             });
  EXPECT_EQ(config_cb_count_0, 1);

  int reset_cb_count_0 = 0;
  controller()->ResetSco([&](pw::Status status) {
    reset_cb_count_0++;
    EXPECT_EQ(status, PW_STATUS_OK);
  });
  int reset_cb_count_1 = 0;
  controller()->ResetSco([&](pw::Status status) {
    reset_cb_count_1++;
    EXPECT_EQ(status, PW_STATUS_ALREADY_EXISTS);
  });

  EXPECT_EQ(device_cb_count, 0);
  EXPECT_EQ(reset_cb_count_0, 0);
  EXPECT_EQ(reset_cb_count_1, 1);
  RunLoopUntilIdle();
  EXPECT_EQ(device_cb_count, 1);
  EXPECT_EQ(reset_cb_count_0, 1);
  EXPECT_EQ(reset_cb_count_1, 1);
}

}  // namespace bt::controllers
