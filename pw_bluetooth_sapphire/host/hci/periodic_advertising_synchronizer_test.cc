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

#include "pw_bluetooth_sapphire/internal/host/hci/periodic_advertising_synchronizer.h"

#include <map>

#include "pw_bluetooth_sapphire/internal/host/testing/controller_test.h"
#include "pw_bluetooth_sapphire/internal/host/testing/mock_controller.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_packets.h"
#include "pw_bluetooth_sapphire/internal/host/transport/control_packets.h"
#include "pw_bluetooth_sapphire/internal/host/transport/error.h"
#include "pw_unit_test/framework.h"

namespace bt::hci {
namespace {

using testing::MockController;
using TestingBase =
    bt::testing::FakeDispatcherControllerTest<bt::testing::MockController>;

constexpr uint16_t kSyncTimeout = 0x4000;
constexpr hci_spec::ConnectionHandle kConnectionHandle = 0x0007;
const uint16_t kServiceData = 0x0908;

auto CreateSyncPacket(bool filter_duplicates = false,
                      bool use_periodic_advertiser_list = false) {
  return bt::testing::LEPeriodicAdvertisingCreateSyncPacket(
      DeviceAddress(DeviceAddress::Type::kLEPublic, {0}),
      /*sid=*/0,
      kSyncTimeout,
      filter_duplicates,
      use_periodic_advertiser_list);
}

void ExpectTerminateSync(
    const bt::testing::MockController::WeakPtr& mock_controller,
    hci_spec::SyncHandle sync_handle) {
  auto terminate_sync_packet =
      bt::testing::LEPeriodicAdvertisingTerminateSyncPacket(sync_handle);
  auto terminate_sync_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_TERMINATE_SYNC,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      mock_controller, terminate_sync_packet, &terminate_sync_complete);
}

class TestDelegate : public PeriodicAdvertisingSynchronizer::Delegate {
 public:
  void OnSyncEstablished(
      SyncId id,
      PeriodicAdvertisingSynchronizer::SyncParameters parameters) override {
    sync_established_count_++;
    last_sync_id_ = id;
    last_sync_params_ = parameters;
  }

  void OnSyncLost(SyncId id, Error error) override {
    sync_lost_count_++;
    last_sync_id_ = id;
    last_error_ = error;
    if (on_sync_lost_cb_) {
      on_sync_lost_cb_();
    }
  }

  void OnAdvertisingReport(
      SyncId id,
      PeriodicAdvertisingSynchronizer::PeriodicAdvertisingReport&& report)
      override {
    report_count_++;
    last_sync_id_ = id;
    last_report_ = std::move(report);
  }

  void OnBigInfoReport(
      SyncId id,
      PeriodicAdvertisingSynchronizer::BroadcastIsochronousGroupInfo&& report)
      override {
    big_info_count_++;
    last_sync_id_ = id;
    last_big_info_ = std::move(report);
  }

  int sync_established_count() const { return sync_established_count_; }
  int sync_lost_count() const { return sync_lost_count_; }
  int report_count() const { return report_count_; }
  int big_info_count() const { return big_info_count_; }

  std::optional<SyncId> last_sync_id() const { return last_sync_id_; }
  std::optional<PeriodicAdvertisingSynchronizer::SyncParameters>
  last_sync_params() const {
    return last_sync_params_;
  }
  std::optional<Error> last_error() const { return last_error_; }
  std::optional<PeriodicAdvertisingSynchronizer::PeriodicAdvertisingReport>
  last_report() const {
    return last_report_;
  }
  std::optional<PeriodicAdvertisingSynchronizer::BroadcastIsochronousGroupInfo>
  last_big_info() const {
    return last_big_info_;
  }

  void set_on_sync_lost_callback(fit::closure cb) {
    on_sync_lost_cb_ = std::move(cb);
  }

 private:
  int sync_established_count_ = 0;
  int sync_lost_count_ = 0;
  int report_count_ = 0;
  int big_info_count_ = 0;

  std::optional<SyncId> last_sync_id_;
  std::optional<PeriodicAdvertisingSynchronizer::SyncParameters>
      last_sync_params_;
  std::optional<Error> last_error_;
  std::optional<PeriodicAdvertisingSynchronizer::PeriodicAdvertisingReport>
      last_report_;
  std::optional<PeriodicAdvertisingSynchronizer::BroadcastIsochronousGroupInfo>
      last_big_info_;
  fit::closure on_sync_lost_cb_;
};

class PeriodicAdvertisingSynchronizerTest : public TestingBase {
 public:
  PeriodicAdvertisingSynchronizerTest() = default;
  ~PeriodicAdvertisingSynchronizerTest() override = default;

 protected:
  void SetUp() override {
    TestingBase::SetUp();
    synchronizer_.emplace(transport()->GetWeakPtr());
  }

  void TearDown() override {
    synchronizer_.reset();
    TestingBase::TearDown();
  }

  std::optional<PeriodicAdvertisingSync> CreateSyncAndExpectSuccess(
      TestDelegate& delegate,
      const DeviceAddress& addr,
      uint8_t adv_sid,
      hci_spec::SyncHandle sync_handle,
      bool v2 = false,
      bool filter_duplicates = false) {
    auto add_to_list_packet =
        bt::testing::LEAddDeviceToPeriodicAdvertiserListPacket(addr, adv_sid);
    auto add_to_list_complete = bt::testing::CommandCompletePacket(
        pw::bluetooth::emboss::OpCode::
            LE_ADD_DEVICE_TO_PERIODIC_ADVERTISER_LIST,
        pw::bluetooth::emboss::StatusCode::SUCCESS);
    EXPECT_CMD_PACKET_OUT(
        test_device(), add_to_list_packet, &add_to_list_complete);

    auto command_status_rsp = bt::testing::CommandStatusPacket(
        pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC,
        pw::bluetooth::emboss::StatusCode::SUCCESS);

    EXPECT_CMD_PACKET_OUT(
        test_device(),
        CreateSyncPacket(filter_duplicates,
                         /*use_periodic_advertiser_list=*/true),
        &command_status_rsp);

    auto sync_result = synchronizer()->CreateSync(
        addr, adv_sid, {.filter_duplicates = filter_duplicates}, delegate);
    EXPECT_TRUE(sync_result.is_ok());
    RunUntilIdle();
    EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());

    constexpr uint16_t kSyncPacketInterval = 0x000A;
    DynamicByteBuffer sync_established_event;

    constexpr uint8_t kNumSubevents = 2;
    if (v2) {
      sync_established_event =
          bt::testing::LEPeriodicAdvertisingSyncEstablishedEventPacketV2(
              pw::bluetooth::emboss::StatusCode::SUCCESS,
              sync_handle,
              adv_sid,
              addr,
              pw::bluetooth::emboss::LEPhy::LE_1M,
              kSyncPacketInterval,
              pw::bluetooth::emboss::LEClockAccuracy::PPM_500,
              kNumSubevents);
    } else {
      sync_established_event =
          bt::testing::LEPeriodicAdvertisingSyncEstablishedEventPacketV1(
              pw::bluetooth::emboss::StatusCode::SUCCESS,
              sync_handle,
              adv_sid,
              addr,
              pw::bluetooth::emboss::LEPhy::LE_1M,
              kSyncPacketInterval,
              pw::bluetooth::emboss::LEClockAccuracy::PPM_500);
    }

    auto remove_from_list_packet =
        bt::testing::LERemoveDeviceFromPeriodicAdvertiserListPacket(addr,
                                                                    adv_sid);
    auto remove_from_list_complete = bt::testing::CommandCompletePacket(
        pw::bluetooth::emboss::OpCode::
            LE_REMOVE_DEVICE_FROM_PERIODIC_ADVERTISER_LIST,
        pw::bluetooth::emboss::StatusCode::SUCCESS);
    EXPECT_CMD_PACKET_OUT(
        test_device(), remove_from_list_packet, &remove_from_list_complete);

    test_device()->SendCommandChannelPacket(sync_established_event);
    RunUntilIdle();

    EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());
    EXPECT_EQ(delegate.sync_established_count(), 1);
    EXPECT_TRUE(delegate.last_sync_id().has_value());
    EXPECT_TRUE(delegate.last_sync_params().has_value());
    if (!delegate.last_sync_params().has_value()) {
      return std::nullopt;
    }
    EXPECT_EQ(delegate.last_sync_params()->address, addr);
    EXPECT_EQ(delegate.last_sync_params()->advertising_sid, adv_sid);
    if (v2) {
      EXPECT_EQ(delegate.last_sync_params()->subevents_count, kNumSubevents);
    } else {
      EXPECT_EQ(delegate.last_sync_params()->subevents_count, 0);
    }

    if (sync_result.is_ok()) {
      return std::move(sync_result.value());
    }
    return std::nullopt;
  }

  PeriodicAdvertisingSynchronizer* synchronizer() {
    return &synchronizer_.value();
  }

  void DestroySynchronizer() { synchronizer_.reset(); }

 private:
  std::optional<PeriodicAdvertisingSynchronizer> synchronizer_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(PeriodicAdvertisingSynchronizerTest);
};

TEST_F(PeriodicAdvertisingSynchronizerTest, CreateSyncSuccessV1) {
  TestDelegate delegate;
  DeviceAddress addr(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid = 12;
  constexpr hci_spec::ConnectionHandle kSyncHandle = 0x01;

  auto sync = CreateSyncAndExpectSuccess(delegate, addr, kAdvSid, kSyncHandle);
  ASSERT_TRUE(sync.has_value());

  ExpectTerminateSync(test_device(), kSyncHandle);
}

TEST_F(PeriodicAdvertisingSynchronizerTest, CreateSyncSuccessV2) {
  TestDelegate delegate;
  DeviceAddress addr(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid = 12;
  constexpr hci_spec::ConnectionHandle kSyncHandle = 0x01;

  auto sync = CreateSyncAndExpectSuccess(
      delegate, addr, kAdvSid, kSyncHandle, /*v2=*/true);
  ASSERT_TRUE(sync.has_value());

  ExpectTerminateSync(test_device(), kSyncHandle);
}

TEST_F(PeriodicAdvertisingSynchronizerTest,
       CreateSyncInvalidAddressTypeIsIgnored) {
  TestDelegate delegate;
  DeviceAddress addr(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid = 12;

  auto add_to_list_packet =
      bt::testing::LEAddDeviceToPeriodicAdvertiserListPacket(addr, kAdvSid);
  auto add_to_list_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_ADD_DEVICE_TO_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), add_to_list_packet, &add_to_list_complete);

  auto command_status_rsp = bt::testing::CommandStatusPacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        CreateSyncPacket(/*filter_duplicates=*/false,
                                         /*use_periodic_advertiser_list=*/true),
                        &command_status_rsp);

  auto sync_result = synchronizer()->CreateSync(
      addr, kAdvSid, {.filter_duplicates = false}, delegate);
  EXPECT_TRUE(sync_result.is_ok());
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());

  StaticByteBuffer kMalformedEvent(
      hci_spec::kLEMetaEventCode,
      0x10,  // parameter_total_size
      0x0E,  // Subevent code: LE Periodic Advertising Sync Established
      0x00,  // status: Success
      // sync_handle: 0x0000
      0x00,
      0x00,
      0x00,  // advertising_sid
      0x04,  // advertiser_address_type (Invalid!)
      // advertiser_address: 0xFFEEDDCCBBAA
      0xAA,
      0xBB,
      0xCC,
      0xDD,
      0xEE,
      0xFF,
      0x01,  // advertiser_phy
      // periodic_advertising_interval: 0x0006
      0x06,
      0x00,
      0x00  // advertiser_clock_accuracy
  );

  // Any Sync Established event ends Create Sync, so no Create Sync Cancel is
  // sent. The failed entry is removed from the advertiser list directly.
  auto remove_from_list_packet =
      bt::testing::LERemoveDeviceFromPeriodicAdvertiserListPacket(addr,
                                                                  kAdvSid);
  auto remove_from_list_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::
          LE_REMOVE_DEVICE_FROM_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), remove_from_list_packet, &remove_from_list_complete);

  test_device()->SendCommandChannelPacket(kMalformedEvent);
  RunUntilIdle();

  // Verify that the pending sync request is failed with a kPacketMalformed
  // error.
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());
  EXPECT_EQ(delegate.sync_established_count(), 0);
  EXPECT_EQ(delegate.sync_lost_count(), 1);
  ASSERT_TRUE(delegate.last_error().has_value());
  EXPECT_EQ(delegate.last_error().value(), Error(HostError::kPacketMalformed));

  // Verify that the state machine is not wedged.
  TestDelegate recovery_delegate;
  constexpr hci_spec::SyncHandle kSyncHandle = 0x01;
  std::optional<PeriodicAdvertisingSync> recovered_sync =
      CreateSyncAndExpectSuccess(recovery_delegate, addr, kAdvSid, kSyncHandle);
  ASSERT_TRUE(recovered_sync.has_value());
  ExpectTerminateSync(test_device(), kSyncHandle);
}

TEST_F(PeriodicAdvertisingSynchronizerTest, CreateSyncFailure) {
  TestDelegate delegate;
  DeviceAddress addr(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid = 12;

  auto add_to_list_packet =
      bt::testing::LEAddDeviceToPeriodicAdvertiserListPacket(addr, kAdvSid);
  auto add_to_list_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_ADD_DEVICE_TO_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), add_to_list_packet, &add_to_list_complete);

  auto command_status_rsp = bt::testing::CommandStatusPacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC,
      pw::bluetooth::emboss::StatusCode::COMMAND_DISALLOWED);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        CreateSyncPacket(/*filter_duplicates=*/false,
                                         /*use_periodic_advertiser_list=*/true),
                        &command_status_rsp);

  Result<PeriodicAdvertisingSync> sync = synchronizer()->CreateSync(
      addr, kAdvSid, {.filter_duplicates = false}, delegate);
  EXPECT_TRUE(sync.is_ok());
  RunUntilIdle();
  EXPECT_EQ(delegate.sync_established_count(), 0);
  EXPECT_EQ(delegate.sync_lost_count(), 1);
  ASSERT_TRUE(delegate.last_error().has_value());
  EXPECT_EQ(delegate.last_error().value(), Error(HostError::kFailed));

  // Future requests should fail because PeriodicAdvertisingSynchronizer is in a
  // bad state.
  Result<PeriodicAdvertisingSync> sync2 = synchronizer()->CreateSync(
      addr, kAdvSid, {.filter_duplicates = false}, delegate);
  EXPECT_TRUE(sync2.is_error());
}

TEST_F(PeriodicAdvertisingSynchronizerTest, CancelEstablishedSync) {
  TestDelegate delegate;
  DeviceAddress addr(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid = 12;
  constexpr hci_spec::ConnectionHandle kSyncHandle = 0x01;

  auto sync = CreateSyncAndExpectSuccess(delegate, addr, kAdvSid, kSyncHandle);
  ASSERT_TRUE(sync.has_value());

  auto terminate_sync_packet =
      bt::testing::LEPeriodicAdvertisingTerminateSyncPacket(kSyncHandle);
  auto terminate_sync_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_TERMINATE_SYNC,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), terminate_sync_packet, &terminate_sync_complete);

  sync.value().Cancel();
  RunUntilIdle();

  auto sync_lost_event = bt::testing::LESyncLostEventPacket(kSyncHandle);
  test_device()->SendCommandChannelPacket(sync_lost_event);
  RunUntilIdle();

  EXPECT_EQ(delegate.sync_lost_count(), 1);
  ASSERT_TRUE(delegate.last_error().has_value());
  EXPECT_EQ(delegate.last_error().value(),
            ToResult(HostError::kCanceled).error_value());
}

TEST_F(PeriodicAdvertisingSynchronizerTest, SyncLost) {
  TestDelegate delegate;
  DeviceAddress addr(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid = 12;
  constexpr hci_spec::ConnectionHandle kSyncHandle = 0x01;

  auto sync = CreateSyncAndExpectSuccess(delegate, addr, kAdvSid, kSyncHandle);
  ASSERT_TRUE(sync.has_value());

  auto sync_lost_event = bt::testing::LESyncLostEventPacket(kSyncHandle);
  test_device()->SendCommandChannelPacket(sync_lost_event);
  RunUntilIdle();

  EXPECT_EQ(delegate.sync_lost_count(), 1);
  ASSERT_TRUE(delegate.last_error().has_value());
  EXPECT_EQ(delegate.last_error().value(),
            ToResult(pw::bluetooth::emboss::StatusCode::CONNECTION_TIMEOUT)
                .error_value());
}

TEST_F(PeriodicAdvertisingSynchronizerTest, AdvertisingReportV1) {
  TestDelegate delegate;
  DeviceAddress addr(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid = 12;
  constexpr hci_spec::ConnectionHandle kSyncHandle = 0x01;

  auto sync = CreateSyncAndExpectSuccess(delegate, addr, kAdvSid, kSyncHandle);
  ASSERT_TRUE(sync.has_value());

  auto advertising_report_event =
      bt::testing::LEPeriodicAdvertisingReportEventPacketV1(
          kSyncHandle,
          pw::bluetooth::emboss::LEPeriodicAdvertisingDataStatus::COMPLETE,
          DynamicByteBuffer({1, 2, 3}));
  test_device()->SendCommandChannelPacket(advertising_report_event);
  RunUntilIdle();

  EXPECT_EQ(delegate.report_count(), 1);
  ASSERT_TRUE(delegate.last_report().has_value());
  EXPECT_EQ(delegate.last_report()->data, DynamicByteBuffer({1, 2, 3}));
  EXPECT_FALSE(delegate.last_report()->event_counter.has_value());

  ExpectTerminateSync(test_device(), kSyncHandle);
}

TEST_F(PeriodicAdvertisingSynchronizerTest, AdvertisingReportV2) {
  TestDelegate delegate;
  DeviceAddress addr(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid = 12;
  constexpr hci_spec::ConnectionHandle kSyncHandle = 0x01;

  auto sync = CreateSyncAndExpectSuccess(delegate, addr, kAdvSid, kSyncHandle);
  ASSERT_TRUE(sync.has_value());

  constexpr uint16_t kEventCounter = 0x1234;
  constexpr uint8_t kSubevent = 0x05;
  auto advertising_report_event =
      bt::testing::LEPeriodicAdvertisingReportEventPacketV2(
          kSyncHandle,
          kEventCounter,
          kSubevent,
          pw::bluetooth::emboss::LEPeriodicAdvertisingDataStatus::COMPLETE,
          DynamicByteBuffer({1, 2, 3}));
  test_device()->SendCommandChannelPacket(advertising_report_event);
  RunUntilIdle();

  EXPECT_EQ(delegate.report_count(), 1);
  ASSERT_TRUE(delegate.last_report().has_value());
  EXPECT_EQ(delegate.last_report()->data, DynamicByteBuffer({1, 2, 3}));
  ASSERT_TRUE(delegate.last_report()->event_counter.has_value());
  EXPECT_EQ(delegate.last_report()->event_counter.value(), kEventCounter);

  ExpectTerminateSync(test_device(), kSyncHandle);
}

TEST_F(PeriodicAdvertisingSynchronizerTest, BigInfoReport) {
  TestDelegate delegate;
  DeviceAddress addr(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid = 12;
  constexpr hci_spec::ConnectionHandle kSyncHandle = 0x01;

  auto sync = CreateSyncAndExpectSuccess(delegate, addr, kAdvSid, kSyncHandle);
  ASSERT_TRUE(sync.has_value());

  auto big_info_report_event =
      bt::testing::LEBigInfoAdvertisingReportEventPacket(
          kSyncHandle,
          /*num_bis=*/1,
          /*nse=*/2,
          /*iso_interval=*/3,
          /*bn=*/4,
          /*pto=*/5,
          /*irc=*/6,
          /*max_pdu=*/7,
          /*sdu_interval=*/8,
          /*max_sdu=*/9,
          pw::bluetooth::emboss::IsoPhyType::LE_2M,
          pw::bluetooth::emboss::BigFraming::FRAMED,
          /*encryption=*/true);
  test_device()->SendCommandChannelPacket(big_info_report_event);
  RunUntilIdle();

  EXPECT_EQ(delegate.big_info_count(), 1);
  ASSERT_TRUE(delegate.last_big_info().has_value());
  PeriodicAdvertisingSynchronizer::BroadcastIsochronousGroupInfo info =
      delegate.last_big_info().value();
  EXPECT_EQ(info.num_bis, 1u);
  EXPECT_EQ(info.nse, 2);
  EXPECT_EQ(info.iso_interval, 3);
  EXPECT_EQ(info.bn, 4);
  EXPECT_EQ(info.pto, 5);
  EXPECT_EQ(info.irc, 6);
  EXPECT_EQ(info.max_pdu, 7);
  EXPECT_EQ(info.sdu_interval, 8u);
  EXPECT_EQ(info.max_sdu, 9);
  EXPECT_EQ(info.phy, pw::bluetooth::emboss::IsoPhyType::LE_2M);
  EXPECT_EQ(info.framing, pw::bluetooth::emboss::BigFraming::FRAMED);
  EXPECT_TRUE(info.encryption);

  ExpectTerminateSync(test_device(), kSyncHandle);
}

TEST_F(PeriodicAdvertisingSynchronizerTest, CreateSyncFilterDuplicates) {
  TestDelegate delegate;
  DeviceAddress addr(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid = 12;
  constexpr hci_spec::ConnectionHandle kSyncHandle = 0x01;

  auto sync = CreateSyncAndExpectSuccess(delegate,
                                         addr,
                                         kAdvSid,
                                         kSyncHandle,
                                         /*v2=*/false,
                                         /*filter_duplicates=*/true);
  ASSERT_TRUE(sync.has_value());

  ExpectTerminateSync(test_device(), kSyncHandle);
}

TEST_F(PeriodicAdvertisingSynchronizerTest, FragmentedAdvertisingReport) {
  TestDelegate delegate;
  DeviceAddress addr(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid = 12;
  constexpr hci_spec::ConnectionHandle kSyncHandle = 0x01;

  auto sync = CreateSyncAndExpectSuccess(delegate, addr, kAdvSid, kSyncHandle);
  ASSERT_TRUE(sync.has_value());

  auto advertising_report_event =
      bt::testing::LEPeriodicAdvertisingReportEventPacketV1(
          kSyncHandle,
          pw::bluetooth::emboss::LEPeriodicAdvertisingDataStatus::INCOMPLETE,
          DynamicByteBuffer({1, 2, 3}));
  test_device()->SendCommandChannelPacket(advertising_report_event);
  RunUntilIdle();

  EXPECT_EQ(delegate.report_count(), 0);

  auto advertising_report_event2 =
      bt::testing::LEPeriodicAdvertisingReportEventPacketV1(
          kSyncHandle,
          pw::bluetooth::emboss::LEPeriodicAdvertisingDataStatus::COMPLETE,
          DynamicByteBuffer({4, 5, 6}));
  test_device()->SendCommandChannelPacket(advertising_report_event2);
  RunUntilIdle();

  EXPECT_EQ(delegate.report_count(), 1);
  ASSERT_TRUE(delegate.last_report().has_value());
  EXPECT_EQ(delegate.last_report()->data,
            DynamicByteBuffer({1, 2, 3, 4, 5, 6}));

  ExpectTerminateSync(test_device(), kSyncHandle);
}

TEST_F(PeriodicAdvertisingSynchronizerTest, ReassemblyExceedingSpecLimitFails) {
  TestDelegate delegate;
  DeviceAddress addr(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid = 12;
  constexpr hci_spec::ConnectionHandle kSyncHandle = 0x01;

  auto sync = CreateSyncAndExpectSuccess(delegate, addr, kAdvSid, kSyncHandle);
  ASSERT_TRUE(sync.has_value());

  // Send fragments exceeding 1650 bytes.
  // 10 fragments of 200 bytes = 2000 bytes.
  const size_t kFragments = 10;

  for (size_t i = 0; i < kFragments; ++i) {
    auto advertising_report_event =
        bt::testing::LEPeriodicAdvertisingReportEventPacketV1(
            kSyncHandle,
            pw::bluetooth::emboss::LEPeriodicAdvertisingDataStatus::INCOMPLETE,
            DynamicByteBuffer(200));
    test_device()->SendCommandChannelPacket(advertising_report_event);
    RunUntilIdle();
  }

  // Send final fragment to complete it.
  auto advertising_report_event2 =
      bt::testing::LEPeriodicAdvertisingReportEventPacketV1(
          kSyncHandle,
          pw::bluetooth::emboss::LEPeriodicAdvertisingDataStatus::COMPLETE,
          DynamicByteBuffer({4, 5, 6}));
  test_device()->SendCommandChannelPacket(advertising_report_event2);
  RunUntilIdle();

  // We expect it drop the report because it exceeded the limit.
  EXPECT_EQ(delegate.report_count(), 0);

  ExpectTerminateSync(test_device(), kSyncHandle);
}

TEST_F(PeriodicAdvertisingSynchronizerTest,
       IncompleteTruncatedAdvertisingReport) {
  TestDelegate delegate;
  DeviceAddress addr(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid = 12;
  constexpr hci_spec::ConnectionHandle kSyncHandle = 0x01;

  auto sync = CreateSyncAndExpectSuccess(delegate, addr, kAdvSid, kSyncHandle);
  ASSERT_TRUE(sync.has_value());

  auto advertising_report_event =
      bt::testing::LEPeriodicAdvertisingReportEventPacketV1(
          kSyncHandle,
          pw::bluetooth::emboss::LEPeriodicAdvertisingDataStatus::
              INCOMPLETE_TRUNCATED,
          DynamicByteBuffer({1, 2, 3}));
  test_device()->SendCommandChannelPacket(advertising_report_event);
  RunUntilIdle();

  EXPECT_EQ(delegate.report_count(), 0);

  DynamicByteBuffer data2({4, 5, 6});
  auto advertising_report_event2 =
      bt::testing::LEPeriodicAdvertisingReportEventPacketV1(
          kSyncHandle,
          pw::bluetooth::emboss::LEPeriodicAdvertisingDataStatus::COMPLETE,
          data2);
  test_device()->SendCommandChannelPacket(advertising_report_event2);
  RunUntilIdle();

  EXPECT_EQ(delegate.report_count(), 1);
  ASSERT_TRUE(delegate.last_report().has_value());
  EXPECT_EQ(delegate.last_report()->data, data2);

  ExpectTerminateSync(test_device(), kSyncHandle);
}

TEST_F(PeriodicAdvertisingSynchronizerTest,
       CreateSyncQueuedWhenAdvertiserListFull) {
  TestDelegate delegate1;
  DeviceAddress addr1(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid1 = 12;

  auto sync1 = synchronizer()->CreateSync(
      addr1, kAdvSid1, {.filter_duplicates = false}, delegate1);
  EXPECT_TRUE(sync1.is_ok());

  auto add_to_list_packet1 =
      bt::testing::LEAddDeviceToPeriodicAdvertiserListPacket(addr1, kAdvSid1);
  auto add_to_list_complete1 = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_ADD_DEVICE_TO_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), add_to_list_packet1, &add_to_list_complete1);

  auto create_sync_packet1 = CreateSyncPacket(
      /*filter_duplicates=*/false,
      /*use_periodic_advertiser_list=*/true);
  auto create_sync_status1 = bt::testing::CommandStatusPacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), create_sync_packet1, &create_sync_status1);

  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());

  TestDelegate delegate2;
  DeviceAddress addr2(DeviceAddress::Type::kLEPublic, {2});
  constexpr uint8_t kAdvSid2 = 13;

  auto sync2 = synchronizer()->CreateSync(
      addr2, kAdvSid2, {.filter_duplicates = false}, delegate2);
  EXPECT_TRUE(sync2.is_ok());

  auto cancel_cmd = bt::testing::LEPeriodicAdvertisingCreateSyncCancelPacket();
  auto cancel_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC_CANCEL,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  auto sync_established_cancel_event =
      bt::testing::LEPeriodicAdvertisingSyncEstablishedEventPacketV1(
          pw::bluetooth::emboss::StatusCode::OPERATION_CANCELLED_BY_HOST,
          /*handle=*/0,
          /*sid=*/0,
          DeviceAddress(DeviceAddress::Type::kLEPublic, {0}),
          pw::bluetooth::emboss::LEPhy::LE_1M,
          /*interval=*/0x0006,
          pw::bluetooth::emboss::LEClockAccuracy::PPM_500);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        cancel_cmd,
                        &cancel_complete,
                        &sync_established_cancel_event);

  auto add_to_list_packet2 =
      bt::testing::LEAddDeviceToPeriodicAdvertiserListPacket(addr2, kAdvSid2);
  auto add_to_list_complete_failure2 = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_ADD_DEVICE_TO_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::MEMORY_CAPACITY_EXCEEDED);
  EXPECT_CMD_PACKET_OUT(
      test_device(), add_to_list_packet2, &add_to_list_complete_failure2);

  auto create_sync_packet2 = CreateSyncPacket(
      /*filter_duplicates=*/false,
      /*use_periodic_advertiser_list=*/true);
  auto create_sync_status2 = bt::testing::CommandStatusPacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), create_sync_packet2, &create_sync_status2);

  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());
  EXPECT_EQ(delegate1.sync_established_count(), 0);
  EXPECT_EQ(delegate2.sync_established_count(), 0);

  constexpr hci_spec::ConnectionHandle kSyncHandle1 = 0x01;
  constexpr uint16_t kSyncPacketInterval1 = 0x000A;
  auto sync_established_event1 =
      bt::testing::LEPeriodicAdvertisingSyncEstablishedEventPacketV1(
          pw::bluetooth::emboss::StatusCode::SUCCESS,
          kSyncHandle1,
          kAdvSid1,
          addr1,
          pw::bluetooth::emboss::LEPhy::LE_1M,
          kSyncPacketInterval1,
          pw::bluetooth::emboss::LEClockAccuracy::PPM_500);

  auto remove_from_list_packet1 =
      bt::testing::LERemoveDeviceFromPeriodicAdvertiserListPacket(addr1,
                                                                  kAdvSid1);
  auto remove_from_list_complete1 = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::
          LE_REMOVE_DEVICE_FROM_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), remove_from_list_packet1, &remove_from_list_complete1);

  auto add_to_list_complete2 = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_ADD_DEVICE_TO_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), add_to_list_packet2, &add_to_list_complete2);

  auto create_sync_packet3 = CreateSyncPacket(
      /*filter_duplicates=*/false,
      /*use_periodic_advertiser_list=*/true);
  auto create_sync_status3 = bt::testing::CommandStatusPacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), create_sync_packet3, &create_sync_status3);

  test_device()->SendCommandChannelPacket(sync_established_event1);
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());
  EXPECT_EQ(delegate1.sync_established_count(), 1);
  EXPECT_EQ(delegate1.sync_lost_count(), 0);
  EXPECT_EQ(delegate2.sync_established_count(), 0);
  EXPECT_EQ(delegate2.sync_lost_count(), 0);

  constexpr hci_spec::ConnectionHandle kSyncHandle2 = 0x02;
  constexpr uint16_t kSyncPacketInterval2 = 0x000B;
  auto sync_established_event2 =
      bt::testing::LEPeriodicAdvertisingSyncEstablishedEventPacketV1(
          pw::bluetooth::emboss::StatusCode::SUCCESS,
          kSyncHandle2,
          kAdvSid2,
          addr2,
          pw::bluetooth::emboss::LEPhy::LE_1M,
          kSyncPacketInterval2,
          pw::bluetooth::emboss::LEClockAccuracy::PPM_500);

  auto remove_from_list_packet2 =
      bt::testing::LERemoveDeviceFromPeriodicAdvertiserListPacket(addr2,
                                                                  kAdvSid2);
  auto remove_from_list_complete2 = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::
          LE_REMOVE_DEVICE_FROM_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), remove_from_list_packet2, &remove_from_list_complete2);

  test_device()->SendCommandChannelPacket(sync_established_event2);
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());
  EXPECT_EQ(delegate1.sync_established_count(), 1);
  EXPECT_EQ(delegate1.sync_lost_count(), 0);
  EXPECT_EQ(delegate2.sync_established_count(), 1);
  EXPECT_EQ(delegate2.sync_lost_count(), 0);

  ExpectTerminateSync(test_device(), kSyncHandle2);
  ExpectTerminateSync(test_device(), kSyncHandle1);
}

TEST_F(PeriodicAdvertisingSynchronizerTest, AdvertiserListFullErrorWhenEmpty) {
  TestDelegate delegate1;
  DeviceAddress addr1(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid1 = 12;

  auto add_to_list_packet1 =
      bt::testing::LEAddDeviceToPeriodicAdvertiserListPacket(addr1, kAdvSid1);
  auto add_to_list_complete1 = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_ADD_DEVICE_TO_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::MEMORY_CAPACITY_EXCEEDED);
  EXPECT_CMD_PACKET_OUT(
      test_device(), add_to_list_packet1, &add_to_list_complete1);

  Result<PeriodicAdvertisingSync> sync1 = synchronizer()->CreateSync(
      addr1, kAdvSid1, {.filter_duplicates = false}, delegate1);
  EXPECT_TRUE(sync1.is_ok());

  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());
  EXPECT_EQ(delegate1.sync_lost_count(), 1);

  TestDelegate delegate2;
  DeviceAddress addr2(DeviceAddress::Type::kLEPublic, {2});
  constexpr uint8_t kAdvSid2 = 13;

  auto add_to_list_packet2 =
      bt::testing::LEAddDeviceToPeriodicAdvertiserListPacket(addr2, kAdvSid2);
  auto add_to_list_complete2 = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_ADD_DEVICE_TO_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::MEMORY_CAPACITY_EXCEEDED);
  EXPECT_CMD_PACKET_OUT(
      test_device(), add_to_list_packet2, &add_to_list_complete2);

  Result<PeriodicAdvertisingSync> sync2 = synchronizer()->CreateSync(
      addr2, kAdvSid2, {.filter_duplicates = false}, delegate2);
  EXPECT_TRUE(sync2.is_ok());
  RunUntilIdle();
  EXPECT_EQ(delegate2.sync_lost_count(), 1);
}

TEST_F(PeriodicAdvertisingSynchronizerTest, CreateSyncMemoryCapacityExceeded) {
  TestDelegate delegate1;
  DeviceAddress addr1(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid1 = 12;

  auto sync1 = synchronizer()->CreateSync(
      addr1, kAdvSid1, {.filter_duplicates = false}, delegate1);
  EXPECT_TRUE(sync1.is_ok());

  auto add_to_list_packet =
      bt::testing::LEAddDeviceToPeriodicAdvertiserListPacket(addr1, kAdvSid1);
  auto add_to_list_complete = bt::testing::CommandCompletePacket(
      hci_spec::kLEAddDeviceToPeriodicAdvertiserList,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), add_to_list_packet, &add_to_list_complete);

  auto command_status_rsp1 = bt::testing::CommandStatusPacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC,
      pw::bluetooth::emboss::StatusCode::MEMORY_CAPACITY_EXCEEDED);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        CreateSyncPacket(
                            /*filter_duplicates=*/false,
                            /*use_periodic_advertiser_list=*/true),
                        &command_status_rsp1);

  auto remove_from_list_packet =
      bt::testing::LERemoveDeviceFromPeriodicAdvertiserListPacket(addr1,
                                                                  kAdvSid1);
  auto remove_from_list_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::
          LE_REMOVE_DEVICE_FROM_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), remove_from_list_packet, &remove_from_list_complete);

  RunUntilIdle();
  EXPECT_EQ(delegate1.sync_established_count(), 0);
  EXPECT_EQ(delegate1.sync_lost_count(), 1);
  EXPECT_EQ(delegate1.last_error().value(), Error(HostError::kFailed));
}

TEST_F(PeriodicAdvertisingSynchronizerTest, CancelQueuedCreateSync) {
  TestDelegate delegate1;
  DeviceAddress addr1(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid1 = 12;
  auto sync1 = synchronizer()->CreateSync(
      addr1, kAdvSid1, {.filter_duplicates = true}, delegate1);
  EXPECT_TRUE(sync1.is_ok());

  TestDelegate delegate2;
  DeviceAddress addr2(DeviceAddress::Type::kLEPublic, {2});
  constexpr uint8_t kAdvSid2 = 13;
  auto sync2 = synchronizer()->CreateSync(
      addr2, kAdvSid2, {.filter_duplicates = false}, delegate2);
  EXPECT_TRUE(sync2.is_ok());

  auto add_to_list_packet =
      bt::testing::LEAddDeviceToPeriodicAdvertiserListPacket(addr1, kAdvSid1);
  auto add_to_list_complete = bt::testing::CommandCompletePacket(
      hci_spec::kLEAddDeviceToPeriodicAdvertiserList,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), add_to_list_packet, &add_to_list_complete);

  auto command_status_rsp1 = bt::testing::CommandStatusPacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        CreateSyncPacket(
                            /*filter_duplicates=*/true, true),
                        &command_status_rsp1);

  RunUntilIdle();
  EXPECT_EQ(delegate1.sync_established_count(), 0);
  EXPECT_EQ(delegate1.sync_lost_count(), 0);
  EXPECT_EQ(delegate2.sync_established_count(), 0);
  EXPECT_EQ(delegate2.sync_lost_count(), 0);

  // No Remove Device from Periodic Advertiser List command should be sent.
  sync2.value().Cancel();
  RunUntilIdle();

  // The delegate should be notified of sync lost with canceled error.
  EXPECT_EQ(delegate2.sync_lost_count(), 1);
  EXPECT_EQ(delegate2.last_error(), ToResult(HostError::kCanceled));

  auto cancel_cmd = bt::testing::LEPeriodicAdvertisingCreateSyncCancelPacket();
  auto cancel_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC_CANCEL,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  auto sync_established_cancel_event =
      bt::testing::LEPeriodicAdvertisingSyncEstablishedEventPacketV1(
          pw::bluetooth::emboss::StatusCode::OPERATION_CANCELLED_BY_HOST,
          /*sync_handle=*/0,
          /*advertising_sid=*/0,
          DeviceAddress(DeviceAddress::Type::kLEPublic, {0}),
          pw::bluetooth::emboss::LEPhy::LE_1M,
          /*interval=*/0x0006,
          pw::bluetooth::emboss::LEClockAccuracy::PPM_500);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        cancel_cmd,
                        &cancel_complete,
                        &sync_established_cancel_event);

  auto remove_from_list_packet =
      bt::testing::LERemoveDeviceFromPeriodicAdvertiserListPacket(addr1,
                                                                  kAdvSid1);
  auto remove_from_list_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::
          LE_REMOVE_DEVICE_FROM_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), remove_from_list_packet, &remove_from_list_complete);

  sync1.value().Cancel();
  RunUntilIdle();
  EXPECT_EQ(delegate2.sync_lost_count(), 1);
  EXPECT_EQ(delegate2.last_error(), ToResult(HostError::kCanceled));
}

// Create 2 sync requests with filter_duplicates = true and 1 request with
// filter_duplicates = false.
TEST_F(PeriodicAdvertisingSynchronizerTest, TwoCreateSyncRequestsSimultaneous) {
  TestDelegate delegate1;
  DeviceAddress addr1(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid1 = 12;

  auto sync1 = synchronizer()->CreateSync(
      addr1, kAdvSid1, {.filter_duplicates = true}, delegate1);
  EXPECT_TRUE(sync1.is_ok());

  auto add_to_list_packet1 =
      bt::testing::LEAddDeviceToPeriodicAdvertiserListPacket(addr1, kAdvSid1);
  auto add_to_list_complete1 = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_ADD_DEVICE_TO_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), add_to_list_packet1, &add_to_list_complete1);

  auto create_sync_packet1 = CreateSyncPacket(
      /*filter_duplicates=*/true,
      /*use_periodic_advertiser_list=*/true);
  auto create_sync_status1 = bt::testing::CommandStatusPacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), create_sync_packet1, &create_sync_status1);

  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());

  TestDelegate delegate2;
  DeviceAddress addr2(DeviceAddress::Type::kLEPublic, {2});
  constexpr uint8_t kAdvSid2 = 13;

  auto sync2 = synchronizer()->CreateSync(
      addr2, kAdvSid2, {.filter_duplicates = true}, delegate2);
  EXPECT_TRUE(sync2.is_ok());

  auto cancel_cmd = bt::testing::LEPeriodicAdvertisingCreateSyncCancelPacket();
  auto cancel_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC_CANCEL,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  auto sync_established_cancel_event =
      bt::testing::LEPeriodicAdvertisingSyncEstablishedEventPacketV1(
          pw::bluetooth::emboss::StatusCode::OPERATION_CANCELLED_BY_HOST,
          /*sync_handle=*/0,
          /*advertising_sid=*/0,
          DeviceAddress(DeviceAddress::Type::kLEPublic, {0}),
          pw::bluetooth::emboss::LEPhy::LE_1M,
          /*interval=*/0x0006,
          pw::bluetooth::emboss::LEClockAccuracy::PPM_500);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        cancel_cmd,
                        &cancel_complete,
                        &sync_established_cancel_event);

  auto add_to_list_packet2 =
      bt::testing::LEAddDeviceToPeriodicAdvertiserListPacket(addr2, kAdvSid2);
  auto add_to_list_complete2 = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_ADD_DEVICE_TO_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), add_to_list_packet2, &add_to_list_complete2);

  auto create_sync_packet2 = CreateSyncPacket(
      /*filter_duplicates=*/true,
      /*use_periodic_advertiser_list=*/true);
  auto create_sync_status2 = bt::testing::CommandStatusPacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), create_sync_packet2, &create_sync_status2);

  TestDelegate delegate3;
  DeviceAddress addr3(DeviceAddress::Type::kLEPublic, {3});
  constexpr uint8_t kAdvSid3 = 14;
  // Create sync3 without running the loop as an extra test.
  // sync3 should be queued because it has a different filter setting.
  auto sync3 = synchronizer()->CreateSync(
      addr3, kAdvSid3, {.filter_duplicates = false}, delegate3);
  EXPECT_TRUE(sync3.is_ok());

  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());

  EXPECT_EQ(delegate1.sync_established_count(), 0);
  EXPECT_EQ(delegate2.sync_established_count(), 0);

  constexpr hci_spec::ConnectionHandle kSyncHandle1 = 0x01;
  constexpr uint16_t kSyncPacketInterval1 = 0x000A;
  auto sync_established_event1 =
      bt::testing::LEPeriodicAdvertisingSyncEstablishedEventPacketV1(
          pw::bluetooth::emboss::StatusCode::SUCCESS,
          kSyncHandle1,
          kAdvSid1,
          addr1,
          pw::bluetooth::emboss::LEPhy::LE_1M,
          kSyncPacketInterval1,
          pw::bluetooth::emboss::LEClockAccuracy::PPM_500);

  auto remove_from_list_packet1 =
      bt::testing::LERemoveDeviceFromPeriodicAdvertiserListPacket(addr1,
                                                                  kAdvSid1);
  auto remove_from_list_complete1 = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::
          LE_REMOVE_DEVICE_FROM_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), remove_from_list_packet1, &remove_from_list_complete1);

  auto create_sync_packet3 = CreateSyncPacket(
      /*filter_duplicates=*/true,
      /*use_periodic_advertiser_list=*/true);
  auto create_sync_status3 = bt::testing::CommandStatusPacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), create_sync_packet3, &create_sync_status3);

  test_device()->SendCommandChannelPacket(sync_established_event1);
  RunUntilIdle();

  EXPECT_EQ(delegate1.sync_established_count(), 1);
  EXPECT_EQ(delegate1.sync_lost_count(), 0);
  EXPECT_EQ(delegate2.sync_established_count(), 0);
  EXPECT_EQ(delegate2.sync_lost_count(), 0);
  EXPECT_EQ(delegate3.sync_established_count(), 0);
  EXPECT_EQ(delegate3.sync_lost_count(), 0);

  constexpr hci_spec::ConnectionHandle kSyncHandle2 = 0x02;
  constexpr uint16_t kSyncPacketInterval2 = 0x000B;
  auto sync_established_event2 =
      bt::testing::LEPeriodicAdvertisingSyncEstablishedEventPacketV1(
          pw::bluetooth::emboss::StatusCode::SUCCESS,
          kSyncHandle2,
          kAdvSid2,
          addr2,
          pw::bluetooth::emboss::LEPhy::LE_1M,
          kSyncPacketInterval2,
          pw::bluetooth::emboss::LEClockAccuracy::PPM_500);

  auto remove_from_list_packet2 =
      bt::testing::LERemoveDeviceFromPeriodicAdvertiserListPacket(addr2,
                                                                  kAdvSid2);
  auto remove_from_list_complete2 = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::
          LE_REMOVE_DEVICE_FROM_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), remove_from_list_packet2, &remove_from_list_complete2);

  auto add_to_list_packet3 =
      bt::testing::LEAddDeviceToPeriodicAdvertiserListPacket(addr3, kAdvSid3);
  auto add_to_list_complete3 = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_ADD_DEVICE_TO_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), add_to_list_packet3, &add_to_list_complete3);

  auto create_sync_packet4 = CreateSyncPacket(
      /*filter_duplicates=*/false,
      /*use_periodic_advertiser_list=*/true);
  auto create_sync_status4 = bt::testing::CommandStatusPacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), create_sync_packet4, &create_sync_status4);

  test_device()->SendCommandChannelPacket(sync_established_event2);
  RunUntilIdle();

  EXPECT_EQ(delegate1.sync_established_count(), 1);
  EXPECT_EQ(delegate1.sync_lost_count(), 0);
  EXPECT_EQ(delegate2.sync_established_count(), 1);
  EXPECT_EQ(delegate2.sync_lost_count(), 0);

  constexpr hci_spec::ConnectionHandle kSyncHandle3 = 0x03;
  constexpr uint16_t kSyncPacketInterval3 = 0x000B;
  auto sync_established_event3 =
      bt::testing::LEPeriodicAdvertisingSyncEstablishedEventPacketV1(
          pw::bluetooth::emboss::StatusCode::SUCCESS,
          kSyncHandle3,
          kAdvSid3,
          addr3,
          pw::bluetooth::emboss::LEPhy::LE_1M,
          kSyncPacketInterval3,
          pw::bluetooth::emboss::LEClockAccuracy::PPM_500);

  auto remove_from_list_packet3 =
      bt::testing::LERemoveDeviceFromPeriodicAdvertiserListPacket(addr3,
                                                                  kAdvSid3);
  auto remove_from_list_complete3 = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::
          LE_REMOVE_DEVICE_FROM_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), remove_from_list_packet3, &remove_from_list_complete3);

  test_device()->SendCommandChannelPacket(sync_established_event3);
  RunUntilIdle();

  ExpectTerminateSync(test_device(), kSyncHandle3);
  ExpectTerminateSync(test_device(), kSyncHandle2);
  ExpectTerminateSync(test_device(), kSyncHandle1);
}

TEST_F(PeriodicAdvertisingSynchronizerTest,
       CreateSyncFailureInSyncEstablishedEvent) {
  TestDelegate delegate;
  DeviceAddress addr(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid = 12;

  auto sync = synchronizer()->CreateSync(
      addr, kAdvSid, {.filter_duplicates = false}, delegate);
  EXPECT_TRUE(sync.is_ok());

  auto add_to_list_packet =
      bt::testing::LEAddDeviceToPeriodicAdvertiserListPacket(addr, kAdvSid);
  auto add_to_list_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_ADD_DEVICE_TO_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), add_to_list_packet, &add_to_list_complete);

  auto command_status_rsp = bt::testing::CommandStatusPacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), CreateSyncPacket(false, true), &command_status_rsp);
  RunUntilIdle();

  constexpr hci_spec::ConnectionHandle kSyncHandle = 0x01;
  constexpr uint16_t kSyncPacketInterval = 0x000A;
  auto sync_established_event =
      bt::testing::LEPeriodicAdvertisingSyncEstablishedEventPacketV1(
          pw::bluetooth::emboss::StatusCode::
              CONNECTION_FAILED_TO_BE_ESTABLISHED,
          kSyncHandle,
          kAdvSid,
          addr,
          pw::bluetooth::emboss::LEPhy::LE_1M,
          kSyncPacketInterval,
          pw::bluetooth::emboss::LEClockAccuracy::PPM_500);

  auto remove_from_list_packet =
      bt::testing::LERemoveDeviceFromPeriodicAdvertiserListPacket(addr,
                                                                  kAdvSid);
  auto remove_from_list_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::
          LE_REMOVE_DEVICE_FROM_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), remove_from_list_packet, &remove_from_list_complete);

  test_device()->SendCommandChannelPacket(sync_established_event);
  RunUntilIdle();

  EXPECT_EQ(delegate.sync_established_count(), 0);
  EXPECT_EQ(delegate.sync_lost_count(), 1);
  ASSERT_TRUE(delegate.last_error().has_value());
  EXPECT_EQ(delegate.last_error().value(),
            ToResult(pw::bluetooth::emboss::StatusCode::
                         CONNECTION_FAILED_TO_BE_ESTABLISHED)
                .error_value());
}

TEST_F(PeriodicAdvertisingSynchronizerTest, CancelCreateSync) {
  TestDelegate delegate;
  DeviceAddress addr(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid = 12;

  auto sync = synchronizer()->CreateSync(
      addr, kAdvSid, {.filter_duplicates = false}, delegate);
  ASSERT_TRUE(sync.is_ok());

  auto add_to_list_packet =
      bt::testing::LEAddDeviceToPeriodicAdvertiserListPacket(addr, kAdvSid);
  auto add_to_list_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_ADD_DEVICE_TO_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), add_to_list_packet, &add_to_list_complete);

  auto create_sync_packet = CreateSyncPacket(
      /*filter_duplicates=*/false,
      /*use_periodic_advertiser_list=*/true);
  auto create_sync_status = bt::testing::CommandStatusPacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(test_device(), create_sync_packet, &create_sync_status);

  RunUntilIdle();

  auto cancel_cmd = bt::testing::LEPeriodicAdvertisingCreateSyncCancelPacket();
  auto cancel_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC_CANCEL,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  auto sync_established_cancel_event =
      bt::testing::LEPeriodicAdvertisingSyncEstablishedEventPacketV1(
          pw::bluetooth::emboss::StatusCode::OPERATION_CANCELLED_BY_HOST,
          /*sync_handle=*/0,
          /*advertising_sid=*/0,
          DeviceAddress(DeviceAddress::Type::kLEPublic, {0}),
          pw::bluetooth::emboss::LEPhy::LE_1M,
          /*interval=*/0x0006,
          pw::bluetooth::emboss::LEClockAccuracy::PPM_500);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        cancel_cmd,
                        &cancel_complete,
                        &sync_established_cancel_event);

  auto remove_from_list_packet =
      bt::testing::LERemoveDeviceFromPeriodicAdvertiserListPacket(addr,
                                                                  kAdvSid);
  auto remove_from_list_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::
          LE_REMOVE_DEVICE_FROM_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), remove_from_list_packet, &remove_from_list_complete);

  sync->Cancel();
  RunUntilIdle();

  EXPECT_EQ(delegate.sync_lost_count(), 1);
  EXPECT_EQ(delegate.last_error().value(), Error(HostError::kCanceled));
}

TEST_F(PeriodicAdvertisingSynchronizerTest,
       CanceledCreateSyncSuccessReceivesSyncEstablishedSuccess) {
  TestDelegate delegate;
  DeviceAddress addr(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid = 12;

  auto sync = synchronizer()->CreateSync(
      addr, kAdvSid, {.filter_duplicates = false}, delegate);
  ASSERT_TRUE(sync.is_ok());

  auto add_to_list_packet =
      bt::testing::LEAddDeviceToPeriodicAdvertiserListPacket(addr, kAdvSid);
  auto add_to_list_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_ADD_DEVICE_TO_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), add_to_list_packet, &add_to_list_complete);

  auto create_sync_packet = CreateSyncPacket(
      /*filter_duplicates=*/false,
      /*use_periodic_advertiser_list=*/true);
  auto create_sync_status = bt::testing::CommandStatusPacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(test_device(), create_sync_packet, &create_sync_status);

  RunUntilIdle();

  auto cancel_cmd = bt::testing::LEPeriodicAdvertisingCreateSyncCancelPacket();
  auto cancel_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC_CANCEL,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  constexpr hci_spec::ConnectionHandle kSyncHandle = 0x01;
  constexpr uint16_t kSyncPacketInterval = 0x000A;
  auto sync_established_event =
      bt::testing::LEPeriodicAdvertisingSyncEstablishedEventPacketV1(
          pw::bluetooth::emboss::StatusCode::SUCCESS,
          kSyncHandle,
          kAdvSid,
          addr,
          pw::bluetooth::emboss::LEPhy::LE_1M,
          kSyncPacketInterval,
          pw::bluetooth::emboss::LEClockAccuracy::PPM_500);
  EXPECT_CMD_PACKET_OUT(
      test_device(), cancel_cmd, &cancel_complete, &sync_established_event);

  ExpectTerminateSync(test_device(), kSyncHandle);

  auto remove_from_list_packet =
      bt::testing::LERemoveDeviceFromPeriodicAdvertiserListPacket(addr,
                                                                  kAdvSid);
  auto remove_from_list_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::
          LE_REMOVE_DEVICE_FROM_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), remove_from_list_packet, &remove_from_list_complete);

  sync->Cancel();
  RunUntilIdle();

  EXPECT_EQ(delegate.sync_lost_count(), 1);
  EXPECT_EQ(delegate.last_error().value(), Error(HostError::kCanceled));
}

TEST_F(PeriodicAdvertisingSynchronizerTest,
       CanceledCreateSyncCommandDisallowedReceivesSyncEstablishedSuccess) {
  TestDelegate delegate;
  DeviceAddress addr(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid = 12;

  auto sync = synchronizer()->CreateSync(
      addr, kAdvSid, {.filter_duplicates = false}, delegate);
  ASSERT_TRUE(sync.is_ok());

  auto add_to_list_packet =
      bt::testing::LEAddDeviceToPeriodicAdvertiserListPacket(addr, kAdvSid);
  auto add_to_list_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_ADD_DEVICE_TO_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), add_to_list_packet, &add_to_list_complete);

  auto create_sync_packet = CreateSyncPacket(
      /*filter_duplicates=*/false,
      /*use_periodic_advertiser_list=*/true);
  auto create_sync_status = bt::testing::CommandStatusPacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(test_device(), create_sync_packet, &create_sync_status);

  RunUntilIdle();

  auto cancel_cmd = bt::testing::LEPeriodicAdvertisingCreateSyncCancelPacket();
  auto cancel_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC_CANCEL,
      pw::bluetooth::emboss::StatusCode::COMMAND_DISALLOWED);
  constexpr hci_spec::ConnectionHandle kSyncHandle = 0x01;
  constexpr uint16_t kSyncPacketInterval = 0x000A;
  auto sync_established_event =
      bt::testing::LEPeriodicAdvertisingSyncEstablishedEventPacketV1(
          pw::bluetooth::emboss::StatusCode::SUCCESS,
          kSyncHandle,
          kAdvSid,
          addr,
          pw::bluetooth::emboss::LEPhy::LE_1M,
          kSyncPacketInterval,
          pw::bluetooth::emboss::LEClockAccuracy::PPM_500);
  EXPECT_CMD_PACKET_OUT(
      test_device(), cancel_cmd, &cancel_complete, &sync_established_event);

  ExpectTerminateSync(test_device(), kSyncHandle);

  auto remove_from_list_packet =
      bt::testing::LERemoveDeviceFromPeriodicAdvertiserListPacket(addr,
                                                                  kAdvSid);
  auto remove_from_list_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::
          LE_REMOVE_DEVICE_FROM_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), remove_from_list_packet, &remove_from_list_complete);

  sync->Cancel();
  RunUntilIdle();

  EXPECT_EQ(delegate.sync_lost_count(), 1);
  EXPECT_EQ(delegate.last_error().value(), Error(HostError::kCanceled));
}

TEST_F(PeriodicAdvertisingSynchronizerTest, UnsolicitedSyncEstablished) {
  TestDelegate delegate;
  DeviceAddress addr(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid = 12;
  constexpr hci_spec::SyncHandle kSyncHandle = 0x01;
  constexpr uint16_t kSyncPacketInterval = 0x000A;

  DynamicByteBuffer sync_established_event =
      bt::testing::LEPeriodicAdvertisingSyncEstablishedEventPacketV1(
          pw::bluetooth::emboss::StatusCode::SUCCESS,
          kSyncHandle,
          kAdvSid,
          addr,
          pw::bluetooth::emboss::LEPhy::LE_1M,
          kSyncPacketInterval,
          pw::bluetooth::emboss::LEClockAccuracy::PPM_500);

  ExpectTerminateSync(test_device(), kSyncHandle);

  test_device()->SendCommandChannelPacket(sync_established_event);
  RunUntilIdle();

  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());
  EXPECT_EQ(delegate.sync_established_count(), 0);
}

TEST_F(PeriodicAdvertisingSynchronizerTest,
       CreateSyncMultipleRequestsSameAddress) {
  TestDelegate delegate1;
  DeviceAddress addr(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid = 12;

  auto sync1 = synchronizer()->CreateSync(
      addr, kAdvSid, {.filter_duplicates = false}, delegate1);
  EXPECT_TRUE(sync1.is_ok());

  auto add_to_list_packet =
      bt::testing::LEAddDeviceToPeriodicAdvertiserListPacket(addr, kAdvSid);
  auto add_to_list_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_ADD_DEVICE_TO_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), add_to_list_packet, &add_to_list_complete);

  auto create_sync_packet = CreateSyncPacket(
      /*filter_duplicates=*/false,
      /*use_periodic_advertiser_list=*/true);
  auto create_sync_status = bt::testing::CommandStatusPacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(test_device(), create_sync_packet, &create_sync_status);

  RunUntilIdle();

  TestDelegate delegate2;
  auto sync2 = synchronizer()->CreateSync(
      addr, kAdvSid, {.filter_duplicates = false}, delegate2);
  EXPECT_TRUE(sync2.is_error());
  RunUntilIdle();

  EXPECT_EQ(delegate1.sync_established_count(), 0);
  EXPECT_EQ(delegate2.sync_established_count(), 0);
  EXPECT_EQ(delegate2.sync_lost_count(), 0);

  constexpr hci_spec::ConnectionHandle kSyncHandle = 0x01;
  constexpr uint16_t kSyncPacketInterval = 0x000A;
  auto sync_established_event =
      bt::testing::LEPeriodicAdvertisingSyncEstablishedEventPacketV1(
          pw::bluetooth::emboss::StatusCode::SUCCESS,
          kSyncHandle,
          kAdvSid,
          addr,
          pw::bluetooth::emboss::LEPhy::LE_1M,
          kSyncPacketInterval,
          pw::bluetooth::emboss::LEClockAccuracy::PPM_500);

  auto remove_from_list_packet =
      bt::testing::LERemoveDeviceFromPeriodicAdvertiserListPacket(addr,
                                                                  kAdvSid);
  auto remove_from_list_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::
          LE_REMOVE_DEVICE_FROM_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), remove_from_list_packet, &remove_from_list_complete);

  test_device()->SendCommandChannelPacket(sync_established_event);
  RunUntilIdle();

  EXPECT_EQ(delegate1.sync_established_count(), 1);
  EXPECT_EQ(delegate1.sync_lost_count(), 0);
  EXPECT_EQ(delegate2.sync_established_count(), 0);
  EXPECT_EQ(delegate2.sync_lost_count(), 0);

  ExpectTerminateSync(test_device(), kSyncHandle);
}

TEST_F(PeriodicAdvertisingSynchronizerTest, CancelSyncFailure) {
  TestDelegate delegate;
  DeviceAddress addr(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid = 12;
  constexpr hci_spec::ConnectionHandle kSyncHandle = 0x01;

  auto sync = CreateSyncAndExpectSuccess(delegate, addr, kAdvSid, kSyncHandle);
  ASSERT_TRUE(sync.has_value());

  auto terminate_sync_packet =
      bt::testing::LEPeriodicAdvertisingTerminateSyncPacket(kSyncHandle);
  auto terminate_sync_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_TERMINATE_SYNC,
      pw::bluetooth::emboss::StatusCode::COMMAND_DISALLOWED);
  EXPECT_CMD_PACKET_OUT(
      test_device(), terminate_sync_packet, &terminate_sync_complete);

  sync.value().Cancel();
  RunUntilIdle();

  EXPECT_EQ(delegate.sync_lost_count(), 1);
  ASSERT_TRUE(delegate.last_error().has_value());
  EXPECT_EQ(delegate.last_error().value(), Error(HostError::kCanceled));
}

TEST_F(PeriodicAdvertisingSynchronizerTest, CancelWhileAddDevicePending) {
  TestDelegate delegate;
  DeviceAddress addr(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid = 12;

  Result<PeriodicAdvertisingSync> sync = synchronizer()->CreateSync(
      addr, kAdvSid, {.filter_duplicates = false}, delegate);
  ASSERT_TRUE(sync.is_ok());

  DynamicByteBuffer add_to_list_packet =
      bt::testing::LEAddDeviceToPeriodicAdvertiserListPacket(addr, kAdvSid);
  EXPECT_CMD_PACKET_OUT(test_device(), add_to_list_packet);

  RunUntilIdle();

  sync->Cancel();
  RunUntilIdle();

  auto remove_from_list_packet =
      bt::testing::LERemoveDeviceFromPeriodicAdvertiserListPacket(addr,
                                                                  kAdvSid);
  auto remove_from_list_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::
          LE_REMOVE_DEVICE_FROM_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), remove_from_list_packet, &remove_from_list_complete);

  DynamicByteBuffer add_to_list_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_ADD_DEVICE_TO_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  test_device()->SendCommandChannelPacket(add_to_list_complete);
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());

  EXPECT_EQ(delegate.sync_lost_count(), 1);
  EXPECT_EQ(delegate.last_error().value(), Error(HostError::kCanceled));
}

TEST_F(PeriodicAdvertisingSynchronizerTest, AddDeviceCommandDisallowed) {
  TestDelegate delegate1;
  DeviceAddress addr1(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid1 = 12;

  auto add_to_list_packet1 =
      bt::testing::LEAddDeviceToPeriodicAdvertiserListPacket(addr1, kAdvSid1);
  auto add_to_list_complete1 = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_ADD_DEVICE_TO_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::COMMAND_DISALLOWED);
  EXPECT_CMD_PACKET_OUT(
      test_device(), add_to_list_packet1, &add_to_list_complete1);

  Result<PeriodicAdvertisingSync> sync1 = synchronizer()->CreateSync(
      addr1, kAdvSid1, {.filter_duplicates = false}, delegate1);
  EXPECT_TRUE(sync1.is_ok());

  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());
  EXPECT_EQ(delegate1.sync_lost_count(), 1);

  TestDelegate delegate2;
  DeviceAddress addr2(DeviceAddress::Type::kLEPublic, {2});
  constexpr uint8_t kAdvSid2 = 13;
  Result<PeriodicAdvertisingSync> sync2 = synchronizer()->CreateSync(
      addr2, kAdvSid2, {.filter_duplicates = false}, delegate2);
  // PeriodicAdvertisingSynchronizer should be in a bad state.
  EXPECT_TRUE(sync2.is_error());
  RunUntilIdle();
  EXPECT_EQ(delegate2.sync_lost_count(), 0);
}

TEST_F(PeriodicAdvertisingSynchronizerTest, RemoveDeviceCommandFailure) {
  TestDelegate delegate;
  DeviceAddress addr(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid = 12;

  auto sync = synchronizer()->CreateSync(
      addr, kAdvSid, {.filter_duplicates = false}, delegate);
  EXPECT_TRUE(sync.is_ok());

  auto add_to_list_packet =
      bt::testing::LEAddDeviceToPeriodicAdvertiserListPacket(addr, kAdvSid);
  auto add_to_list_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_ADD_DEVICE_TO_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), add_to_list_packet, &add_to_list_complete);

  auto command_status_rsp = bt::testing::CommandStatusPacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), CreateSyncPacket(false, true), &command_status_rsp);
  RunUntilIdle();

  constexpr hci_spec::ConnectionHandle kSyncHandle = 0x01;
  constexpr uint16_t kSyncPacketInterval = 0x000A;
  auto sync_established_event =
      bt::testing::LEPeriodicAdvertisingSyncEstablishedEventPacketV1(
          pw::bluetooth::emboss::StatusCode::SUCCESS,
          kSyncHandle,
          kAdvSid,
          addr,
          pw::bluetooth::emboss::LEPhy::LE_1M,
          kSyncPacketInterval,
          pw::bluetooth::emboss::LEClockAccuracy::PPM_500);

  auto remove_from_list_packet =
      bt::testing::LERemoveDeviceFromPeriodicAdvertiserListPacket(addr,
                                                                  kAdvSid);
  auto remove_from_list_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::
          LE_REMOVE_DEVICE_FROM_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::UNKNOWN_ADVERTISING_IDENTIFIER);
  EXPECT_CMD_PACKET_OUT(
      test_device(), remove_from_list_packet, &remove_from_list_complete);

  test_device()->SendCommandChannelPacket(sync_established_event);
  RunUntilIdle();
  EXPECT_EQ(delegate.sync_established_count(), 1);
  EXPECT_EQ(delegate.sync_lost_count(), 0);

  TestDelegate delegate2;
  DeviceAddress addr2(DeviceAddress::Type::kLEPublic, {2});
  constexpr uint8_t kAdvSid2 = 13;
  Result<PeriodicAdvertisingSync> sync2 = synchronizer()->CreateSync(
      addr2, kAdvSid2, {.filter_duplicates = false}, delegate2);
  // PeriodicAdvertisingSynchronizer should be in a bad state.
  EXPECT_TRUE(sync2.is_error());
  RunUntilIdle();
  EXPECT_EQ(delegate2.sync_lost_count(), 0);

  ExpectTerminateSync(test_device(), kSyncHandle);
}

TEST_F(PeriodicAdvertisingSynchronizerTest, TerminateSyncFailure) {
  TestDelegate delegate;
  DeviceAddress addr(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid = 12;
  constexpr hci_spec::ConnectionHandle kSyncHandle = 0x01;

  std::optional<PeriodicAdvertisingSync> sync =
      CreateSyncAndExpectSuccess(delegate, addr, kAdvSid, kSyncHandle);
  ASSERT_TRUE(sync.has_value());

  auto terminate_sync_packet =
      bt::testing::LEPeriodicAdvertisingTerminateSyncPacket(kSyncHandle);
  auto terminate_sync_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_TERMINATE_SYNC,
      pw::bluetooth::emboss::StatusCode::UNKNOWN_ADVERTISING_IDENTIFIER);
  EXPECT_CMD_PACKET_OUT(
      test_device(), terminate_sync_packet, &terminate_sync_complete);
  sync->Cancel();
  RunUntilIdle();
}

TEST_F(PeriodicAdvertisingSynchronizerTest, SyncLostWithUnknownHandleIgnored) {
  constexpr hci_spec::ConnectionHandle kSyncHandle = 0x0F;
  auto sync_lost_event = bt::testing::LESyncLostEventPacket(kSyncHandle);
  test_device()->SendCommandChannelPacket(sync_lost_event);
  RunUntilIdle();
}

TEST_F(PeriodicAdvertisingSynchronizerTest,
       AdvertisingReportWithUnknownHandleIgnored) {
  constexpr hci_spec::ConnectionHandle kSyncHandle = 0x0F;
  DynamicByteBuffer data({4, 5, 6});
  auto advertising_report_event =
      bt::testing::LEPeriodicAdvertisingReportEventPacketV1(
          kSyncHandle,
          pw::bluetooth::emboss::LEPeriodicAdvertisingDataStatus::COMPLETE,
          data);
  test_device()->SendCommandChannelPacket(advertising_report_event);
  RunUntilIdle();
}

TEST_F(PeriodicAdvertisingSynchronizerTest,
       BigInfoReportWithUnknownHandleIgnored) {
  constexpr hci_spec::ConnectionHandle kSyncHandle = 0x0F;
  auto big_info_report_event =
      bt::testing::LEBigInfoAdvertisingReportEventPacket(
          kSyncHandle,
          /*num_bis=*/1,
          /*nse=*/2,
          /*iso_interval=*/3,
          /*bn=*/4,
          /*pto=*/5,
          /*irc=*/6,
          /*max_pdu=*/7,
          /*sdu_interval=*/8,
          /*max_sdu=*/9,
          pw::bluetooth::emboss::IsoPhyType::LE_2M,
          pw::bluetooth::emboss::BigFraming::FRAMED,
          /*encryption=*/true);
  test_device()->SendCommandChannelPacket(big_info_report_event);
  RunUntilIdle();
}

TEST_F(PeriodicAdvertisingSynchronizerTest, InvalidAddressTypes) {
  TestDelegate delegate;
  constexpr uint8_t kAdvSid = 13;

  DeviceAddress bredr_addr(DeviceAddress::Type::kBREDR, {2});
  auto result = synchronizer()->CreateSync(
      bredr_addr, kAdvSid, {.filter_duplicates = true}, delegate);
  ASSERT_TRUE(result.is_error());
  EXPECT_EQ(result.error_value(), Error(HostError::kInvalidParameters));

  DeviceAddress anon_addr(DeviceAddress::Type::kLEAnonymous, {2});
  result = synchronizer()->CreateSync(
      anon_addr, kAdvSid, {.filter_duplicates = true}, delegate);
  ASSERT_TRUE(result.is_error());
  EXPECT_EQ(result.error_value(), Error(HostError::kInvalidParameters));
}

TEST_F(PeriodicAdvertisingSynchronizerTest, TransferSyncSuccess) {
  TestDelegate delegate;
  DeviceAddress addr(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid = 12;
  constexpr hci_spec::SyncHandle kSyncHandle = 0x01;

  std::optional<PeriodicAdvertisingSync> sync =
      CreateSyncAndExpectSuccess(delegate, addr, kAdvSid, kSyncHandle);
  ASSERT_TRUE(sync.has_value());

  std::optional<Result<>> transfer_result;
  auto callback = [&transfer_result](Result<> result) {
    transfer_result = result;
  };

  auto command = testing::LEPeriodicAdvertisingSyncTransferPacket(
      kConnectionHandle, kServiceData, kSyncHandle);
  auto transfer_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_SYNC_TRANSFER,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(test_device(), command, &transfer_complete);

  synchronizer()->TransferSync(
      sync->id(), kConnectionHandle, kServiceData, std::move(callback));

  RunUntilIdle();
  ASSERT_TRUE(transfer_result.has_value());
  EXPECT_TRUE(transfer_result->is_ok());
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());

  ExpectTerminateSync(test_device(), kSyncHandle);
}

TEST_F(PeriodicAdvertisingSynchronizerTest, TransferSyncInvalidSyncId) {
  TestDelegate delegate;
  DeviceAddress addr(DeviceAddress::Type::kLEPublic, {1});

  std::optional<Result<>> transfer_result;
  auto callback = [&](Result<> result) { transfer_result = result; };

  synchronizer()->TransferSync(
      kInvalidSyncId, kConnectionHandle, kServiceData, std::move(callback));

  RunUntilIdle();
  ASSERT_TRUE(transfer_result.has_value());
  ASSERT_TRUE(transfer_result->is_error());
  EXPECT_TRUE(transfer_result->error_value().is(HostError::kInvalidParameters));
}

TEST_F(PeriodicAdvertisingSynchronizerTest,
       TransferSyncUnsupportedRemoteFeatureError) {
  TestDelegate delegate;
  DeviceAddress addr(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid = 12;
  constexpr hci_spec::SyncHandle kSyncHandle = 0x01;

  std::optional<PeriodicAdvertisingSync> sync =
      CreateSyncAndExpectSuccess(delegate, addr, kAdvSid, kSyncHandle);
  ASSERT_TRUE(sync.has_value());

  std::optional<Result<>> transfer_result;
  auto callback = [&](Result<> result) { transfer_result = result; };

  auto command = testing::LEPeriodicAdvertisingSyncTransferPacket(
      kConnectionHandle, kServiceData, kSyncHandle);
  auto transfer_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_SYNC_TRANSFER,
      pw::bluetooth::emboss::StatusCode::UNSUPPORTED_REMOTE_FEATURE);
  EXPECT_CMD_PACKET_OUT(test_device(), command, &transfer_complete);

  synchronizer()->TransferSync(
      sync->id(), kConnectionHandle, kServiceData, std::move(callback));

  RunUntilIdle();
  ASSERT_TRUE(transfer_result.has_value());
  ASSERT_TRUE(transfer_result->is_error());
  EXPECT_TRUE(transfer_result->error_value().is(
      pw::bluetooth::emboss::StatusCode::UNSUPPORTED_REMOTE_FEATURE));

  ExpectTerminateSync(test_device(), kSyncHandle);
}

// Verify that receiving a truncated LE Periodic Advertising Sync Established
// event is handled safely without crashing, failing pending sync requests.
TEST_F(PeriodicAdvertisingSynchronizerTest, MalformedSyncEstablishedEvent) {
  TestDelegate delegate;
  DeviceAddress addr(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid = 12;

  auto add_to_list_packet =
      bt::testing::LEAddDeviceToPeriodicAdvertiserListPacket(addr, kAdvSid);
  auto add_to_list_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_ADD_DEVICE_TO_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), add_to_list_packet, &add_to_list_complete);

  auto command_status_rsp = bt::testing::CommandStatusPacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        CreateSyncPacket(/*filter_duplicates=*/false,
                                         /*use_periodic_advertiser_list=*/true),
                        &command_status_rsp);

  auto sync_result = synchronizer()->CreateSync(
      addr, kAdvSid, {.filter_duplicates = false}, delegate);
  EXPECT_TRUE(sync_result.is_ok());
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());

  StaticByteBuffer kTruncatedEvent(
      hci_spec::kLEMetaEventCode,
      0x05,  // parameter_total_size
      0x0E,  // Subevent code: LE Periodic Advertising Sync Established
      0x00,  // status: Success
      0x00,
      0x00,
      0x00);

  // Any Sync Established event ends Create Sync, so no Create Sync Cancel is
  // sent. The failed entry is removed from the advertiser list directly.
  auto remove_from_list_packet =
      bt::testing::LERemoveDeviceFromPeriodicAdvertiserListPacket(addr,
                                                                  kAdvSid);
  auto remove_from_list_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::
          LE_REMOVE_DEVICE_FROM_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), remove_from_list_packet, &remove_from_list_complete);

  test_device()->SendCommandChannelPacket(kTruncatedEvent);
  RunUntilIdle();

  EXPECT_EQ(delegate.sync_established_count(), 0);
  EXPECT_EQ(delegate.sync_lost_count(), 1);
  ASSERT_TRUE(delegate.last_error().has_value());
  EXPECT_EQ(delegate.last_error().value(), Error(HostError::kPacketMalformed));
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());

  // Verify that the state machine is not wedged.
  TestDelegate recovery_delegate;
  constexpr hci_spec::SyncHandle kSyncHandle = 0x01;
  std::optional<PeriodicAdvertisingSync> recovered_sync =
      CreateSyncAndExpectSuccess(recovery_delegate, addr, kAdvSid, kSyncHandle);
  ASSERT_TRUE(recovered_sync.has_value());
  ExpectTerminateSync(test_device(), kSyncHandle);
}

// Verify that a malformed Sync Established event while Create Sync is pending
// ends Create Sync, so a delegate that retries from OnSyncLost gets a new
// Create Sync instead of a Create Sync Cancel that the controller would reject.
TEST_F(PeriodicAdvertisingSynchronizerTest,
       MalformedSyncEstablishedEventRetryFromSyncLost) {
  DeviceAddress addr(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid = 12;

  class RetryDelegate : public TestDelegate {
   public:
    RetryDelegate(PeriodicAdvertisingSynchronizer& synchronizer,
                  DeviceAddress addr,
                  uint8_t adv_sid)
        : synchronizer_(synchronizer), addr_(addr), adv_sid_(adv_sid) {}

    void OnSyncLost(SyncId id, Error error) override {
      TestDelegate::OnSyncLost(id, error);
      if (sync_lost_count() == 1) {
        retry_.emplace(synchronizer_.CreateSync(
            addr_, adv_sid_, {.filter_duplicates = false}, *this));
      }
    }

    std::optional<Result<PeriodicAdvertisingSync>>& retry() { return retry_; }

   private:
    PeriodicAdvertisingSynchronizer& synchronizer_;
    DeviceAddress addr_;
    uint8_t adv_sid_;
    std::optional<Result<PeriodicAdvertisingSync>> retry_;
  };
  RetryDelegate delegate(*synchronizer(), addr, kAdvSid);

  auto add_to_list_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_ADD_DEVICE_TO_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(),
      bt::testing::LEAddDeviceToPeriodicAdvertiserListPacket(addr, kAdvSid),
      &add_to_list_complete);
  auto command_status_rsp = bt::testing::CommandStatusPacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        CreateSyncPacket(/*filter_duplicates=*/false,
                                         /*use_periodic_advertiser_list=*/true),
                        &command_status_rsp);
  auto sync_result = synchronizer()->CreateSync(
      addr, kAdvSid, {.filter_duplicates = false}, delegate);
  EXPECT_TRUE(sync_result.is_ok());
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());

  // The entry is still in the advertiser list, so the retry only needs a new
  // Create Sync. No Create Sync Cancel is sent.
  EXPECT_CMD_PACKET_OUT(test_device(),
                        CreateSyncPacket(/*filter_duplicates=*/false,
                                         /*use_periodic_advertiser_list=*/true),
                        &command_status_rsp);
  StaticByteBuffer kShortSyncEstablishedV1(
      hci_spec::kLEMetaEventCode,
      0x01,  // parameter_total_size
      0x0E   // Subevent code: LE Periodic Advertising Sync Established
  );
  test_device()->SendCommandChannelPacket(kShortSyncEstablishedV1);
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());
  EXPECT_EQ(delegate.sync_lost_count(), 1);
  ASSERT_TRUE(delegate.last_error().has_value());
  EXPECT_EQ(delegate.last_error().value(), Error(HostError::kPacketMalformed));
  ASSERT_TRUE(delegate.retry().has_value());
  ASSERT_TRUE(delegate.retry()->is_ok());

  // The retried Create Sync completes normally.
  constexpr hci_spec::SyncHandle kSyncHandle = 0x01;
  auto remove_from_list_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::
          LE_REMOVE_DEVICE_FROM_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(),
      bt::testing::LERemoveDeviceFromPeriodicAdvertiserListPacket(addr,
                                                                  kAdvSid),
      &remove_from_list_complete);
  test_device()->SendCommandChannelPacket(
      bt::testing::LEPeriodicAdvertisingSyncEstablishedEventPacketV1(
          pw::bluetooth::emboss::StatusCode::SUCCESS,
          kSyncHandle,
          kAdvSid,
          addr,
          pw::bluetooth::emboss::LEPhy::LE_1M,
          /*interval=*/0x000A,
          pw::bluetooth::emboss::LEClockAccuracy::PPM_500));
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());
  EXPECT_EQ(delegate.sync_established_count(), 1);

  ExpectTerminateSync(test_device(), kSyncHandle);
}

// Verify that receiving a truncated LE Periodic Advertising Report event is
// dropped safely without asserting or crashing.
TEST_F(PeriodicAdvertisingSynchronizerTest,
       MalformedPeriodicAdvertisingReport) {
  TestDelegate delegate;
  DeviceAddress addr(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid = 12;
  constexpr hci_spec::SyncHandle kSyncHandle = 0x01;

  std::optional<PeriodicAdvertisingSync> sync =
      CreateSyncAndExpectSuccess(delegate, addr, kAdvSid, kSyncHandle);
  ASSERT_TRUE(sync.has_value());

  StaticByteBuffer kShortReportEvent(
      hci_spec::kLEMetaEventCode,
      0x01,  // parameter_total_size
      0x0F   // Subevent code: LE Periodic Advertising Report
  );
  test_device()->SendCommandChannelPacket(kShortReportEvent);

  StaticByteBuffer kTruncatedReportEvent(
      hci_spec::kLEMetaEventCode,
      0x04,  // parameter_total_size
      0x0F,  // Subevent code: LE Periodic Advertising Report
      0x00,
      0x00,
      0x00);
  test_device()->SendCommandChannelPacket(kTruncatedReportEvent);

  // Header is complete (10 bytes), but data_length (5) exceeds remaining bytes
  // (1).
  StaticByteBuffer kTruncatedReportDataEvent(
      hci_spec::kLEMetaEventCode,
      0x09,  // parameter_total_size (8 bytes subevent header + 1 byte data)
      0x0F,  // Subevent code: LE Periodic Advertising Report
      0x01,
      0x00,  // sync_handle = 0x0001
      0x00,  // tx_power
      0x00,  // rssi
      0xFF,  // cte_type = no CTE
      0x00,  // data_status = COMPLETE
      0x05,  // data_length = 5 (only 1 byte follows)
      0xAA);
  test_device()->SendCommandChannelPacket(kTruncatedReportDataEvent);
  RunUntilIdle();

  EXPECT_EQ(delegate.report_count(), 0);
  ExpectTerminateSync(test_device(), kSyncHandle);
}

// Verify that a truncated LE Periodic Advertising Sync Established v2 event
// while Create Sync is pending fails the listed request without wedging.
TEST_F(PeriodicAdvertisingSynchronizerTest, MalformedSyncEstablishedEventV2) {
  TestDelegate delegate;
  DeviceAddress addr(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid = 12;

  auto add_to_list_packet =
      bt::testing::LEAddDeviceToPeriodicAdvertiserListPacket(addr, kAdvSid);
  auto add_to_list_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_ADD_DEVICE_TO_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), add_to_list_packet, &add_to_list_complete);

  auto command_status_rsp = bt::testing::CommandStatusPacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        CreateSyncPacket(/*filter_duplicates=*/false,
                                         /*use_periodic_advertiser_list=*/true),
                        &command_status_rsp);

  auto sync_result = synchronizer()->CreateSync(
      addr, kAdvSid, {.filter_duplicates = false}, delegate);
  EXPECT_TRUE(sync_result.is_ok());
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());

  StaticByteBuffer kShortSyncEstablishedV2(
      hci_spec::kLEMetaEventCode,
      0x01,  // parameter_total_size
      0x24   // Subevent code: LE Periodic Advertising Sync Established V2
  );

  // Any Sync Established event ends Create Sync, so no Create Sync Cancel is
  // sent. The failed entry is removed from the advertiser list directly.
  auto remove_from_list_packet =
      bt::testing::LERemoveDeviceFromPeriodicAdvertiserListPacket(addr,
                                                                  kAdvSid);
  auto remove_from_list_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::
          LE_REMOVE_DEVICE_FROM_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(), remove_from_list_packet, &remove_from_list_complete);

  test_device()->SendCommandChannelPacket(kShortSyncEstablishedV2);
  RunUntilIdle();

  EXPECT_EQ(delegate.sync_established_count(), 0);
  EXPECT_EQ(delegate.sync_lost_count(), 1);
  ASSERT_TRUE(delegate.last_error().has_value());
  EXPECT_EQ(delegate.last_error().value(), Error(HostError::kPacketMalformed));
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());

  // Verify that a subsequent CreateSync request succeeds and the state machine
  // is not wedged.
  TestDelegate recovery_delegate;
  constexpr hci_spec::SyncHandle kSyncHandle = 0x01;
  std::optional<PeriodicAdvertisingSync> recovered_sync =
      CreateSyncAndExpectSuccess(
          recovery_delegate, addr, kAdvSid, kSyncHandle, /*v2=*/true);
  ASSERT_TRUE(recovered_sync.has_value());
  ExpectTerminateSync(test_device(), kSyncHandle);
}

// Verify that a malformed Sync Established event while an Add Device command is
// in flight (no Create Sync pending) is ignored: requests already in the
// advertiser list are not failed, and the Add Device flow continues.
TEST_F(PeriodicAdvertisingSynchronizerTest,
       MalformedSyncEstablishedEventWhileAddDevicePendingIsIgnored) {
  TestDelegate delegate1;
  TestDelegate delegate2;
  DeviceAddress addr1(DeviceAddress::Type::kLEPublic, {1});
  DeviceAddress addr2(DeviceAddress::Type::kLEPublic, {2});
  constexpr uint8_t kAdvSid = 12;

  auto add_to_list_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_ADD_DEVICE_TO_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(),
      bt::testing::LEAddDeviceToPeriodicAdvertiserListPacket(addr1, kAdvSid),
      &add_to_list_complete);
  auto command_status_rsp = bt::testing::CommandStatusPacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        CreateSyncPacket(/*filter_duplicates=*/false,
                                         /*use_periodic_advertiser_list=*/true),
                        &command_status_rsp);
  auto sync1 = synchronizer()->CreateSync(
      addr1, kAdvSid, {.filter_duplicates = false}, delegate1);
  EXPECT_TRUE(sync1.is_ok());
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());

  // A second request cancels Create Sync and adds addr2. Leave the Add Device
  // command in flight.
  auto cancel_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC_CANCEL,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  auto sync_established_cancel_event =
      bt::testing::LEPeriodicAdvertisingSyncEstablishedEventPacketV1(
          pw::bluetooth::emboss::StatusCode::OPERATION_CANCELLED_BY_HOST,
          /*handle=*/0,
          /*sid=*/0,
          DeviceAddress(DeviceAddress::Type::kLEPublic, {0}),
          pw::bluetooth::emboss::LEPhy::LE_1M,
          /*interval=*/0x0006,
          pw::bluetooth::emboss::LEClockAccuracy::PPM_500);
  EXPECT_CMD_PACKET_OUT(
      test_device(),
      bt::testing::LEPeriodicAdvertisingCreateSyncCancelPacket(),
      &cancel_complete,
      &sync_established_cancel_event);
  EXPECT_CMD_PACKET_OUT(
      test_device(),
      bt::testing::LEAddDeviceToPeriodicAdvertiserListPacket(addr2, kAdvSid));
  auto sync2 = synchronizer()->CreateSync(
      addr2, kAdvSid, {.filter_duplicates = false}, delegate2);
  EXPECT_TRUE(sync2.is_ok());
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());

  StaticByteBuffer kShortSyncEstablishedV1(
      hci_spec::kLEMetaEventCode,
      0x01,  // parameter_total_size
      0x0E   // Subevent code: LE Periodic Advertising Sync Established
  );
  test_device()->SendCommandChannelPacket(kShortSyncEstablishedV1);
  RunUntilIdle();
  EXPECT_EQ(delegate1.sync_lost_count(), 0);
  EXPECT_EQ(delegate2.sync_lost_count(), 0);

  // Completing Add Device starts Create Sync for both entries.
  EXPECT_CMD_PACKET_OUT(test_device(),
                        CreateSyncPacket(/*filter_duplicates=*/false,
                                         /*use_periodic_advertiser_list=*/true),
                        &command_status_rsp);
  test_device()->SendCommandChannelPacket(add_to_list_complete);
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());
  EXPECT_EQ(delegate1.sync_lost_count(), 0);
  EXPECT_EQ(delegate2.sync_lost_count(), 0);

  // Destroying the sync handles cancels the pending Create Sync.
  EXPECT_CMD_PACKET_OUT(
      test_device(),
      bt::testing::LEPeriodicAdvertisingCreateSyncCancelPacket(),
      &cancel_complete);
}

// Verify that a malformed Sync Established event while Create Sync Cancel is
// pending ends Create Sync, so the canceled entry is removed and a new request
// makes progress.
TEST_F(PeriodicAdvertisingSynchronizerTest,
       MalformedSyncEstablishedEventWhileCreateSyncCancelPending) {
  TestDelegate delegate;
  DeviceAddress addr(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid = 12;

  auto add_to_list_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_ADD_DEVICE_TO_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(),
      bt::testing::LEAddDeviceToPeriodicAdvertiserListPacket(addr, kAdvSid),
      &add_to_list_complete);
  auto command_status_rsp = bt::testing::CommandStatusPacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        CreateSyncPacket(/*filter_duplicates=*/false,
                                         /*use_periodic_advertiser_list=*/true),
                        &command_status_rsp);
  auto sync = synchronizer()->CreateSync(
      addr, kAdvSid, {.filter_duplicates = false}, delegate);
  ASSERT_TRUE(sync.is_ok());
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());

  // Cancel the request. The controller accepts Create Sync Cancel, but the
  // Sync Established event that follows is malformed.
  auto cancel_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC_CANCEL,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(),
      bt::testing::LEPeriodicAdvertisingCreateSyncCancelPacket(),
      &cancel_complete);
  sync->Cancel();
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());
  EXPECT_EQ(delegate.sync_lost_count(), 1);

  auto remove_from_list_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::
          LE_REMOVE_DEVICE_FROM_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(),
      bt::testing::LERemoveDeviceFromPeriodicAdvertiserListPacket(addr,
                                                                  kAdvSid),
      &remove_from_list_complete);
  StaticByteBuffer kShortSyncEstablishedV1(
      hci_spec::kLEMetaEventCode,
      0x01,  // parameter_total_size
      0x0E   // Subevent code: LE Periodic Advertising Sync Established
  );
  test_device()->SendCommandChannelPacket(kShortSyncEstablishedV1);
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());
  EXPECT_EQ(delegate.sync_lost_count(), 1);

  // Verify that the state machine is not wedged.
  TestDelegate recovery_delegate;
  DeviceAddress addr2(DeviceAddress::Type::kLEPublic, {2});
  constexpr hci_spec::SyncHandle kSyncHandle = 0x01;
  std::optional<PeriodicAdvertisingSync> recovered_sync =
      CreateSyncAndExpectSuccess(
          recovery_delegate, addr2, kAdvSid, kSyncHandle);
  ASSERT_TRUE(recovered_sync.has_value());
  ExpectTerminateSync(test_device(), kSyncHandle);
}

// Verify that receiving a truncated LE Periodic Advertising Report v2 event is
// dropped safely without asserting or crashing.
TEST_F(PeriodicAdvertisingSynchronizerTest,
       MalformedPeriodicAdvertisingReportV2) {
  TestDelegate delegate;
  DeviceAddress addr(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid = 12;
  constexpr hci_spec::SyncHandle kSyncHandle = 0x01;

  std::optional<PeriodicAdvertisingSync> sync = CreateSyncAndExpectSuccess(
      delegate, addr, kAdvSid, kSyncHandle, /*v2=*/true);
  ASSERT_TRUE(sync.has_value());

  StaticByteBuffer kShortReportV2Event(
      hci_spec::kLEMetaEventCode,
      0x01,  // parameter_total_size
      0x25   // Subevent code: LE Periodic Advertising Report V2
  );
  test_device()->SendCommandChannelPacket(kShortReportV2Event);

  // Header is complete (13 bytes), but data_length (4) exceeds remaining bytes
  // (1).
  StaticByteBuffer kTruncatedReportV2DataEvent(
      hci_spec::kLEMetaEventCode,
      0x0C,  // parameter_total_size (11 bytes subevent header + 1 byte data)
      0x25,  // Subevent code: LE Periodic Advertising Report V2
      0x01,
      0x00,  // sync_handle = 0x0001
      0x00,  // tx_power
      0x00,  // rssi
      0xFF,  // cte_type = no CTE
      0x01,
      0x00,  // periodic_event_counter
      0x00,  // subevent
      0x00,  // data_status = COMPLETE
      0x04,  // data_length = 4 (only 1 byte follows)
      0xBB);
  test_device()->SendCommandChannelPacket(kTruncatedReportV2DataEvent);
  RunUntilIdle();

  EXPECT_EQ(delegate.report_count(), 0);
  ExpectTerminateSync(test_device(), kSyncHandle);
}

// Verify that receiving a short/malformed LE Periodic Advertising Sync Lost
// event is dropped safely without asserting or dropping an active sync.
TEST_F(PeriodicAdvertisingSynchronizerTest, MalformedSyncLostEvent) {
  TestDelegate delegate;
  DeviceAddress addr(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid = 12;
  constexpr hci_spec::SyncHandle kSyncHandle = 0x01;

  std::optional<PeriodicAdvertisingSync> sync =
      CreateSyncAndExpectSuccess(delegate, addr, kAdvSid, kSyncHandle);
  ASSERT_TRUE(sync.has_value());

  // 3-byte minimal LE Meta Event (missing 2-byte sync_handle).
  StaticByteBuffer kShortSyncLostEvent(
      hci_spec::kLEMetaEventCode,
      0x01,  // parameter_total_size
      0x10   // Subevent code: LE Periodic Advertising Sync Lost
  );
  test_device()->SendCommandChannelPacket(kShortSyncLostEvent);

  // 4-byte truncated Sync Lost event (only 1 byte of sync_handle).
  StaticByteBuffer kTruncatedSyncLostEvent(
      hci_spec::kLEMetaEventCode,
      0x02,  // parameter_total_size
      0x10,  // Subevent code: LE Periodic Advertising Sync Lost
      0x01);
  test_device()->SendCommandChannelPacket(kTruncatedSyncLostEvent);
  RunUntilIdle();

  EXPECT_EQ(delegate.sync_lost_count(), 0);
  ExpectTerminateSync(test_device(), kSyncHandle);
}

// Verify that receiving a short/malformed LE BIGInfo Advertising Report event
// is dropped safely without asserting or crashing.
TEST_F(PeriodicAdvertisingSynchronizerTest, MalformedBigInfoReport) {
  TestDelegate delegate;
  DeviceAddress addr(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSid = 12;
  constexpr hci_spec::SyncHandle kSyncHandle = 0x01;

  std::optional<PeriodicAdvertisingSync> sync =
      CreateSyncAndExpectSuccess(delegate, addr, kAdvSid, kSyncHandle);
  ASSERT_TRUE(sync.has_value());

  // 3-byte minimal LE Meta Event (requires 22 bytes).
  StaticByteBuffer kShortBigInfoEvent(
      hci_spec::kLEMetaEventCode,
      0x01,  // parameter_total_size
      0x22   // Subevent code: LE BIGInfo Advertising Report
  );
  test_device()->SendCommandChannelPacket(kShortBigInfoEvent);

  // Truncated BIGInfo event (only sync_handle present, missing remaining
  // fields).
  StaticByteBuffer kTruncatedBigInfoEvent(
      hci_spec::kLEMetaEventCode,
      0x03,  // parameter_total_size
      0x22,  // Subevent code: LE BIGInfo Advertising Report
      0x01,
      0x00);
  test_device()->SendCommandChannelPacket(kTruncatedBigInfoEvent);
  RunUntilIdle();

  EXPECT_EQ(delegate.big_info_count(), 0);
  ExpectTerminateSync(test_device(), kSyncHandle);
}

TEST_F(PeriodicAdvertisingSynchronizerTest,
       DuplicateSyncHandleTerminatesAndNotifiesBothDelegates) {
  TestDelegate delegate_a;
  DeviceAddress addr_a(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSidA = 1;
  constexpr hci_spec::SyncHandle kReusedHandle = 0x0042;

  std::optional<PeriodicAdvertisingSync> sync_a =
      CreateSyncAndExpectSuccess(delegate_a, addr_a, kAdvSidA, kReusedHandle);
  ASSERT_TRUE(sync_a.has_value());
  EXPECT_EQ(1, delegate_a.sync_established_count());
  EXPECT_EQ(0, delegate_a.sync_lost_count());

  // Start a second sync for a different advertiser.
  TestDelegate delegate_b;
  DeviceAddress addr_b(DeviceAddress::Type::kLEPublic, {2});
  constexpr uint8_t kAdvSidB = 2;

  auto add_b_packet =
      bt::testing::LEAddDeviceToPeriodicAdvertiserListPacket(addr_b, kAdvSidB);
  auto add_b_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_ADD_DEVICE_TO_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(test_device(), add_b_packet, &add_b_complete);

  auto create_sync_status = bt::testing::CommandStatusPacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        CreateSyncPacket(/*filter_duplicates=*/true,
                                         /*use_periodic_advertiser_list=*/true),
                        &create_sync_status);

  auto sync_b_result = synchronizer()->CreateSync(
      addr_b, kAdvSidB, {.filter_duplicates = true}, delegate_b);
  ASSERT_TRUE(sync_b_result.is_ok());
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());

  // Controller erroneously reuses kReusedHandle for sync B. The host must
  // remove B from the advertiser list, send Terminate_Sync once for
  // kReusedHandle, and notify both delegates via OnSyncLost.
  auto remove_b_packet =
      bt::testing::LERemoveDeviceFromPeriodicAdvertiserListPacket(addr_b,
                                                                  kAdvSidB);
  auto remove_b_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::
          LE_REMOVE_DEVICE_FROM_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(test_device(), remove_b_packet, &remove_b_complete);
  ExpectTerminateSync(test_device(), kReusedHandle);

  DynamicByteBuffer reused_event =
      bt::testing::LEPeriodicAdvertisingSyncEstablishedEventPacketV1(
          pw::bluetooth::emboss::StatusCode::SUCCESS,
          kReusedHandle,
          kAdvSidB,
          addr_b,
          pw::bluetooth::emboss::LEPhy::LE_1M,
          /*interval=*/0x000A,
          pw::bluetooth::emboss::LEClockAccuracy::PPM_500);
  test_device()->SendCommandChannelPacket(reused_event);
  RunUntilIdle();

  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());
  EXPECT_EQ(1, delegate_a.sync_lost_count());
  ASSERT_TRUE(delegate_a.last_error().has_value());
  EXPECT_EQ(Error(HostError::kFailed), *delegate_a.last_error());
  EXPECT_EQ(sync_a->id(), delegate_a.last_sync_id());

  EXPECT_EQ(0, delegate_b.sync_established_count());
  EXPECT_EQ(1, delegate_b.sync_lost_count());
  ASSERT_TRUE(delegate_b.last_error().has_value());
  EXPECT_EQ(Error(HostError::kFailed), *delegate_b.last_error());
  EXPECT_EQ(sync_b_result->id(), delegate_b.last_sync_id());

  // Dropping both RAII handles afterward must be a clean no-op (no duplicate
  // Terminate_Sync commands sent).
  sync_a.reset();
  sync_b_result->Cancel();
  RunUntilIdle();
}

TEST_F(
    PeriodicAdvertisingSynchronizerTest,
    UnsolicitedSyncEstablishedWhileIdleReusingHandleTerminatesAndNotifiesDelegate) {
  TestDelegate delegate_a;
  DeviceAddress addr_a(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSidA = 1;
  constexpr hci_spec::SyncHandle kReusedHandle = 0x0042;

  std::optional<PeriodicAdvertisingSync> sync_a =
      CreateSyncAndExpectSuccess(delegate_a, addr_a, kAdvSidA, kReusedHandle);
  ASSERT_TRUE(sync_a.has_value());

  ExpectTerminateSync(test_device(), kReusedHandle);

  DeviceAddress unsolicited_addr(DeviceAddress::Type::kLEPublic, {2});
  constexpr uint8_t kUnsolicitedSid = 2;
  DynamicByteBuffer unsolicited_event =
      bt::testing::LEPeriodicAdvertisingSyncEstablishedEventPacketV1(
          pw::bluetooth::emboss::StatusCode::SUCCESS,
          kReusedHandle,
          kUnsolicitedSid,
          unsolicited_addr,
          pw::bluetooth::emboss::LEPhy::LE_1M,
          /*interval=*/0x000A,
          pw::bluetooth::emboss::LEClockAccuracy::PPM_500);
  test_device()->SendCommandChannelPacket(unsolicited_event);
  RunUntilIdle();

  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());
  EXPECT_EQ(1, delegate_a.sync_lost_count());
  ASSERT_TRUE(delegate_a.last_error().has_value());
  EXPECT_EQ(Error(HostError::kFailed), *delegate_a.last_error());
  EXPECT_EQ(sync_a->id(), delegate_a.last_sync_id());

  sync_a.reset();
  RunUntilIdle();
}

TEST_F(
    PeriodicAdvertisingSynchronizerTest,
    UnmatchedSyncEstablishedWhilePendingReusingHandleTerminatesAndReissuesCreateSync) {
  TestDelegate delegate_a;
  DeviceAddress addr_a(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSidA = 1;
  constexpr hci_spec::SyncHandle kReusedHandle = 0x0042;

  std::optional<PeriodicAdvertisingSync> sync_a =
      CreateSyncAndExpectSuccess(delegate_a, addr_a, kAdvSidA, kReusedHandle);
  ASSERT_TRUE(sync_a.has_value());

  TestDelegate delegate_b;
  DeviceAddress addr_b(DeviceAddress::Type::kLEPublic, {2});
  constexpr uint8_t kAdvSidB = 2;

  auto add_b_packet =
      bt::testing::LEAddDeviceToPeriodicAdvertiserListPacket(addr_b, kAdvSidB);
  auto add_b_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_ADD_DEVICE_TO_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(test_device(), add_b_packet, &add_b_complete);

  auto create_sync_status = bt::testing::CommandStatusPacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        CreateSyncPacket(/*filter_duplicates=*/true,
                                         /*use_periodic_advertiser_list=*/true),
                        &create_sync_status);

  auto sync_b_result = synchronizer()->CreateSync(
      addr_b, kAdvSidB, {.filter_duplicates = true}, delegate_b);
  ASSERT_TRUE(sync_b_result.is_ok());
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());

  // Unmatched (addr_c, kAdvSidC) arrives reusing kReusedHandle while sync_b is
  // pending: Terminate_Sync is sent, Create_Sync is re-issued for sync_b, and
  // only delegate_a is notified of sync loss.
  ExpectTerminateSync(test_device(), kReusedHandle);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        CreateSyncPacket(/*filter_duplicates=*/true,
                                         /*use_periodic_advertiser_list=*/true),
                        &create_sync_status);

  DeviceAddress addr_c(DeviceAddress::Type::kLEPublic, {3});
  constexpr uint8_t kAdvSidC = 3;
  DynamicByteBuffer unmatched_event =
      bt::testing::LEPeriodicAdvertisingSyncEstablishedEventPacketV1(
          pw::bluetooth::emboss::StatusCode::SUCCESS,
          kReusedHandle,
          kAdvSidC,
          addr_c,
          pw::bluetooth::emboss::LEPhy::LE_1M,
          /*interval=*/0x000A,
          pw::bluetooth::emboss::LEClockAccuracy::PPM_500);
  test_device()->SendCommandChannelPacket(unmatched_event);
  RunUntilIdle();

  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());
  EXPECT_EQ(1, delegate_a.sync_lost_count());
  ASSERT_TRUE(delegate_a.last_error().has_value());
  EXPECT_EQ(Error(HostError::kFailed), *delegate_a.last_error());
  EXPECT_EQ(sync_a->id(), delegate_a.last_sync_id());

  EXPECT_EQ(0, delegate_b.sync_established_count());
  EXPECT_EQ(0, delegate_b.sync_lost_count());

  DestroySynchronizer();
  sync_a.reset();
  sync_b_result->Cancel();
  RunUntilIdle();
}

TEST_F(
    PeriodicAdvertisingSynchronizerTest,
    DuplicateSyncHandleDestroyingSynchronizerInFirstOnSyncLostSkipsSecondDelegate) {
  TestDelegate delegate_a;
  DeviceAddress addr_a(DeviceAddress::Type::kLEPublic, {1});
  constexpr uint8_t kAdvSidA = 1;
  constexpr hci_spec::SyncHandle kReusedHandle = 0x0042;

  std::optional<PeriodicAdvertisingSync> sync_a =
      CreateSyncAndExpectSuccess(delegate_a, addr_a, kAdvSidA, kReusedHandle);
  ASSERT_TRUE(sync_a.has_value());

  delegate_a.set_on_sync_lost_callback([this]() { DestroySynchronizer(); });

  TestDelegate delegate_b;
  DeviceAddress addr_b(DeviceAddress::Type::kLEPublic, {2});
  constexpr uint8_t kAdvSidB = 2;

  auto add_b_packet =
      bt::testing::LEAddDeviceToPeriodicAdvertiserListPacket(addr_b, kAdvSidB);
  auto add_b_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::LE_ADD_DEVICE_TO_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(test_device(), add_b_packet, &add_b_complete);

  auto create_sync_status = bt::testing::CommandStatusPacket(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        CreateSyncPacket(/*filter_duplicates=*/true,
                                         /*use_periodic_advertiser_list=*/true),
                        &create_sync_status);

  auto sync_b_result = synchronizer()->CreateSync(
      addr_b, kAdvSidB, {.filter_duplicates = true}, delegate_b);
  ASSERT_TRUE(sync_b_result.is_ok());
  RunUntilIdle();
  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());

  auto remove_b_packet =
      bt::testing::LERemoveDeviceFromPeriodicAdvertiserListPacket(addr_b,
                                                                  kAdvSidB);
  auto remove_b_complete = bt::testing::CommandCompletePacket(
      pw::bluetooth::emboss::OpCode::
          LE_REMOVE_DEVICE_FROM_PERIODIC_ADVERTISER_LIST,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(test_device(), remove_b_packet, &remove_b_complete);
  ExpectTerminateSync(test_device(), kReusedHandle);

  DynamicByteBuffer reused_event =
      bt::testing::LEPeriodicAdvertisingSyncEstablishedEventPacketV1(
          pw::bluetooth::emboss::StatusCode::SUCCESS,
          kReusedHandle,
          kAdvSidB,
          addr_b,
          pw::bluetooth::emboss::LEPhy::LE_1M,
          /*interval=*/0x000A,
          pw::bluetooth::emboss::LEClockAccuracy::PPM_500);
  test_device()->SendCommandChannelPacket(reused_event);
  RunUntilIdle();

  EXPECT_TRUE(test_device()->AllExpectedCommandPacketsSent());
  EXPECT_EQ(1, delegate_a.sync_lost_count());
  EXPECT_EQ(0, delegate_b.sync_established_count());
  EXPECT_EQ(0, delegate_b.sync_lost_count());
}

}  // namespace
}  // namespace bt::hci
