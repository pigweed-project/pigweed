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

#include "pw_bluetooth_sapphire/internal/host/iso/iso_group_manager.h"

#include "pw_bluetooth_sapphire/internal/host/iso/fake_cig_stream_creator.h"
#include "pw_bluetooth_sapphire/internal/host/iso/fake_iso_group.h"
#include "pw_bluetooth_sapphire/internal/host/testing/controller_test.h"
#include "pw_bluetooth_sapphire/internal/host/testing/mock_controller.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_packets.h"

namespace bt::iso {
namespace {

using testing::FakeCigStreamCreator;
using testing::FakeIsoGroup;

class IsoGroupManagerTest : public bt::testing::FakeDispatcherControllerTest<
                                bt::testing::MockController> {
 public:
  IsoGroupManagerTest() = default;
  ~IsoGroupManagerTest() override = default;

  void SetUp() override {
    bt::testing::FakeDispatcherControllerTest<
        bt::testing::MockController>::SetUp();
    cig_stream_creator_ = std::make_unique<FakeCigStreamCreator>();
    manager_ = std::make_unique<IsoGroupManager>(
        transport()->GetWeakPtr(),
        cig_stream_creator_->GetWeakPtr(),
        [this](hci_spec::CigIdentifier id,
               hci::Transport::WeakPtr hci,
               CigStreamCreator::WeakPtr cig_stream_creator,
               IsoGroup::OnClosedCallback on_closed_callback) {
          auto fake_cig =
              std::make_unique<FakeIsoGroup>(id,
                                             std::move(hci),
                                             std::move(cig_stream_creator),
                                             std::move(on_closed_callback));
          last_created_cig_ = fake_cig->GetWeakPtrForFake();
          return fake_cig;
        });
  }

  void TearDown() override {
    manager_.reset();
    cig_stream_creator_.reset();
    bt::testing::FakeDispatcherControllerTest<
        bt::testing::MockController>::TearDown();
  }

 protected:
  std::unique_ptr<FakeCigStreamCreator> cig_stream_creator_;
  std::unique_ptr<IsoGroupManager> manager_;
  FakeIsoGroup::WeakPtr last_created_cig_;
};

TEST_F(IsoGroupManagerTest, CreateCigSuccess) {
  CigParams cig_params = {
      .sdu_interval_c_to_p = 0x000F0E0D,
      .sdu_interval_p_to_c = 0x000C0B0A,
      .packing = CigPacking::kSequential,
      .framing = CigFraming::kUnframed,
      .max_transport_latency_c_to_p = 0x0102,
      .max_transport_latency_p_to_c = 0x0304,
      .worst_case_sca =
          pw::bluetooth::emboss::LESleepClockAccuracyRange::PPM_0_TO_20,
  };
  std::vector<CigCisParams> cis_params;

  bool callback_called = false;
  manager_->CreateCig(
      std::move(cig_params),
      std::move(cis_params),
      [&](pw::expected<IsoGroup::WeakPtr, HostError> result) {
        callback_called = true;
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(result->is_alive());
      },
      [](IsoGroup&) {});
  RunUntilIdle();

  EXPECT_FALSE(callback_called);
  ASSERT_TRUE(last_created_cig_.is_alive());
  last_created_cig_->CompleteSetParams({});
  RunUntilIdle();

  EXPECT_TRUE(callback_called);
  EXPECT_EQ(last_created_cig_->id(), 0);
}

TEST_F(IsoGroupManagerTest, CreateCigSetParamsFails) {
  CigParams cig_params = {
      .sdu_interval_c_to_p = 0x000F0E0D,
      .sdu_interval_p_to_c = 0x000C0B0A,
      .packing = CigPacking::kSequential,
      .framing = CigFraming::kUnframed,
      .max_transport_latency_c_to_p = 0x0102,
      .max_transport_latency_p_to_c = 0x0304,
      .worst_case_sca =
          pw::bluetooth::emboss::LESleepClockAccuracyRange::PPM_0_TO_20,
  };

  bool callback_called = false;
  manager_->CreateCig(
      std::move(cig_params),
      {},
      [&](pw::expected<IsoGroup::WeakPtr, HostError> result) {
        callback_called = true;
        EXPECT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), HostError::kFailed);
      },
      [](IsoGroup&) {});
  RunUntilIdle();

  EXPECT_FALSE(callback_called);
  ASSERT_TRUE(last_created_cig_.is_alive());
  last_created_cig_->CompleteSetParams(pw::unexpected(HostError::kFailed));
  RunUntilIdle();

  EXPECT_TRUE(callback_called);
  EXPECT_EQ(last_created_cig_->id(), 0);
}

TEST_F(IsoGroupManagerTest, CreateCigAndDeleteOnClose) {
  CigParams cig_params = {
      .sdu_interval_c_to_p = 0x000F0E0D,
      .sdu_interval_p_to_c = 0x000C0B0A,
      .packing = CigPacking::kSequential,
      .framing = CigFraming::kUnframed,
      .max_transport_latency_c_to_p = 0x0102,
      .max_transport_latency_p_to_c = 0x0304,
      .worst_case_sca =
          pw::bluetooth::emboss::LESleepClockAccuracyRange::PPM_0_TO_20,
  };
  std::vector<CigCisParams> cis_params;

  bool on_closed_cb_called = false;
  manager_->CreateCig(
      std::move(cig_params),
      std::move(cis_params),
      [&](pw::expected<IsoGroup::WeakPtr, HostError> result) {
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(result->is_alive());
      },
      [&](IsoGroup&) { on_closed_cb_called = true; });
  RunUntilIdle();

  ASSERT_TRUE(last_created_cig_.is_alive());
  last_created_cig_->CompleteSetParams({});
  RunUntilIdle();

  EXPECT_EQ(last_created_cig_->id(), 0);

  last_created_cig_->TriggerOnClosedCallback();
  RunUntilIdle();
  EXPECT_TRUE(on_closed_cb_called);
  EXPECT_FALSE(last_created_cig_.is_alive());
}

TEST_F(IsoGroupManagerTest, CreateCigExhaustiveAllocation) {
  CigParams cig_params = {
      .sdu_interval_c_to_p = 0x000F0E0D,
      .sdu_interval_p_to_c = 0x000C0B0A,
      .packing = CigPacking::kSequential,
      .framing = CigFraming::kUnframed,
      .max_transport_latency_c_to_p = 0x0102,
      .max_transport_latency_p_to_c = 0x0304,
      .worst_case_sca =
          pw::bluetooth::emboss::LESleepClockAccuracyRange::PPM_0_TO_20,
  };

  FakeIsoGroup::WeakPtr cig_42;

  // Allocate all 240 IDs
  for (int i = 0; i < 240; ++i) {
    bool callback_called = false;
    manager_->CreateCig(
        cig_params,
        {},
        [&](pw::expected<IsoGroup::WeakPtr, HostError> result) {
          callback_called = true;
          EXPECT_TRUE(result.has_value());
        },
        [](IsoGroup&) {});
    RunUntilIdle();
    EXPECT_FALSE(callback_called);
    ASSERT_TRUE(last_created_cig_.is_alive());
    last_created_cig_->CompleteSetParams({});
    RunUntilIdle();
    EXPECT_TRUE(callback_called);
    EXPECT_EQ(last_created_cig_->id(), i);

    if (i == 42) {
      cig_42 = last_created_cig_;
    }
  }

  // 241st should fail
  bool callback_called = false;
  manager_->CreateCig(
      cig_params,
      {},
      [&](pw::expected<IsoGroup::WeakPtr, HostError> result) {
        callback_called = true;
        EXPECT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), HostError::kOutOfMemory);
      },
      [](IsoGroup&) {});
  RunUntilIdle();
  EXPECT_TRUE(callback_called);

  // Delete ID 239 (the last one created)
  ASSERT_TRUE(last_created_cig_.is_alive());
  EXPECT_EQ(last_created_cig_->id(), 239);
  last_created_cig_->TriggerOnClosedCallback();
  RunUntilIdle();

  // Try to allocate again. It should succeed and get ID 239.
  callback_called = false;
  manager_->CreateCig(
      cig_params,
      {},
      [&](pw::expected<IsoGroup::WeakPtr, HostError> result) {
        callback_called = true;
        EXPECT_TRUE(result.has_value());
        if (result.has_value()) {
          EXPECT_EQ((*result)->id(), 239);
        }
      },
      [](IsoGroup&) {});
  RunUntilIdle();

  ASSERT_TRUE(last_created_cig_.is_alive());
  last_created_cig_->CompleteSetParams({});
  RunUntilIdle();
  EXPECT_TRUE(callback_called);

  // Delete ID 42
  ASSERT_TRUE(cig_42.is_alive());
  EXPECT_EQ(cig_42->id(), 42);
  cig_42->TriggerOnClosedCallback();
  RunUntilIdle();

  // Try to allocate again. It should succeed and get ID 42.
  callback_called = false;
  manager_->CreateCig(
      cig_params,
      {},
      [&](pw::expected<IsoGroup::WeakPtr, HostError> result) {
        callback_called = true;
        EXPECT_TRUE(result.has_value());
        if (result.has_value()) {
          EXPECT_EQ((*result)->id(), 42);
        }
      },
      [](IsoGroup&) {});
  RunUntilIdle();

  ASSERT_TRUE(last_created_cig_.is_alive());
  last_created_cig_->CompleteSetParams({});
  RunUntilIdle();

  EXPECT_TRUE(callback_called);

  // One more out of memory for good measure
  callback_called = false;
  manager_->CreateCig(
      cig_params,
      {},
      [&](pw::expected<IsoGroup::WeakPtr, HostError> result) {
        callback_called = true;
        EXPECT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), HostError::kOutOfMemory);
      },
      [](IsoGroup&) {});
  RunUntilIdle();
  EXPECT_TRUE(callback_called);
}

TEST_F(IsoGroupManagerTest, CreateCigCigDestroyedBeforeSetParamsCompletes) {
  CigParams cig_params = {
      .sdu_interval_c_to_p = 0x000F0E0D,
      .sdu_interval_p_to_c = 0x000C0B0A,
      .packing = CigPacking::kSequential,
      .framing = CigFraming::kUnframed,
      .max_transport_latency_c_to_p = 0x0102,
      .max_transport_latency_p_to_c = 0x0304,
      .worst_case_sca =
          pw::bluetooth::emboss::LESleepClockAccuracyRange::PPM_0_TO_20,
  };

  bool callback_called = false;
  manager_->CreateCig(
      std::move(cig_params),
      {},
      [&](pw::expected<IsoGroup::WeakPtr, HostError> result) {
        callback_called = true;
        EXPECT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), HostError::kFailed);
      },
      [](IsoGroup&) {});
  RunUntilIdle();

  EXPECT_FALSE(callback_called);
  ASSERT_TRUE(last_created_cig_.is_alive());

  auto callback = last_created_cig_->steal_set_params_callback();
  ASSERT_TRUE(callback);

  // Destroy the CIG by triggering the OnClosedCallback which causes the manager
  // to erase it.
  last_created_cig_->TriggerOnClosedCallback();
  RunUntilIdle();

  EXPECT_FALSE(last_created_cig_.is_alive());

  callback({});
  RunUntilIdle();

  EXPECT_TRUE(callback_called);
}

}  // namespace
}  // namespace bt::iso
