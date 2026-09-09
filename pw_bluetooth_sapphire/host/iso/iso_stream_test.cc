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

#include "pw_bluetooth_sapphire/internal/host/iso/iso_stream.h"

#include "pw_bluetooth/hci_data.emb.h"
#include "pw_bluetooth_sapphire/internal/host/testing/controller_test.h"
#include "pw_bluetooth_sapphire/internal/host/testing/mock_controller.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_packets.h"
#include "pw_chrono/simulated_system_clock.h"

namespace bt::iso {
namespace {

using pw::bluetooth::emboss::IsoDataPacketStatus;
using pw::bluetooth::emboss::IsoDataPbFlag;

constexpr hci_spec::CigIdentifier kCigId = 0x22;
constexpr hci_spec::CisIdentifier kCisId = 0x42;

constexpr hci_spec::ConnectionHandle kCisHandleId = 0x59e;

constexpr size_t kMaxControllerSDUFragmentSize = 100;
constexpr size_t kMaxControllerPacketCount = 9;

constexpr size_t kSduHeaderSize = 4;

class MockConnection : public hci::IsoDataChannel::ConnectionInterface,
                       public WeakSelf<MockConnection> {
 public:
  MockConnection(hci::IsoDataChannel& iso_data_channel)
      : WeakSelf(this), iso_data_channel_(iso_data_channel) {}
  ~MockConnection() override = default;
  void Send(DynamicByteBuffer pdu) {
    send_queue_.emplace(std::move(pdu));
    iso_data_channel_.TrySendPackets();
  }

 private:
  void ReceiveInboundPacket(pw::span<const std::byte>) override {}
  std::optional<DynamicByteBuffer> GetNextOutboundPdu() override {
    if (send_queue_.empty()) {
      return std::nullopt;
    }
    DynamicByteBuffer pdu = std::move(send_queue_.front());
    send_queue_.pop();
    return pdu;
  }
  hci::IsoDataChannel& iso_data_channel_;
  std::queue<DynamicByteBuffer> send_queue_;
};

using MockControllerTestBase =
    bt::testing::FakeDispatcherControllerTest<bt::testing::MockController>;

class IsoStreamTest : public MockControllerTestBase {
 public:
  IsoStreamTest() = default;
  ~IsoStreamTest() override = default;

  void SetUp() override {
    MockControllerTestBase::SetUp(
        pw::bluetooth::Controller::FeaturesBits::kHciIso);
    hci::DataBufferInfo iso_buffer_info(kMaxControllerSDUFragmentSize,
                                        kMaxControllerPacketCount);
    transport()->InitializeIsoDataChannel(iso_buffer_info);
    iso_stream_ = IsoStream::Create(
        kCigId,
        kCisId,
        kCisHandleId,
        transport()->GetWeakPtr(),
        dispatcher(),
        /*on_established_cb=*/
        [this](pw::bluetooth::emboss::StatusCode status,
               std::optional<WeakSelf<IsoStream>::WeakPtr>,
               const std::optional<CisEstablishedParameters>& parameters) {
          ASSERT_FALSE(establishment_status_.has_value());
          establishment_status_ = status;
          established_parameters_ = parameters;
        },
        /*on_closed_cb=*/
        [this]() {
          ASSERT_FALSE(closed_);
          closed_ = true;
          transport()->iso_data_channel()->UnregisterConnection(kCisHandleId);
          iso_stream_.reset();
        },
        stream_lease_provider_);
  }

  void TearDown() override {
    RunUntilIdle();
    if (iso_stream_ &&
        establishment_status_ == pw::bluetooth::emboss::StatusCode::SUCCESS) {
      EXPECT_CMD_PACKET_OUT(test_device(),
                            testing::DisconnectPacket(kCisHandleId));
    }
    iso_stream_.reset();
    RunUntilIdle();
  }

 protected:
  // Send an HCI_LE_CIS_Established event with the provided status
  void EstablishCis(pw::bluetooth::emboss::StatusCode status);

  // Call IsoStream::SetupDataPath().
  // |cmd_complete_status| is nullopt if we do not expect an
  // LE_Setup_ISO_Data_Path command to be generated, otherwise it should be set
  // to the status code we want to generate in the response frame.
  // |expected_cb_result| should be set to the expected result of the callback
  // from IsoStream::SetupDataPath.
  void SetupDataPath(pw::bluetooth::emboss::DataPathDirection direction,
                     const std::optional<std::vector<uint8_t>>& codec_config,
                     const std::optional<pw::bluetooth::emboss::StatusCode>&
                         cmd_complete_status,
                     iso::IsoStream::SetupDataPathError expected_cb_result,
                     bool generate_mismatched_cid = false);

  void RegisterStream() {
    transport()->iso_data_channel()->RegisterConnection(
        kCisHandleId, iso_stream()->GetWeakPtr());
  }

  bool HandleCompleteIncomingSDU(const pw::span<const std::byte>& complete_sdu);

  IsoStream* iso_stream() { return iso_stream_.get(); }

  std::queue<std::vector<std::byte>>* complete_incoming_sdus() {
    return &complete_incoming_sdus_;
  }

  std::optional<pw::bluetooth::emboss::StatusCode> establishment_status() {
    return establishment_status_;
  }

  std::optional<CisEstablishedParameters> established_parameters() {
    return established_parameters_;
  }

  bool closed() { return closed_; }

  pw::bluetooth_sapphire::testing::FakeLeaseProvider& stream_lease_provider() {
    return stream_lease_provider_;
  }

  std::unique_ptr<IsoStream> CreateAndEstablishStream(
      hci_spec::ConnectionHandle handle, pw::Callback<void()> on_closed_cb) {
    auto stream = IsoStream::Create(
        kCigId,
        kCisId,
        handle,
        transport()->GetWeakPtr(),
        dispatcher(),
        /*on_established_cb=*/[](auto, auto, auto) {},
        /*on_closed_cb=*/
        [this, handle, cb = std::move(on_closed_cb)]() mutable {
          cb();
          transport()->iso_data_channel()->UnregisterConnection(handle);
        },
        stream_lease_provider());

    DynamicByteBuffer le_cis_established_packet =
        testing::LECisEstablishedEventPacket(
            pw::bluetooth::emboss::StatusCode::SUCCESS,
            handle,
            0x123456,
            0x7890ab,
            0x654321,
            0x0fedcb,
            pw::bluetooth::emboss::IsoPhyType::LE_2M,
            pw::bluetooth::emboss::IsoPhyType::LE_CODED,
            0x10,
            0x05,
            0x0f,
            0x01,
            0xff,
            0x0042,
            0x00fb,
            0x0222);
    test_device()->SendCommandChannelPacket(le_cis_established_packet);
    RunUntilIdle();

    transport()->iso_data_channel()->RegisterConnection(handle,
                                                        stream->GetWeakPtr());
    return stream;
  }

 protected:
  bool accept_incoming_sdus_ = true;

 private:
  std::unique_ptr<IsoStream> iso_stream_;
  std::optional<pw::bluetooth::emboss::StatusCode> establishment_status_;
  std::optional<CisEstablishedParameters> established_parameters_;
  std::queue<std::vector<std::byte>> complete_incoming_sdus_;
  bool closed_ = false;
  pw::bluetooth_sapphire::testing::FakeLeaseProvider stream_lease_provider_;
};

static DynamicByteBuffer LECisEstablishedPacketWithDefaultValues(
    pw::bluetooth::emboss::StatusCode status) {
  return testing::LECisEstablishedEventPacket(
      status,
      kCisHandleId,
      /*cig_sync_delay_us=*/0x123456,  // Must be in [0x0000ea, 0x7fffff]
      /*cis_sync_delay_us=*/0x7890ab,  // Must be in [0x0000ea, 0x7fffff]
      /*transport_latency_c_to_p_us=*/0x654321,  // Must be in [0x0000ea,
                                                 // 0x7fffff]
      /*transport_latency_p_to_c_us=*/0x0fedcb,  // Must be in [0x0000ea,
                                                 // 0x7fffff]
      /*phy_c_to_p=*/pw::bluetooth::emboss::IsoPhyType::LE_2M,
      /*phy_c_to_p=*/pw::bluetooth::emboss::IsoPhyType::LE_CODED,
      /*nse=*/0x10,               // Must be in [0x01, 0x1f]
      /*bn_c_to_p=*/0x05,         // Must be in [0x00, 0x0f]
      /*bn_p_to_c=*/0x0f,         // Must be in [0x00, 0x0f]
      /*ft_c_to_p=*/0x01,         // Must be in [0x01, 0xff]
      /*ft_p_to_c=*/0xff,         // Must be in [0x01, 0xff]
      /*max_pdu_c_to_p=*/0x0042,  // Must be in [0x0000, 0x00fb]
      /*max_pdu_p_to_c=*/0x00fb,  // Must be in [0x0000, 0x00fb]
      /*iso_interval=*/0x0222     // Must be in [0x0004, 0x0c80]
  );
}

void IsoStreamTest::EstablishCis(pw::bluetooth::emboss::StatusCode status) {
  DynamicByteBuffer le_cis_established_packet =
      LECisEstablishedPacketWithDefaultValues(status);
  test_device()->SendCommandChannelPacket(le_cis_established_packet);
  RunUntilIdle();
  ASSERT_TRUE(establishment_status().has_value());
  EXPECT_EQ(*(establishment_status()), status);
  if (status == pw::bluetooth::emboss::StatusCode::SUCCESS) {
    EXPECT_TRUE(established_parameters().has_value());
  } else {
    EXPECT_FALSE(established_parameters().has_value());
  }
}

bt::StaticPacket<pw::bluetooth::emboss::CodecIdWriter> GenerateCodecId() {
  const uint16_t kCompanyId = 0x1234;
  bt::StaticPacket<pw::bluetooth::emboss::CodecIdWriter> codec_id;
  auto codec_id_view = codec_id.view();
  codec_id_view.coding_format().Write(pw::bluetooth::emboss::CodingFormat::LC3);
  codec_id_view.company_id().Write(kCompanyId);
  return codec_id;
}

void IsoStreamTest::SetupDataPath(
    pw::bluetooth::emboss::DataPathDirection direction,
    const std::optional<std::vector<uint8_t>>& codec_configuration,
    const std::optional<pw::bluetooth::emboss::StatusCode>& cmd_complete_status,
    iso::IsoStream::SetupDataPathError expected_cb_result,
    bool generate_mismatched_cid) {
  const uint32_t kControllerDelay = 1234;  // Must be < 4000000
  std::optional<iso::IsoStream::SetupDataPathError> actual_cb_result;

  if (cmd_complete_status.has_value()) {
    auto setup_data_path_packet =
        testing::LESetupIsoDataPathPacket(kCisHandleId,
                                          direction,
                                          /*HCI*/ 0,
                                          GenerateCodecId(),
                                          kControllerDelay,
                                          codec_configuration);
    hci_spec::ConnectionHandle cis_handle =
        kCisHandleId + (generate_mismatched_cid ? 1 : 0);
    auto setup_data_path_complete_packet =
        testing::LESetupIsoDataPathResponse(*cmd_complete_status, cis_handle);
    EXPECT_CMD_PACKET_OUT(test_device(),
                          setup_data_path_packet,
                          &setup_data_path_complete_packet);
  }

  iso_stream()->SetupDataPath(
      direction,
      GenerateCodecId(),
      codec_configuration,
      kControllerDelay,
      [&actual_cb_result](iso::IsoStream::SetupDataPathError result) {
        actual_cb_result = result;
      },
      fit::bind_member<&IsoStreamTest::HandleCompleteIncomingSDU>(this));
  RunUntilIdle();
  ASSERT_TRUE(actual_cb_result.has_value());
  EXPECT_EQ(expected_cb_result, *actual_cb_result);
}

bool IsoStreamTest::HandleCompleteIncomingSDU(
    const pw::span<const std::byte>& sdu) {
  if (!accept_incoming_sdus_) {
    return false;
  }
  complete_incoming_sdus_.emplace(sdu.begin(), sdu.end());
  return true;
}

TEST_F(IsoStreamTest, CisEstablishedSuccessfully) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
}

TEST_F(IsoStreamTest, CisEstablishmentFailed) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::MEMORY_CAPACITY_EXCEEDED);
}

TEST_F(IsoStreamTest, ClosedCallsCloseCallback) {
  EXPECT_FALSE(closed());
  iso_stream()->Close();
  EXPECT_TRUE(closed());
}

TEST_F(IsoStreamTest, SetupDataPathSuccessfully) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
  SetupDataPath(pw::bluetooth::emboss::DataPathDirection::OUTPUT,
                /*codec_configuration=*/std::nullopt,
                pw::bluetooth::emboss::StatusCode::SUCCESS,
                iso::IsoStream::SetupDataPathError::kSuccess);
}

TEST_F(IsoStreamTest, SetupDataPathBeforeCisEstablished) {
  SetupDataPath(pw::bluetooth::emboss::DataPathDirection::OUTPUT,
                /*codec_configuration=*/std::nullopt,
                /*cmd_complete_status=*/std::nullopt,
                iso::IsoStream::SetupDataPathError::kCisNotEstablished);
}

TEST_F(IsoStreamTest, SetupInputDataPathTwice) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
  SetupDataPath(
      pw::bluetooth::emboss::DataPathDirection::INPUT,
      /*codec_configuration=*/std::nullopt,
      /*cmd_complete_status=*/pw::bluetooth::emboss::StatusCode::SUCCESS,
      iso::IsoStream::SetupDataPathError::kSuccess);
  SetupDataPath(pw::bluetooth::emboss::DataPathDirection::INPUT,
                /*codec_configuration=*/std::nullopt,
                /*cmd_complete_status=*/std::nullopt,
                iso::IsoStream::SetupDataPathError::kStreamAlreadyExists);
}

TEST_F(IsoStreamTest, SetupOutputDataPathTwice) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
  SetupDataPath(
      pw::bluetooth::emboss::DataPathDirection::OUTPUT,
      /*codec_configuration=*/std::nullopt,
      /*cmd_complete_status=*/pw::bluetooth::emboss::StatusCode::SUCCESS,
      iso::IsoStream::SetupDataPathError::kSuccess);
  SetupDataPath(pw::bluetooth::emboss::DataPathDirection::OUTPUT,
                /*codec_configuration=*/std::nullopt,
                /*cmd_complete_status=*/std::nullopt,
                iso::IsoStream::SetupDataPathError::kStreamAlreadyExists);
}

TEST_F(IsoStreamTest, SetupBothInputAndOutputDataPaths) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
  SetupDataPath(
      pw::bluetooth::emboss::DataPathDirection::OUTPUT,
      /*codec_configuration=*/std::nullopt,
      /*cmd_complete_status=*/pw::bluetooth::emboss::StatusCode::SUCCESS,
      iso::IsoStream::SetupDataPathError::kSuccess);
  SetupDataPath(
      pw::bluetooth::emboss::DataPathDirection::INPUT,
      /*codec_configuration=*/std::nullopt,
      /*cmd_complete_status=*/pw::bluetooth::emboss::StatusCode::SUCCESS,
      iso::IsoStream::SetupDataPathError::kSuccess);
}

TEST_F(IsoStreamTest, SetupDataPathInvalidArgs) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
  SetupDataPath(static_cast<pw::bluetooth::emboss::DataPathDirection>(250),
                /*codec_configuration=*/std::nullopt,
                /*cmd_complete_status=*/std::nullopt,
                iso::IsoStream::SetupDataPathError::kInvalidArgs);
}

TEST_F(IsoStreamTest, SetupDataPathWithCodecConfig) {
  std::vector<uint8_t> codec_config{5, 6, 7, 8};
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
  SetupDataPath(
      pw::bluetooth::emboss::DataPathDirection::OUTPUT,
      codec_config,
      /*cmd_complete_status=*/pw::bluetooth::emboss::StatusCode::SUCCESS,
      iso::IsoStream::SetupDataPathError::kSuccess);
}

// If the connection ID doesn't match in the command complete packet, fail
TEST_F(IsoStreamTest, SetupDataPathHandleMismatch) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
  SetupDataPath(
      pw::bluetooth::emboss::DataPathDirection::INPUT,
      /*codec_configuration=*/std::nullopt,
      /*cmd_complete_status=*/pw::bluetooth::emboss::StatusCode::SUCCESS,
      iso::IsoStream::SetupDataPathError::kStreamRejectedByController,
      /*generate_mismatched_cid=*/true);
}

TEST_F(IsoStreamTest, SetupDataPathControllerError) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
  SetupDataPath(
      pw::bluetooth::emboss::DataPathDirection::INPUT,
      /*codec_configuration=*/std::nullopt,
      /*cmd_complete_status=*/
      pw::bluetooth::emboss::StatusCode::CONNECTION_ALREADY_EXISTS,
      iso::IsoStream::SetupDataPathError::kStreamRejectedByController);
}

// If the client asks for frames before any are ready it will receive
// a notification when the next packet arrives.
TEST_F(IsoStreamTest, PendingRead) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
  SetupDataPath(
      pw::bluetooth::emboss::DataPathDirection::OUTPUT,
      /*codec_configuration=*/std::nullopt,
      /*cmd_complete_status=*/pw::bluetooth::emboss::StatusCode::SUCCESS,
      iso::IsoStream::SetupDataPathError::kSuccess);
  EXPECT_EQ(stream_lease_provider().lease_count(), 0u);
  const size_t kIsoSduLength = 212;
  std::vector<uint8_t> sdu_data =
      testing::GenDataBlob(kIsoSduLength, /*starting_value=*/14);
  DynamicByteBuffer packet0 = testing::IsoDataPacket(
      /*connection_handle=*/iso_stream()->cis_handle(),
      pw::bluetooth::emboss::IsoDataPbFlag::COMPLETE_SDU,
      /*time_stamp=*/std::nullopt,
      /*packet_sequence_number=*/0,
      /*iso_sdu_length=*/kIsoSduLength,
      pw::bluetooth::emboss::IsoDataPacketStatus::VALID_DATA,
      sdu_data);
  pw::span<const std::byte> packet0_span = packet0.subspan();
  ASSERT_FALSE(iso_stream()->ReadNextQueuedIncomingPacket());
  iso_stream()->ReceiveInboundPacket(packet0_span);
  ASSERT_EQ(complete_incoming_sdus()->size(), 1u);
  std::vector<std::byte>& received_frame = complete_incoming_sdus()->front();
  ASSERT_EQ(packet0_span.size(), received_frame.size());
  EXPECT_TRUE(std::equal(
      packet0_span.begin(), packet0_span.end(), received_frame.begin()));
  EXPECT_EQ(stream_lease_provider().lease_count(), 0u);
}

// If the client does not ask for frames it will not receive any notifications
// and the IsoStream will just queue them up.
TEST_F(IsoStreamTest, UnreadData) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
  SetupDataPath(
      pw::bluetooth::emboss::DataPathDirection::OUTPUT,
      /*codec_configuration=*/std::nullopt,
      /*cmd_complete_status=*/pw::bluetooth::emboss::StatusCode::SUCCESS,
      iso::IsoStream::SetupDataPathError::kSuccess);
  EXPECT_EQ(stream_lease_provider().lease_count(), 0u);
  const size_t kTotalFrameCount = 5;
  for (size_t i = 0; i < kTotalFrameCount; i++) {
    std::vector<uint8_t> sdu_data = testing::GenDataBlob(
        /*size=*/kMaxControllerSDUFragmentSize - i, /*starting_value=*/i);
    DynamicByteBuffer packet = testing::IsoDataPacket(
        /*connection_handle=*/iso_stream()->cis_handle(),
        pw::bluetooth::emboss::IsoDataPbFlag::COMPLETE_SDU,
        /*time_stamp=*/std::nullopt,
        /*packet_sequence_number=*/i,
        /*iso_sdu_length=*/kMaxControllerSDUFragmentSize - i,
        pw::bluetooth::emboss::IsoDataPacketStatus::VALID_DATA,
        sdu_data);
    iso_stream()->ReceiveInboundPacket(packet.subspan());
    EXPECT_NE(stream_lease_provider().lease_count(), 0u);
  }
  EXPECT_EQ(complete_incoming_sdus()->size(), 0u);
}

// This is the (somewhat unusual) case where the client asks for a frame but
// then rejects it when the frame is ready. The frame should stay in the queue
// and future frames should not receive notification, either.
TEST_F(IsoStreamTest, ReadRequestedAndThenRejected) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
  SetupDataPath(
      pw::bluetooth::emboss::DataPathDirection::OUTPUT,
      /*codec_configuration=*/std::nullopt,
      /*cmd_complete_status=*/pw::bluetooth::emboss::StatusCode::SUCCESS,
      iso::IsoStream::SetupDataPathError::kSuccess);
  size_t sdu_0_size = kMaxControllerSDUFragmentSize;
  std::vector<uint8_t> sdu_0 =
      testing::GenDataBlob(sdu_0_size, /*starting_value=*/11);
  DynamicByteBuffer packet0 = testing::IsoDataPacket(
      /*connection_handle=*/iso_stream()->cis_handle(),
      pw::bluetooth::emboss::IsoDataPbFlag::COMPLETE_SDU,
      /*time_stamp=*/std::nullopt,
      /*packet_sequence_number=*/0,
      /*iso_sdu_length=*/sdu_0_size,
      pw::bluetooth::emboss::IsoDataPacketStatus::VALID_DATA,
      sdu_0);
  pw::span<const std::byte> packet0_span = packet0.subspan();
  size_t sdu_1_size = sdu_0_size - 1;
  std::vector<uint8_t> sdu_1 =
      testing::GenDataBlob(sdu_1_size, /*starting_value=*/13);
  DynamicByteBuffer packet1 = testing::IsoDataPacket(
      /*connection_handle=*/iso_stream()->cis_handle(),
      pw::bluetooth::emboss::IsoDataPbFlag::COMPLETE_SDU,
      /*time_stamp=*/std::nullopt,
      /*packet_sequence_number=*/1,
      /*iso_sdu_length=*/sdu_1_size,
      pw::bluetooth::emboss::IsoDataPacketStatus::VALID_DATA,
      sdu_1);
  pw::span<const std::byte> packet1_span = packet1.subspan();

  // Request a frame but then reject it when proffered by the stream
  ASSERT_FALSE(iso_stream()->ReadNextQueuedIncomingPacket());
  accept_incoming_sdus_ = false;
  iso_stream()->ReceiveInboundPacket(packet0_span);
  EXPECT_EQ(complete_incoming_sdus()->size(), 0u);

  // Accept future frames, but because no read request has been made that we
  // couldn't fulfill, the stream should just queue them up.
  accept_incoming_sdus_ = true;
  iso_stream()->ReceiveInboundPacket(packet1_span);
  EXPECT_EQ(complete_incoming_sdus()->size(), 0u);

  // And finally, we should be able to read out the packets in the right order
  std::optional<IsoDataPacket> rx_packet_0 =
      iso_stream()->ReadNextQueuedIncomingPacket();
  ASSERT_TRUE(rx_packet_0);
  ASSERT_EQ(rx_packet_0->size(), packet0_span.size());
  ASSERT_TRUE(std::equal(
      rx_packet_0->begin(), rx_packet_0->end(), packet0_span.begin()));
  std::optional<IsoDataPacket> rx_packet_1 =
      iso_stream()->ReadNextQueuedIncomingPacket();
  ASSERT_TRUE(rx_packet_1);
  ASSERT_EQ(rx_packet_1->size(), packet1_span.size());
  ASSERT_TRUE(std::equal(
      rx_packet_1->begin(), rx_packet_1->end(), packet1_span.begin()));

  // Stream's packet queue should be empty now
  ASSERT_FALSE(iso_stream()->ReadNextQueuedIncomingPacket());
}

// Bad packets will not be passed on
TEST_F(IsoStreamTest, BadPacket) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
  SetupDataPath(
      pw::bluetooth::emboss::DataPathDirection::OUTPUT,
      /*codec_configuration=*/std::nullopt,
      /*cmd_complete_status=*/pw::bluetooth::emboss::StatusCode::SUCCESS,
      iso::IsoStream::SetupDataPathError::kSuccess);
  const size_t kIsoSduLength = 212;
  std::vector<uint8_t> sdu =
      testing::GenDataBlob(kIsoSduLength, /*starting_value=*/91);
  DynamicByteBuffer packet0 = testing::IsoDataPacket(
      /*connection_handle=*/iso_stream()->cis_handle(),
      pw::bluetooth::emboss::IsoDataPbFlag::COMPLETE_SDU,
      /*time_stamp=*/std::nullopt,
      /*packet_sequence_number=*/0,
      kIsoSduLength,
      pw::bluetooth::emboss::IsoDataPacketStatus::VALID_DATA,
      sdu);
  // Improperly formatted packets are discarded. To test this, we'll remove the
  // last byte of SDU data before we send it.
  pw::span<const std::byte> packet0_as_span =
      packet0.subspan(0, packet0.size() - 1);
  ASSERT_FALSE(iso_stream()->ReadNextQueuedIncomingPacket());
  iso_stream()->ReceiveInboundPacket(packet0_as_span);
  ASSERT_EQ(complete_incoming_sdus()->size(), 0u);
  EXPECT_EQ(stream_lease_provider().lease_count(), 0u);
}

// Extra data at the end of the frame will be removed
TEST_F(IsoStreamTest, ExcessDataIsTruncated) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
  SetupDataPath(
      pw::bluetooth::emboss::DataPathDirection::OUTPUT,
      /*codec_configuration=*/std::nullopt,
      /*cmd_complete_status=*/pw::bluetooth::emboss::StatusCode::SUCCESS,
      iso::IsoStream::SetupDataPathError::kSuccess);
  const size_t kIsoSduLength = 212;
  std::vector<uint8_t> sdu =
      testing::GenDataBlob(kIsoSduLength, /*starting_value=*/91);
  DynamicByteBuffer packet0 = testing::IsoDataPacket(
      /*connection_handle=*/iso_stream()->cis_handle(),
      pw::bluetooth::emboss::IsoDataPbFlag::COMPLETE_SDU,
      /*time_stamp=*/std::nullopt,
      /*packet_sequence_number=*/0,
      /*iso_sdu_length=*/kIsoSduLength,
      pw::bluetooth::emboss::IsoDataPacketStatus::VALID_DATA,
      sdu);
  size_t original_packet0_size = packet0.size();
  packet0.expand(packet0.size() + 100);
  ASSERT_FALSE(iso_stream()->ReadNextQueuedIncomingPacket());
  iso_stream()->ReceiveInboundPacket(packet0.subspan());
  ASSERT_EQ(complete_incoming_sdus()->size(), 1u);
  EXPECT_EQ(complete_incoming_sdus()->front().size(), original_packet0_size);
}

// Ensure sent packets are structured and sent correctly, including
// fragmentation.
TEST_F(IsoStreamTest, SendPacket) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
  SetupDataPath(
      pw::bluetooth::emboss::DataPathDirection::OUTPUT,
      /*codec_configuration=*/std::nullopt,
      /*cmd_complete_status=*/pw::bluetooth::emboss::StatusCode::SUCCESS,
      iso::IsoStream::SetupDataPathError::kSuccess);
  RegisterStream();
  EXPECT_EQ(stream_lease_provider().lease_count(), 0u);

  uint32_t iso_interval_usec =
      established_parameters()->iso_interval *
      CisEstablishedParameters::kIsoIntervalToMicroseconds;

  constexpr size_t kMaxFirstPacketSize =
      kMaxControllerSDUFragmentSize - kSduHeaderSize;

  uint16_t expected_sequence_num = 0;

  {
    std::vector<uint8_t> sdu =
        testing::GenDataBlob(kMaxFirstPacketSize / 2, /*starting_value=*/111);
    EXPECT_ISO_PACKET_OUT(
        test_device(),
        testing::IsoDataPacket(
            /*connection_handle = */ iso_stream()->cis_handle(),
            /*pb_flag = */ IsoDataPbFlag::COMPLETE_SDU,
            /*timestamp = */ std::nullopt,
            /*sequence_number = */ expected_sequence_num,
            /*iso_sdu_length = */ sdu.size(),
            /*status_flag = */ IsoDataPacketStatus::VALID_DATA,
            sdu));

    iso_stream()->Send(pw::as_bytes(pw::span(sdu)));
    // The stream lease will be acquired but quickly dropped.
    EXPECT_EQ(stream_lease_provider().lease_count(), 0u);
    EXPECT_NE(lease_provider().lease_count(), 0u);
    RunUntilIdle();
    EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());

    ++expected_sequence_num;

    // Advance the clock by one ISO interval
    RunFor(std::chrono::microseconds(iso_interval_usec));
  }

  {
    // Send a packet that fits exactly within one ISO buffer easily.
    std::vector<uint8_t> sdu =
        testing::GenDataBlob(kMaxFirstPacketSize, /*starting_value=*/7);
    EXPECT_ISO_PACKET_OUT(
        test_device(),
        testing::IsoDataPacket(
            /*connection_handle = */ iso_stream()->cis_handle(),
            /*pb_flag = */ IsoDataPbFlag::COMPLETE_SDU,
            /*timestamp = */ std::nullopt,
            /*sequence_number = */ expected_sequence_num,
            /*iso_sdu_length = */ sdu.size(),
            /*status_flag = */ IsoDataPacketStatus::VALID_DATA,
            sdu));

    iso_stream()->Send(pw::as_bytes(pw::span(sdu)));
    RunUntilIdle();
    EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());

    ++expected_sequence_num;

    // Advance the clock by one ISO interval
    RunFor(std::chrono::microseconds(iso_interval_usec));
  }

  {
    // Send a packet that has to be split into three.
    std::vector<uint8_t> sdu = testing::GenDataBlob(
        kMaxFirstPacketSize + kMaxControllerSDUFragmentSize * 2,
        /*starting_value=*/42);
    EXPECT_ISO_PACKET_OUT(
        test_device(),
        testing::IsoDataPacket(
            /*connection_handle = */ iso_stream()->cis_handle(),
            /*pb_flag = */ IsoDataPbFlag::FIRST_FRAGMENT,
            /*timestamp = */ std::nullopt,
            /*sequence_number = */ expected_sequence_num,
            /*sdu_length = */ sdu.size(),
            /*status_flag = */ IsoDataPacketStatus::VALID_DATA,
            pw::span(sdu).subspan(0, kMaxFirstPacketSize)));
    EXPECT_ISO_PACKET_OUT(
        test_device(),
        testing::IsoDataPacket(
            /*connection_handle = */ iso_stream()->cis_handle(),
            /*pb_flag = */ IsoDataPbFlag::INTERMEDIATE_FRAGMENT,
            /*timestamp = */ std::nullopt,
            /*sequence_number = */ std::nullopt,
            /*sdu_length = */ std::nullopt,
            /*status_flag = */ std::nullopt,
            pw::span(sdu).subspan(kMaxFirstPacketSize,
                                  kMaxControllerSDUFragmentSize)));
    EXPECT_ISO_PACKET_OUT(
        test_device(),
        testing::IsoDataPacket(
            /*connection_handle = */ iso_stream()->cis_handle(),
            /*pb_flag = */ IsoDataPbFlag::LAST_FRAGMENT,
            /*timestamp = */ std::nullopt,
            /*sequence_number = */ std::nullopt,
            /*sdu_length = */ std::nullopt,
            /*status_flag = */ std::nullopt,
            pw::span(sdu).subspan(kMaxFirstPacketSize +
                                  kMaxControllerSDUFragmentSize)));

    iso_stream()->Send(pw::as_bytes(pw::span(sdu)));
    RunUntilIdle();
    EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());

    expected_sequence_num += 2;

    // Advance the clock by two ISO intervals to simulate a skipped interval
    RunFor(std::chrono::microseconds(2 * iso_interval_usec));
  }

  {
    // Send a packet that just barely needs to be split into three fragments.
    std::vector<uint8_t> sdu = testing::GenDataBlob(
        kMaxFirstPacketSize + kMaxControllerSDUFragmentSize + 1,
        /*starting_value=*/77);
    EXPECT_ISO_PACKET_OUT(
        test_device(),
        testing::IsoDataPacket(
            /*connection_handle = */ iso_stream()->cis_handle(),
            /*pb_flag = */ IsoDataPbFlag::FIRST_FRAGMENT,
            /*timestamp = */ std::nullopt,
            /*sequence_number = */ expected_sequence_num,
            /*sdu_length = */ sdu.size(),
            /*status_flag = */ IsoDataPacketStatus::VALID_DATA,
            pw::span(sdu).subspan(0, kMaxFirstPacketSize)));
    EXPECT_ISO_PACKET_OUT(
        test_device(),
        testing::IsoDataPacket(
            /*connection_handle = */ iso_stream()->cis_handle(),
            /*pb_flag = */ IsoDataPbFlag::INTERMEDIATE_FRAGMENT,
            /*timestamp = */ std::nullopt,
            /*sequence_number = */ std::nullopt,
            /*sdu_length = */ std::nullopt,
            /*status_flag = */ std::nullopt,
            pw::span(sdu).subspan(kMaxFirstPacketSize,
                                  kMaxControllerSDUFragmentSize)));
    EXPECT_ISO_PACKET_OUT(
        test_device(),
        testing::IsoDataPacket(
            /*connection_handle = */ iso_stream()->cis_handle(),
            /*pb_flag = */ IsoDataPbFlag::LAST_FRAGMENT,
            /*timestamp = */ std::nullopt,
            /*sequence_number = */ std::nullopt,
            /*sdu_length = */ std::nullopt,
            /*status_flag = */ std::nullopt,
            pw::span(sdu).subspan(kMaxFirstPacketSize +
                                  kMaxControllerSDUFragmentSize)));

    iso_stream()->Send(pw::as_bytes(pw::span(sdu)));
    RunUntilIdle();
    EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());
  }

  // Only IsoDataChannel should be holding a lease now.
  EXPECT_EQ(stream_lease_provider().lease_count(), 0u);
  EXPECT_NE(lease_provider().lease_count(), 0u);
}

TEST_F(IsoStreamTest, PacketReceivedBeforeNextInterval) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
  SetupDataPath(
      pw::bluetooth::emboss::DataPathDirection::OUTPUT,
      /*codec_configuration=*/std::nullopt,
      /*cmd_complete_status=*/pw::bluetooth::emboss::StatusCode::SUCCESS,
      iso::IsoStream::SetupDataPathError::kSuccess);
  RegisterStream();

  uint32_t iso_interval_usec =
      established_parameters()->iso_interval *
      CisEstablishedParameters::kIsoIntervalToMicroseconds;

  constexpr size_t kMaxFirstPacketSize =
      kMaxControllerSDUFragmentSize - kSduHeaderSize;

  uint16_t expected_sequence_num = 0;

  // Send the first packet
  {
    std::vector<uint8_t> sdu =
        testing::GenDataBlob(kMaxFirstPacketSize / 2, /*starting_value=*/111);

    EXPECT_ISO_PACKET_OUT(
        test_device(),
        testing::IsoDataPacket(
            /*connection_handle = */ iso_stream()->cis_handle(),
            /*pb_flag = */ IsoDataPbFlag::COMPLETE_SDU,
            /*timestamp = */ std::nullopt,
            /*sequence_number = */ expected_sequence_num,
            /*iso_sdu_length = */ sdu.size(),
            /*status_flag = */ IsoDataPacketStatus::VALID_DATA,
            sdu));

    iso_stream()->Send(pw::as_bytes(pw::span(sdu)));
    RunUntilIdle();
    EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());
    ++expected_sequence_num;
  }

  // Send a second packet immediately before the next interval is produced
  {
    std::vector<uint8_t> sdu =
        testing::GenDataBlob(kMaxFirstPacketSize / 3, /*starting_value=*/55);

    EXPECT_ISO_PACKET_OUT(
        test_device(),
        testing::IsoDataPacket(
            /*connection_handle = */ iso_stream()->cis_handle(),
            /*pb_flag = */ IsoDataPbFlag::COMPLETE_SDU,
            /*timestamp = */ std::nullopt,
            /*sequence_number = */ expected_sequence_num,
            /*iso_sdu_length = */ sdu.size(),
            /*status_flag = */ IsoDataPacketStatus::VALID_DATA,
            sdu));

    iso_stream()->Send(pw::as_bytes(pw::span(sdu)));
    RunUntilIdle();
    EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());
    ++expected_sequence_num;

    RunFor(std::chrono::microseconds(iso_interval_usec));
  }

  // Send a third packet after the interval has passed
  {
    std::vector<uint8_t> sdu =
        testing::GenDataBlob(kMaxFirstPacketSize / 4, /*starting_value=*/99);

    // Expect the sequence number to have advanced to the next interval
    EXPECT_ISO_PACKET_OUT(
        test_device(),
        testing::IsoDataPacket(
            /*connection_handle = */ iso_stream()->cis_handle(),
            /*pb_flag = */ IsoDataPbFlag::COMPLETE_SDU,
            /*timestamp = */ std::nullopt,
            /*sequence_number = */ expected_sequence_num,
            /*iso_sdu_length = */ sdu.size(),
            /*status_flag = */ IsoDataPacketStatus::VALID_DATA,
            sdu));

    iso_stream()->Send(pw::as_bytes(pw::span(sdu)));
    RunUntilIdle();
    EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());
  }
}

TEST_F(IsoStreamTest, PacketReceivedAtIntervalBoundaries) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
  SetupDataPath(
      pw::bluetooth::emboss::DataPathDirection::OUTPUT,
      /*codec_configuration=*/std::nullopt,
      /*cmd_complete_status=*/pw::bluetooth::emboss::StatusCode::SUCCESS,
      iso::IsoStream::SetupDataPathError::kSuccess);
  RegisterStream();

  uint32_t iso_interval_usec =
      established_parameters()->iso_interval *
      CisEstablishedParameters::kIsoIntervalToMicroseconds;

  constexpr size_t kMaxFirstPacketSize =
      kMaxControllerSDUFragmentSize - kSduHeaderSize;

  uint16_t expected_sequence_num = 0;

  {
    std::vector<uint8_t> sdu =
        testing::GenDataBlob(kMaxFirstPacketSize / 2, /*starting_value=*/111);
    EXPECT_ISO_PACKET_OUT(
        test_device(),
        testing::IsoDataPacket(
            /*connection_handle = */ iso_stream()->cis_handle(),
            /*pb_flag = */ IsoDataPbFlag::COMPLETE_SDU,
            /*timestamp = */ std::nullopt,
            /*sequence_number = */ expected_sequence_num,
            /*iso_sdu_length = */ sdu.size(),
            /*status_flag = */ IsoDataPacketStatus::VALID_DATA,
            sdu));

    iso_stream()->Send(pw::as_bytes(pw::span(sdu)));
    RunUntilIdle();
    EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());

    ++expected_sequence_num;

    // Advance the clock by almost one ISO interval
    RunFor(std::chrono::microseconds(iso_interval_usec - 1));
  }

  {
    // Send a packet one tick before the next interval
    std::vector<uint8_t> sdu =
        testing::GenDataBlob(kMaxFirstPacketSize, /*starting_value=*/7);
    EXPECT_ISO_PACKET_OUT(
        test_device(),
        testing::IsoDataPacket(
            /*connection_handle = */ iso_stream()->cis_handle(),
            /*pb_flag = */ IsoDataPbFlag::COMPLETE_SDU,
            /*timestamp = */ std::nullopt,
            /*sequence_number = */ expected_sequence_num,
            /*iso_sdu_length = */ sdu.size(),
            /*status_flag = */ IsoDataPacketStatus::VALID_DATA,
            sdu));

    iso_stream()->Send(pw::as_bytes(pw::span(sdu)));
    RunUntilIdle();
    EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());

    // Advance the clock by one more microsecond to reach the next interval
    RunFor(std::chrono::microseconds(1));
    ++expected_sequence_num;
  }

  // Send a third packet after the interval has passed
  {
    std::vector<uint8_t> sdu =
        testing::GenDataBlob(kMaxFirstPacketSize / 4, /*starting_value=*/99);

    // Expect the sequence number to have advanced to the next interval
    EXPECT_ISO_PACKET_OUT(
        test_device(),
        testing::IsoDataPacket(
            /*connection_handle = */ iso_stream()->cis_handle(),
            /*pb_flag = */ IsoDataPbFlag::COMPLETE_SDU,
            /*timestamp = */ std::nullopt,
            /*sequence_number = */ expected_sequence_num,
            /*iso_sdu_length = */ sdu.size(),
            /*status_flag = */ IsoDataPacketStatus::VALID_DATA,
            /*sdu_data = */ pw::span(sdu.data(), sdu.size())));

    iso_stream()->Send(pw::as_bytes(pw::span(sdu)));
    RunUntilIdle();
    EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());
  }
}

TEST_F(IsoStreamTest, ClearControllerPacketCountOnDisconnectComplete) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
  SetupDataPath(
      pw::bluetooth::emboss::DataPathDirection::OUTPUT,
      /*codec_config=*/std::nullopt,
      /*cmd_complete_status=*/pw::bluetooth::emboss::StatusCode::SUCCESS,
      iso::IsoStream::SetupDataPathError::kSuccess);
  RegisterStream();

  uint32_t iso_interval_usec =
      established_parameters()->iso_interval *
      CisEstablishedParameters::kIsoIntervalToMicroseconds;

  constexpr size_t kMaxFirstPacketSize =
      kMaxControllerSDUFragmentSize - kSduHeaderSize;

  uint16_t expected_sequence_num = 0;

  // FIll up controller buffer + 1 packet queued.
  for (size_t i = 0; i < kMaxControllerPacketCount; i++) {
    std::vector<uint8_t> sdu =
        testing::GenDataBlob(kMaxFirstPacketSize / 2, /*starting_value=*/i);
    EXPECT_ISO_PACKET_OUT(
        test_device(),
        testing::IsoDataPacket(iso_stream()->cis_handle(),
                               IsoDataPbFlag::COMPLETE_SDU,
                               std::nullopt,
                               expected_sequence_num,
                               sdu.size(),
                               IsoDataPacketStatus::VALID_DATA,
                               sdu));
    iso_stream()->Send(pw::as_bytes(pw::span(sdu)));
    RunUntilIdle();
    EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());
    ++expected_sequence_num;
    // Advance the clock by one ISO interval
    RunFor(std::chrono::microseconds(iso_interval_usec));
  }

  // Queue a packet on a different connection. IsoDataChannel should not send it
  // yet.
  const hci_spec::ConnectionHandle kIsoHandle2 = 0x0002;
  MockConnection connection(*transport()->iso_data_channel());
  transport()->iso_data_channel()->RegisterConnection(kIsoHandle2,
                                                      connection.GetWeakPtr());
  DynamicByteBuffer pdu(kMaxFirstPacketSize);
  pdu.Fill(3);
  EXPECT_ISO_PACKET_OUT(test_device(), pdu);
  connection.Send(std::move(pdu));
  RunUntilIdle();
  EXPECT_FALSE(test_device()->AllExpectedIsoPacketsSent());

  EXPECT_CMD_PACKET_OUT(test_device(), testing::DisconnectPacket(kCisHandleId));
  iso_stream()->Close();
  RunUntilIdle();
  EXPECT_FALSE(test_device()->AllExpectedIsoPacketsSent());
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());

  // The Disconnect Complete event should clear the pending packets in
  // IsoDataChannel, allowing the queued packet to be sent.
  test_device()->SendCommandChannelPacket(
      testing::DisconnectionCompletePacket(kCisHandleId));
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());

  transport()->iso_data_channel()->UnregisterConnection(kIsoHandle2);
}

TEST_F(IsoStreamTest, HoldWakeLeaseWhileTxPacketQueued) {
  EstablishCis(pw::bluetooth::emboss::StatusCode::SUCCESS);
  SetupDataPath(
      pw::bluetooth::emboss::DataPathDirection::OUTPUT,
      /*codec_config=*/std::nullopt,
      /*cmd_complete_status=*/pw::bluetooth::emboss::StatusCode::SUCCESS,
      iso::IsoStream::SetupDataPathError::kSuccess);
  RegisterStream();

  constexpr size_t kMaxFirstPacketSize =
      kMaxControllerSDUFragmentSize - kSduHeaderSize;

  // Fill up the controller buffer with packets from a different connection.
  const hci_spec::ConnectionHandle kIsoHandle2 = 0x0002;
  MockConnection connection(*transport()->iso_data_channel());
  transport()->iso_data_channel()->RegisterConnection(kIsoHandle2,
                                                      connection.GetWeakPtr());
  for (size_t i = 0; i < kMaxControllerPacketCount; ++i) {
    DynamicByteBuffer pdu(kMaxFirstPacketSize);
    pdu.Fill(3);
    EXPECT_ISO_PACKET_OUT(test_device(), pdu);
    connection.Send(std::move(pdu));
    RunUntilIdle();
    EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());
  }

  EXPECT_EQ(stream_lease_provider().lease_count(), 0u);
  std::vector<uint8_t> sdu =
      testing::GenDataBlob(kMaxFirstPacketSize / 2, /*starting_value=*/0);
  EXPECT_ISO_PACKET_OUT(test_device(),
                        testing::IsoDataPacket(iso_stream()->cis_handle(),
                                               IsoDataPbFlag::COMPLETE_SDU,
                                               std::nullopt,
                                               /*packet_sequence_number=*/0,
                                               sdu.size(),
                                               IsoDataPacketStatus::VALID_DATA,
                                               sdu));
  iso_stream()->Send(pw::as_bytes(pw::span(sdu)));
  RunUntilIdle();
  EXPECT_FALSE(test_device()->AllExpectedIsoPacketsSent());
  // A lease should be held while the packet is queued.
  EXPECT_NE(stream_lease_provider().lease_count(), 0u);

  // Clear the pending packets in IsoDataChannel, allowing the queued packet to
  // be sent.
  transport()->iso_data_channel()->UnregisterConnection(kIsoHandle2);
  transport()->iso_data_channel()->ClearControllerPacketCount(kIsoHandle2);
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedIsoPacketsSent());
  EXPECT_EQ(stream_lease_provider().lease_count(), 0u);
}

TEST_F(IsoStreamTest, CloseAfterPeerDisconnectedDoesNotCrash) {
  bool closed_callback_called = false;
  constexpr hci_spec::ConnectionHandle kCustomCisHandle = 0x600;
  auto custom_stream = CreateAndEstablishStream(
      kCustomCisHandle, [&]() { closed_callback_called = true; });

  // Simulate peer disconnection (Disconnection Complete event)
  test_device()->SendCommandChannelPacket(
      testing::DisconnectionCompletePacket(kCustomCisHandle));
  RunUntilIdle();

  EXPECT_TRUE(closed_callback_called);

  // Calling Close() now should NOT crash / assert on connection state.
  custom_stream->Close();
  RunUntilIdle();
}

TEST_F(IsoStreamTest, CloseMultipleTimesDuringDisconnectDoesNotCrash) {
  bool closed_callback_called = false;
  constexpr hci_spec::ConnectionHandle kCustomCisHandle = 0x601;
  auto custom_stream = CreateAndEstablishStream(
      kCustomCisHandle, [&]() { closed_callback_called = true; });

  // Calling Close() the first time should send a Disconnect command.
  EXPECT_CMD_PACKET_OUT(
      test_device(),
      testing::DisconnectPacket(kCustomCisHandle,
                                pw::bluetooth::emboss::StatusCode::
                                    REMOTE_USER_TERMINATED_CONNECTION));
  custom_stream->Close();
  RunUntilIdle();

  // Calling Close() a second time (before the disconnection completes) should
  // return early and NOT crash.
  custom_stream->Close();
  RunUntilIdle();

  // Now, simulate the disconnection complete event from the controller.
  test_device()->SendCommandChannelPacket(
      testing::DisconnectionCompletePacket(kCustomCisHandle));
  RunUntilIdle();

  EXPECT_TRUE(closed_callback_called);
}

}  // namespace
}  // namespace bt::iso
