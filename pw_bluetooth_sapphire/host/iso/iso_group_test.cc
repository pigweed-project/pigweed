// Copyright 2025 The Pigweed Authors
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

#include "pw_bluetooth_sapphire/internal/host/iso/iso_group.h"

#include "pw_bluetooth_sapphire/internal/host/iso/fake_iso_stream.h"
#include "pw_bluetooth_sapphire/internal/host/testing/controller_test.h"
#include "pw_bluetooth_sapphire/internal/host/testing/mock_controller.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_packets.h"

namespace bt::iso {
namespace {

using testing::FakeIsoStream;

// FakeCigStreamCreator
// Holds an unordered_map of `FakeIsoStream`s, accessible from outside
// with the const `streams()` accessor.
// Implements CigStreamCreator.
class FakeCigStreamCreator : public CigStreamCreator {
 public:
  FakeCigStreamCreator() : weak_self_(this) {}
  ~FakeCigStreamCreator() override = default;

  WeakSelf<IsoStream>::WeakPtr CreateCisConfiguration(
      CigCisIdentifier id,
      hci_spec::ConnectionHandle cis_handle,
      CisEstablishedCallback on_established_cb,
      pw::Callback<void()> on_closed_cb) override {
    auto stream = std::make_unique<FakeIsoStream>(
        cis_handle, std::move(on_established_cb), std::move(on_closed_cb));
    auto weak_stream = stream->GetWeakPtr();
    streams_.emplace(id, std::move(stream));
    return weak_stream;
  }

  const std::unordered_map<CigCisIdentifier, std::unique_ptr<FakeIsoStream>>&
  streams() const {
    return streams_;
  }

  CigStreamCreator::WeakPtr GetWeakPtr() { return weak_self_.GetWeakPtr(); }

 private:
  std::unordered_map<CigCisIdentifier, std::unique_ptr<FakeIsoStream>> streams_;
  WeakSelf<FakeCigStreamCreator> weak_self_;
};

class IsoGroupTest : public bt::testing::FakeDispatcherControllerTest<
                         bt::testing::MockController> {
 public:
  IsoGroupTest() = default;
  ~IsoGroupTest() override = default;

  void SetUp() override {
    bt::testing::FakeDispatcherControllerTest<
        bt::testing::MockController>::SetUp();
    cig_stream_creator_ = std::make_unique<FakeCigStreamCreator>();
  }

  void TearDown() override {
    cig_stream_creator_.reset();
    bt::testing::FakeDispatcherControllerTest<
        bt::testing::MockController>::TearDown();
  }

 protected:
  std::unique_ptr<FakeCigStreamCreator> cig_stream_creator_;
};

TEST_F(IsoGroupTest, CreateCig) {
  hci_spec::CigIdentifier kCigId = 0x01;
  bool on_closed_cb_called = false;
  auto cig =
      IsoGroup::CreateCig(kCigId,
                          transport()->GetWeakPtr(),
                          cig_stream_creator_->GetWeakPtr(),
                          [&](IsoGroup&) { on_closed_cb_called = true; });

  ASSERT_TRUE(cig);
  EXPECT_EQ(cig->id(), kCigId);
  EXPECT_FALSE(on_closed_cb_called);
  EXPECT_TRUE(cig_stream_creator_->streams().empty());
}

TEST_F(IsoGroupTest, SetParamsNoCisParams) {
  constexpr hci_spec::CigIdentifier kCigId = 0x99;
  constexpr uint32_t kSduIntervalCToP = 0x000F0E0D;
  constexpr uint32_t kSduIntervalPToC = 0x000C0B0A;
  constexpr uint16_t kMaxTransportLatencyCToP = 0x0102;
  constexpr uint16_t kMaxTransportLatencyPToC = 0x0304;

  auto cig = IsoGroup::CreateCig(kCigId,
                                 transport()->GetWeakPtr(),
                                 cig_stream_creator_->GetWeakPtr(),
                                 [](IsoGroup&) {});

  CigParams cig_params = {
      .sdu_interval_c_to_p = kSduIntervalCToP,
      .sdu_interval_p_to_c = kSduIntervalPToC,
      .packing = CigPacking::kSequential,
      .framing = CigFraming::kUnframed,
      .max_transport_latency_c_to_p = kMaxTransportLatencyCToP,
      .max_transport_latency_p_to_c = kMaxTransportLatencyPToC,
      .worst_case_sca =
          pw::bluetooth::emboss::LESleepClockAccuracyRange::PPM_0_TO_20,
  };

  bool callback_called = false;
  cig->SetParams(
      std::move(cig_params), {}, [&](IsoGroup::SetParamsResult result) {
        callback_called = true;
        EXPECT_TRUE(result.has_value());
      });

  EXPECT_CMD_PACKET_OUT(
      test_device(),
      bt::testing::LESetCIGParametersCommandPacket(
          kCigId,
          kSduIntervalCToP,
          kSduIntervalPToC,
          pw::bluetooth::emboss::LESleepClockAccuracyRange::PPM_0_TO_20,
          pw::bluetooth::emboss::LECISPacking::SEQUENTIAL,
          pw::bluetooth::emboss::LECISFraming::UNFRAMED,
          kMaxTransportLatencyCToP,
          kMaxTransportLatencyPToC,
          {}));
  RunUntilIdle();

  EXPECT_FALSE(callback_called);

  test_device()->SendCommandChannelPacket(
      bt::testing::LESetCIGParametersCompletePacket(kCigId, {}));
  RunUntilIdle();

  EXPECT_TRUE(callback_called);
  EXPECT_TRUE(cig_stream_creator_->streams().empty());
}

TEST_F(IsoGroupTest, SetParamsWorstCaseScaDefault) {
  constexpr hci_spec::CigIdentifier kCigId = 0x99;
  constexpr uint32_t kSduIntervalCToP = 0x000F0E0D;
  constexpr uint32_t kSduIntervalPToC = 0x000C0B0A;
  constexpr uint16_t kMaxTransportLatencyCToP = 0x0102;
  constexpr uint16_t kMaxTransportLatencyPToC = 0x0304;

  auto cig = IsoGroup::CreateCig(kCigId,
                                 transport()->GetWeakPtr(),
                                 cig_stream_creator_->GetWeakPtr(),
                                 [](IsoGroup&) {});

  CigParams cig_params = {
      .sdu_interval_c_to_p = kSduIntervalCToP,
      .sdu_interval_p_to_c = kSduIntervalPToC,
      .packing = CigPacking::kSequential,
      .framing = CigFraming::kUnframed,
      .max_transport_latency_c_to_p = kMaxTransportLatencyCToP,
      .max_transport_latency_p_to_c = kMaxTransportLatencyPToC,
      .worst_case_sca = std::nullopt,
  };

  bool callback_called = false;
  cig->SetParams(
      std::move(cig_params), {}, [&](IsoGroup::SetParamsResult result) {
        callback_called = true;
        EXPECT_TRUE(result.has_value());
      });

  EXPECT_CMD_PACKET_OUT(
      test_device(),
      bt::testing::LESetCIGParametersCommandPacket(
          kCigId,
          kSduIntervalCToP,
          kSduIntervalPToC,
          static_cast<pw::bluetooth::emboss::LESleepClockAccuracyRange>(0),
          pw::bluetooth::emboss::LECISPacking::SEQUENTIAL,
          pw::bluetooth::emboss::LECISFraming::UNFRAMED,
          kMaxTransportLatencyCToP,
          kMaxTransportLatencyPToC,
          {}));
  RunUntilIdle();

  EXPECT_FALSE(callback_called);

  test_device()->SendCommandChannelPacket(
      bt::testing::LESetCIGParametersCompletePacket(kCigId, {}));
  RunUntilIdle();

  EXPECT_TRUE(callback_called);
}

TEST_F(IsoGroupTest, SetParamsFails) {
  constexpr hci_spec::CigIdentifier kCigId = 0x99;
  constexpr uint32_t kSduIntervalCToP = 0x000F0E0D;
  constexpr uint32_t kSduIntervalPToC = 0x000C0B0A;
  constexpr uint16_t kMaxTransportLatencyCToP = 0x0102;
  constexpr uint16_t kMaxTransportLatencyPToC = 0x0304;

  auto cig = IsoGroup::CreateCig(kCigId,
                                 transport()->GetWeakPtr(),
                                 cig_stream_creator_->GetWeakPtr(),
                                 [](IsoGroup&) {});

  CigParams cig_params = {
      .sdu_interval_c_to_p = kSduIntervalCToP,
      .sdu_interval_p_to_c = kSduIntervalPToC,
      .packing = CigPacking::kSequential,
      .framing = CigFraming::kUnframed,
      .max_transport_latency_c_to_p = kMaxTransportLatencyCToP,
      .max_transport_latency_p_to_c = kMaxTransportLatencyPToC,
      .worst_case_sca =
          pw::bluetooth::emboss::LESleepClockAccuracyRange::PPM_0_TO_20,
  };

  bool callback_called = false;
  cig->SetParams(
      std::move(cig_params), {}, [&](IsoGroup::SetParamsResult result) {
        callback_called = true;
        EXPECT_FALSE(result.has_value());
      });

  EXPECT_CMD_PACKET_OUT(
      test_device(),
      bt::testing::LESetCIGParametersCommandPacket(
          kCigId,
          kSduIntervalCToP,
          kSduIntervalPToC,
          pw::bluetooth::emboss::LESleepClockAccuracyRange::PPM_0_TO_20,
          pw::bluetooth::emboss::LECISPacking::SEQUENTIAL,
          pw::bluetooth::emboss::LECISFraming::UNFRAMED,
          kMaxTransportLatencyCToP,
          kMaxTransportLatencyPToC,
          {}));
  RunUntilIdle();

  EXPECT_FALSE(callback_called);

  test_device()->SendCommandChannelPacket(
      bt::testing::LESetCIGParametersCompletePacket(
          kCigId, {}, pw::bluetooth::emboss::StatusCode::UNSPECIFIED_ERROR));
  RunUntilIdle();

  EXPECT_TRUE(callback_called);
  EXPECT_TRUE(cig_stream_creator_->streams().empty());
}

TEST_F(IsoGroupTest, SetParamsSingleCis) {
  constexpr hci_spec::CigIdentifier kCigId = 0x99;
  constexpr uint32_t kSduIntervalCToP = 0x000F0E0D;
  constexpr uint32_t kSduIntervalPToC = 0x000C0B0A;
  constexpr uint16_t kMaxTransportLatencyCToP = 0x0102;
  constexpr uint16_t kMaxTransportLatencyPToC = 0x0304;

  auto cig = IsoGroup::CreateCig(kCigId,
                                 transport()->GetWeakPtr(),
                                 cig_stream_creator_->GetWeakPtr(),
                                 [](IsoGroup&) {});

  CigParams cig_params = {
      .sdu_interval_c_to_p = kSduIntervalCToP,
      .sdu_interval_p_to_c = kSduIntervalPToC,
      .packing = CigPacking::kSequential,
      .framing = CigFraming::kUnframed,
      .max_transport_latency_c_to_p = kMaxTransportLatencyCToP,
      .max_transport_latency_p_to_c = kMaxTransportLatencyPToC,
      .worst_case_sca =
          pw::bluetooth::emboss::LESleepClockAccuracyRange::PPM_0_TO_20,
  };

  constexpr hci_spec::ConnectionHandle kCisHandle1 = 0x0001;

  constexpr CisConfigParams kCisConfigParams = {
      .cis_id = 0x01,
      .max_sdu_c_to_p = 0x100,
      .max_sdu_p_to_c = 0x200,
  };

  std::vector<CigCisParams> cis_params;
  cis_params.push_back({kCisConfigParams, [](auto, auto, auto) { FAIL(); }});

  bool callback_called = false;
  cig->SetParams(std::move(cig_params),
                 std::move(cis_params),
                 [&](IsoGroup::SetParamsResult result) {
                   callback_called = true;
                   EXPECT_TRUE(result.has_value());
                 });

  // Expect a HCI_LE_Set_CIG_Parameters command with 1 CIS
  EXPECT_CMD_PACKET_OUT(
      test_device(),
      bt::testing::LESetCIGParametersCommandPacket(
          kCigId,
          kSduIntervalCToP,
          kSduIntervalPToC,
          pw::bluetooth::emboss::LESleepClockAccuracyRange::PPM_0_TO_20,
          pw::bluetooth::emboss::LECISPacking::SEQUENTIAL,
          pw::bluetooth::emboss::LECISFraming::UNFRAMED,
          kMaxTransportLatencyCToP,
          kMaxTransportLatencyPToC,
          pw::span<const CisConfigParams>(&kCisConfigParams, 1)));
  RunUntilIdle();

  EXPECT_FALSE(callback_called);
  EXPECT_TRUE(cig_stream_creator_->streams().empty());
  EXPECT_TRUE(cig->streams().empty());

  test_device()->SendCommandChannelPacket(
      bt::testing::LESetCIGParametersCompletePacket(kCigId, {kCisHandle1}));
  RunUntilIdle();

  EXPECT_TRUE(callback_called);
  EXPECT_EQ(cig_stream_creator_->streams().size(), 1u);
  EXPECT_EQ(cig->streams().size(), 1u);

  // Close the stream, ensure it is removed from the CIG.
  ASSERT_TRUE(
      cig_stream_creator_->streams().count({kCigId, kCisConfigParams.cis_id}));
  cig_stream_creator_->streams().at({kCigId, kCisConfigParams.cis_id})->Close();
  RunUntilIdle();

  EXPECT_EQ(cig->streams().size(), 0u);
}

TEST_F(IsoGroupTest, FullHappyPath) {
  constexpr hci_spec::CigIdentifier kCigId = 0x99;
  constexpr uint32_t kSduIntervalCToP = 0x000F0E0D;
  constexpr uint32_t kSduIntervalPToC = 0x000C0B0A;
  constexpr uint16_t kMaxTransportLatencyCToP = 0x0102;
  constexpr uint16_t kMaxTransportLatencyPToC = 0x0304;

  auto cig = IsoGroup::CreateCig(kCigId,
                                 transport()->GetWeakPtr(),
                                 cig_stream_creator_->GetWeakPtr(),
                                 [](IsoGroup&) {});

  CigParams cig_params = {
      .sdu_interval_c_to_p = kSduIntervalCToP,
      .sdu_interval_p_to_c = kSduIntervalPToC,
      .packing = CigPacking::kSequential,
      .framing = CigFraming::kUnframed,
      .max_transport_latency_c_to_p = kMaxTransportLatencyCToP,
      .max_transport_latency_p_to_c = kMaxTransportLatencyPToC,
      .worst_case_sca =
          pw::bluetooth::emboss::LESleepClockAccuracyRange::PPM_0_TO_20,
  };

  constexpr hci_spec::ConnectionHandle kCisHandle1 = 0x0001;
  constexpr hci_spec::ConnectionHandle kCisHandle2 = 0x0002;

  constexpr std::array<CisConfigParams, 2> kCisConfigParams = {{
      {
          .cis_id = 0x01,
          .max_sdu_c_to_p = 0x100,
          .max_sdu_p_to_c = 0x200,
      },
      {
          .cis_id = 0x02,
          .max_sdu_c_to_p = 0x300,
          .max_sdu_p_to_c = 0x400,
      },
  }};

  std::vector<CigCisParams> cis_params;
  for (const auto& cis_config : kCisConfigParams) {
    cis_params.push_back({cis_config, [](auto, auto, auto) { FAIL(); }});
  }

  bool callback_called = false;
  cig->SetParams(std::move(cig_params),
                 std::move(cis_params),
                 [&](IsoGroup::SetParamsResult result) {
                   callback_called = true;
                   EXPECT_TRUE(result.has_value());
                 });

  // Expect a HCI_LE_Set_CIG_Parameters command with 2 CISes
  EXPECT_CMD_PACKET_OUT(
      test_device(),
      bt::testing::LESetCIGParametersCommandPacket(
          kCigId,
          kSduIntervalCToP,
          kSduIntervalPToC,
          pw::bluetooth::emboss::LESleepClockAccuracyRange::PPM_0_TO_20,
          pw::bluetooth::emboss::LECISPacking::SEQUENTIAL,
          pw::bluetooth::emboss::LECISFraming::UNFRAMED,
          kMaxTransportLatencyCToP,
          kMaxTransportLatencyPToC,
          kCisConfigParams));
  RunUntilIdle();

  EXPECT_FALSE(callback_called);
  EXPECT_TRUE(cig_stream_creator_->streams().empty());
  EXPECT_TRUE(cig->streams().empty());

  test_device()->SendCommandChannelPacket(
      bt::testing::LESetCIGParametersCompletePacket(
          kCigId, {kCisHandle1, kCisHandle2}));
  RunUntilIdle();

  EXPECT_TRUE(callback_called);
  EXPECT_EQ(cig_stream_creator_->streams().size(), 2u);
  EXPECT_EQ(cig->streams().size(), 2u);

  // Close the streams, ensure they are removed from the CIG.
  ASSERT_TRUE(cig_stream_creator_->streams().count(
      {kCigId, kCisConfigParams[0].cis_id}));
  cig_stream_creator_->streams()
      .at({kCigId, kCisConfigParams[0].cis_id})
      ->Close();
  ASSERT_TRUE(cig_stream_creator_->streams().count(
      {kCigId, kCisConfigParams[1].cis_id}));
  cig_stream_creator_->streams()
      .at({kCigId, kCisConfigParams[1].cis_id})
      ->Close();
  RunUntilIdle();

  EXPECT_EQ(cig->streams().size(), 0u);
}

TEST_F(IsoGroupTest, SetParamsIncorrectCigIdResponse) {
  constexpr hci_spec::CigIdentifier kCigId = 0x99;
  constexpr hci_spec::CigIdentifier kOtherCigId = 0x98;
  constexpr uint32_t kSduIntervalCToP = 0x000F0E0D;
  constexpr uint32_t kSduIntervalPToC = 0x000C0B0A;
  constexpr uint16_t kMaxTransportLatencyCToP = 0x0102;
  constexpr uint16_t kMaxTransportLatencyPToC = 0x0304;

  auto cig = IsoGroup::CreateCig(kCigId,
                                 transport()->GetWeakPtr(),
                                 cig_stream_creator_->GetWeakPtr(),
                                 [](IsoGroup&) {});

  CigParams cig_params = {
      .sdu_interval_c_to_p = kSduIntervalCToP,
      .sdu_interval_p_to_c = kSduIntervalPToC,
      .packing = CigPacking::kSequential,
      .framing = CigFraming::kUnframed,
      .max_transport_latency_c_to_p = kMaxTransportLatencyCToP,
      .max_transport_latency_p_to_c = kMaxTransportLatencyPToC,
      .worst_case_sca =
          pw::bluetooth::emboss::LESleepClockAccuracyRange::PPM_0_TO_20,
  };

  bool callback_called = false;
  cig->SetParams(
      std::move(cig_params), {}, [&](IsoGroup::SetParamsResult result) {
        callback_called = true;
        EXPECT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), HostError::kFailed);
      });

  EXPECT_CMD_PACKET_OUT(
      test_device(),
      bt::testing::LESetCIGParametersCommandPacket(
          kCigId,
          kSduIntervalCToP,
          kSduIntervalPToC,
          pw::bluetooth::emboss::LESleepClockAccuracyRange::PPM_0_TO_20,
          pw::bluetooth::emboss::LECISPacking::SEQUENTIAL,
          pw::bluetooth::emboss::LECISFraming::UNFRAMED,
          kMaxTransportLatencyCToP,
          kMaxTransportLatencyPToC,
          {}));
  RunUntilIdle();

  EXPECT_FALSE(callback_called);

  // Send response with incorrect CigId.
  test_device()->SendCommandChannelPacket(
      bt::testing::LESetCIGParametersCompletePacket(kOtherCigId, {}));
  RunUntilIdle();

  EXPECT_TRUE(callback_called);
}

TEST_F(IsoGroupTest, SetParamsIncorrectCisCountResponse) {
  constexpr hci_spec::CigIdentifier kCigId = 0x99;
  constexpr uint32_t kSduIntervalCToP = 0x000F0E0D;
  constexpr uint32_t kSduIntervalPToC = 0x000C0B0A;
  constexpr uint16_t kMaxTransportLatencyCToP = 0x0102;
  constexpr uint16_t kMaxTransportLatencyPToC = 0x0304;

  auto cig = IsoGroup::CreateCig(kCigId,
                                 transport()->GetWeakPtr(),
                                 cig_stream_creator_->GetWeakPtr(),
                                 [](IsoGroup&) {});

  CigParams cig_params = {
      .sdu_interval_c_to_p = kSduIntervalCToP,
      .sdu_interval_p_to_c = kSduIntervalPToC,
      .packing = CigPacking::kSequential,
      .framing = CigFraming::kUnframed,
      .max_transport_latency_c_to_p = kMaxTransportLatencyCToP,
      .max_transport_latency_p_to_c = kMaxTransportLatencyPToC,
      .worst_case_sca =
          pw::bluetooth::emboss::LESleepClockAccuracyRange::PPM_0_TO_20,
  };

  constexpr CisConfigParams kCisConfigParams = {
      .cis_id = 0x01,
      .max_sdu_c_to_p = 0x100,
      .max_sdu_p_to_c = 0x200,
  };

  std::vector<CigCisParams> cis_params;
  cis_params.push_back({kCisConfigParams, [](auto, auto, auto) { FAIL(); }});

  bool callback_called = false;
  cig->SetParams(std::move(cig_params),
                 std::move(cis_params),
                 [&](IsoGroup::SetParamsResult result) {
                   callback_called = true;
                   EXPECT_FALSE(result.has_value());
                   EXPECT_EQ(result.error(), HostError::kFailed);
                 });

  EXPECT_CMD_PACKET_OUT(
      test_device(),
      bt::testing::LESetCIGParametersCommandPacket(
          kCigId,
          kSduIntervalCToP,
          kSduIntervalPToC,
          pw::bluetooth::emboss::LESleepClockAccuracyRange::PPM_0_TO_20,
          pw::bluetooth::emboss::LECISPacking::SEQUENTIAL,
          pw::bluetooth::emboss::LECISFraming::UNFRAMED,
          kMaxTransportLatencyCToP,
          kMaxTransportLatencyPToC,
          pw::span<const CisConfigParams>(&kCisConfigParams, 1)));
  RunUntilIdle();

  EXPECT_FALSE(callback_called);

  // The response has 0 CIS handles, but we requested 1.
  test_device()->SendCommandChannelPacket(
      bt::testing::LESetCIGParametersCompletePacket(kCigId, {}));
  RunUntilIdle();

  EXPECT_TRUE(callback_called);
}

TEST_F(IsoGroupTest, SetParamsCigStreamCreatorDestroyed) {
  constexpr hci_spec::CigIdentifier kCigId = 0x99;
  constexpr uint32_t kSduIntervalCToP = 0x000F0E0D;
  constexpr uint32_t kSduIntervalPToC = 0x000C0B0A;
  constexpr uint16_t kMaxTransportLatencyCToP = 0x0102;
  constexpr uint16_t kMaxTransportLatencyPToC = 0x0304;

  auto cig = IsoGroup::CreateCig(kCigId,
                                 transport()->GetWeakPtr(),
                                 cig_stream_creator_->GetWeakPtr(),
                                 [](IsoGroup&) {});

  CigParams cig_params = {
      .sdu_interval_c_to_p = kSduIntervalCToP,
      .sdu_interval_p_to_c = kSduIntervalPToC,
      .packing = CigPacking::kSequential,
      .framing = CigFraming::kUnframed,
      .max_transport_latency_c_to_p = kMaxTransportLatencyCToP,
      .max_transport_latency_p_to_c = kMaxTransportLatencyPToC,
      .worst_case_sca =
          pw::bluetooth::emboss::LESleepClockAccuracyRange::PPM_0_TO_20,
  };

  constexpr CisConfigParams kCisConfigParams = {
      .cis_id = 0x01,
      .max_sdu_c_to_p = 0x100,
      .max_sdu_p_to_c = 0x200,
  };

  std::vector<CigCisParams> cis_params;
  cis_params.push_back({kCisConfigParams, [](auto, auto, auto) { FAIL(); }});

  bool callback_called = false;
  cig->SetParams(std::move(cig_params),
                 std::move(cis_params),
                 [&](IsoGroup::SetParamsResult result) {
                   callback_called = true;
                   EXPECT_FALSE(result.has_value());
                   EXPECT_EQ(result.error(), HostError::kFailed);
                 });

  EXPECT_CMD_PACKET_OUT(
      test_device(),
      bt::testing::LESetCIGParametersCommandPacket(
          kCigId,
          kSduIntervalCToP,
          kSduIntervalPToC,
          pw::bluetooth::emboss::LESleepClockAccuracyRange::PPM_0_TO_20,
          pw::bluetooth::emboss::LECISPacking::SEQUENTIAL,
          pw::bluetooth::emboss::LECISFraming::UNFRAMED,
          kMaxTransportLatencyCToP,
          kMaxTransportLatencyPToC,
          pw::span<const CisConfigParams>(&kCisConfigParams, 1)));
  RunUntilIdle();

  EXPECT_FALSE(callback_called);

  // Destroy CigStreamCreator before response.
  cig_stream_creator_.reset();

  test_device()->SendCommandChannelPacket(
      bt::testing::LESetCIGParametersCompletePacket(kCigId, {0x0001}));
  RunUntilIdle();

  EXPECT_TRUE(callback_called);
}

TEST_F(IsoGroupTest, SetParamsDuplicateCisIds) {
  constexpr hci_spec::CigIdentifier kCigId = 0x99;
  constexpr uint32_t kSduIntervalCToP = 0x000F0E0D;
  constexpr uint32_t kSduIntervalPToC = 0x000C0B0A;
  constexpr uint16_t kMaxTransportLatencyCToP = 0x0102;
  constexpr uint16_t kMaxTransportLatencyPToC = 0x0304;

  auto cig = IsoGroup::CreateCig(kCigId,
                                 transport()->GetWeakPtr(),
                                 cig_stream_creator_->GetWeakPtr(),
                                 [](IsoGroup&) {});

  CigParams cig_params = {
      .sdu_interval_c_to_p = kSduIntervalCToP,
      .sdu_interval_p_to_c = kSduIntervalPToC,
      .packing = CigPacking::kSequential,
      .framing = CigFraming::kUnframed,
      .max_transport_latency_c_to_p = kMaxTransportLatencyCToP,
      .max_transport_latency_p_to_c = kMaxTransportLatencyPToC,
      .worst_case_sca =
          pw::bluetooth::emboss::LESleepClockAccuracyRange::PPM_0_TO_20,
  };

  constexpr CisConfigParams kCisConfigParams = {
      .cis_id = 0x01,
      .max_sdu_c_to_p = 0x100,
      .max_sdu_p_to_c = 0x200,
  };

  std::vector<CigCisParams> cis_params;
  cis_params.push_back({kCisConfigParams, [](auto, auto, auto) { FAIL(); }});
  cis_params.push_back({kCisConfigParams, [](auto, auto, auto) { FAIL(); }});

  bool callback_called = false;
  cig->SetParams(std::move(cig_params),
                 std::move(cis_params),
                 [&](IsoGroup::SetParamsResult result) {
                   callback_called = true;
                   EXPECT_FALSE(result.has_value());
                   EXPECT_EQ(result.error(), HostError::kInvalidParameters);
                 });
  RunUntilIdle();

  EXPECT_TRUE(callback_called);
  EXPECT_TRUE(cig->streams().empty());
}

TEST_F(IsoGroupTest, SetParamsExistingCisId) {
  constexpr hci_spec::CigIdentifier kCigId = 0x99;
  constexpr uint32_t kSduIntervalCToP = 0x000F0E0D;
  constexpr uint32_t kSduIntervalPToC = 0x000C0B0A;
  constexpr uint16_t kMaxTransportLatencyCToP = 0x0102;
  constexpr uint16_t kMaxTransportLatencyPToC = 0x0304;

  auto cig = IsoGroup::CreateCig(kCigId,
                                 transport()->GetWeakPtr(),
                                 cig_stream_creator_->GetWeakPtr(),
                                 [](IsoGroup&) {});

  CigParams cig_params = {
      .sdu_interval_c_to_p = kSduIntervalCToP,
      .sdu_interval_p_to_c = kSduIntervalPToC,
      .packing = CigPacking::kSequential,
      .framing = CigFraming::kUnframed,
      .max_transport_latency_c_to_p = kMaxTransportLatencyCToP,
      .max_transport_latency_p_to_c = kMaxTransportLatencyPToC,
      .worst_case_sca =
          pw::bluetooth::emboss::LESleepClockAccuracyRange::PPM_0_TO_20,
  };

  constexpr hci_spec::ConnectionHandle kCisHandle1 = 0x0001;

  constexpr CisConfigParams kCisConfigParams = {
      .cis_id = 0x01,
      .max_sdu_c_to_p = 0x100,
      .max_sdu_p_to_c = 0x200,
  };

  std::vector<CigCisParams> cis_params;
  cis_params.push_back({kCisConfigParams, [](auto, auto, auto) { FAIL(); }});

  bool callback_called = false;
  cig->SetParams(
      cig_params, std::move(cis_params), [&](IsoGroup::SetParamsResult result) {
        callback_called = true;
        EXPECT_TRUE(result.has_value());
      });

  EXPECT_CMD_PACKET_OUT(
      test_device(),
      bt::testing::LESetCIGParametersCommandPacket(
          kCigId,
          kSduIntervalCToP,
          kSduIntervalPToC,
          pw::bluetooth::emboss::LESleepClockAccuracyRange::PPM_0_TO_20,
          pw::bluetooth::emboss::LECISPacking::SEQUENTIAL,
          pw::bluetooth::emboss::LECISFraming::UNFRAMED,
          kMaxTransportLatencyCToP,
          kMaxTransportLatencyPToC,
          pw::span<const CisConfigParams>(&kCisConfigParams, 1)));
  RunUntilIdle();

  EXPECT_FALSE(callback_called);

  test_device()->SendCommandChannelPacket(
      bt::testing::LESetCIGParametersCompletePacket(kCigId, {kCisHandle1}));
  RunUntilIdle();

  EXPECT_TRUE(callback_called);
  EXPECT_EQ(cig->streams().size(), 1u);

  // Now, try to configure the same CIS ID again.
  std::vector<CigCisParams> duplicate_cis_params;
  duplicate_cis_params.push_back(
      {kCisConfigParams, [](auto, auto, auto) { FAIL(); }});

  bool callback_called_duplicate = false;
  cig->SetParams(std::move(cig_params),
                 std::move(duplicate_cis_params),
                 [&](IsoGroup::SetParamsResult result) {
                   callback_called_duplicate = true;
                   EXPECT_FALSE(result.has_value());
                   EXPECT_EQ(result.error(), HostError::kInvalidParameters);
                 });
  RunUntilIdle();

  EXPECT_TRUE(callback_called_duplicate);
  EXPECT_EQ(cig->streams().size(), 1u);
}

}  // namespace
}  // namespace bt::iso
