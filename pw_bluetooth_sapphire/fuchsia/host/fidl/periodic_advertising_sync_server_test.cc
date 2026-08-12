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

#include "pw_bluetooth_sapphire/fuchsia/host/fidl/periodic_advertising_sync_server.h"

#include "pw_bluetooth_sapphire/fuchsia/host/fidl/fake_adapter_test_fixture.h"

namespace fble = ::fuchsia_bluetooth_le;
using Delegate = bt::gap::Adapter::LowEnergy::PeriodicAdvertisingSyncDelegate;

using WatchAdvertisingReport =
    ::fuchsia_bluetooth_le::PeriodicAdvertisingSync::WatchAdvertisingReport;

namespace bthost {

namespace {

constexpr bt::PeerId kPeerId(2);

constexpr bt::hci::SyncId kSyncId(1);

constexpr uint8_t kAdvertisingSid = 3;

constexpr bt::gap::PeriodicAdvertisingSyncManager::SyncParameters
    kSyncParameters{
        .peer_id = kPeerId,
        .advertising_sid = kAdvertisingSid,
        .interval = 2,
        .phy = pw::bluetooth::emboss::LEPhy::LE_2M,
        .subevents_count = 3,
};

bt::hci_spec::BroadcastIsochronousGroupInfo kBigInfo{
    .num_bis = 0x01,
    .nse = 0x02,
    .iso_interval = 0x03,
    .bn = 0x04,
    .pto = 0x05,
    .irc = 0x06,
    .max_pdu = 0x07,
    .sdu_interval = 0x08,
    .max_sdu = 0x09,
    .phy = pw::bluetooth::emboss::IsoPhyType::LE_1M,
    .framing = pw::bluetooth::emboss::BigFraming::FRAMED,
    .encryption = true};

}  // namespace

class SyncEventHandler final
    : public fidl::AsyncEventHandler<fble::PeriodicAdvertisingSync> {
 public:
  const std::vector<fble::PeriodicAdvertisingSyncError>& errors() const {
    return errors_;
  }

  const std::vector<fble::PeriodicAdvertisingSyncOnEstablishedRequest>&
  established_events() const {
    return established_events_;
  }

  std::optional<::fidl::UnbindInfo> fidl_error() const { return fidl_error_; }

 private:
  void OnEstablished(
      ::fidl::Event<fble::PeriodicAdvertisingSync::OnEstablished>& event)
      override {
    established_events_.emplace_back(event);
  }

  void OnError(
      ::fidl::Event<fble::PeriodicAdvertisingSync::OnError>& event) override {
    errors_.push_back(event.error());
  }

  void on_fidl_error(::fidl::UnbindInfo error) override { fidl_error_ = error; }

  void handle_unknown_event(
      fidl::UnknownEventMetadata<fble::PeriodicAdvertisingSync> metadata)
      override {
    ADD_FAILURE();
  }

  std::optional<::fidl::UnbindInfo> fidl_error_;
  std::vector<fble::PeriodicAdvertisingSyncError> errors_;
  std::vector<fble::PeriodicAdvertisingSyncOnEstablishedRequest>
      established_events_;
};

class PeriodicAdvertisingSyncServerTest
    : public bt::fidl::testing::FakeAdapterTestFixture {};

class PeriodicAdvertisingSyncServerSyncEstablishedTest
    : public PeriodicAdvertisingSyncServerTest {
 public:
  void SetUp() override {
    PeriodicAdvertisingSyncServerTest::SetUp();

    auto result = ::fidl::CreateEndpoints<
        ::fuchsia_bluetooth_le::PeriodicAdvertisingSync>();
    ASSERT_TRUE(result.is_ok());
    auto [client_end, server_end] = std::move(result.value());

    client_.Bind(std::move(client_end), dispatcher(), &event_handler_);

    bt::gap::Adapter::LowEnergy::SyncOptions options;
    options.filter_duplicates = false;
    auto closed_cb = [this]() { ++closed_count_; };
    server_ = PeriodicAdvertisingSyncServer::Create(dispatcher(),
                                                    std::move(server_end),
                                                    adapter()->AsWeakPtr(),
                                                    kPeerId,
                                                    kAdvertisingSid,
                                                    options,
                                                    std::move(closed_cb));
    EXPECT_TRUE(server_);
    RunLoopUntilIdle();
    ASSERT_TRUE(event_handler_.errors().empty());
    ASSERT_TRUE(event_handler_.established_events().empty());

    ASSERT_EQ(adapter()->fake_le()->periodic_advertisement_syncs().size(), 1u);
    delegate_ = &adapter()
                     ->fake_le()
                     ->periodic_advertisement_syncs()
                     .begin()
                     ->second.delegate;
    delegate_->OnSyncEstablished(kSyncId, kSyncParameters);
    RunLoopUntilIdle();
    ASSERT_EQ(event_handler_.established_events().size(), 1u);
  }

  void TearDown() override {
    {
      auto _ = client_.UnbindMaybeGetEndpoint();
    }
    server_.reset();
    PeriodicAdvertisingSyncServerTest::TearDown();
  }

  int closed_count() const { return closed_count_; }

  const SyncEventHandler& event_handler() const { return event_handler_; }

  fidl::Client<::fuchsia_bluetooth_le::PeriodicAdvertisingSync>& client() {
    return client_;
  }

  Delegate* delegate() const {
    PW_CHECK(delegate_);
    return delegate_;
  }

 private:
  int closed_count_ = 0;
  SyncEventHandler event_handler_;
  fidl::Client<::fuchsia_bluetooth_le::PeriodicAdvertisingSync> client_;
  Delegate* delegate_ = nullptr;
  std::unique_ptr<PeriodicAdvertisingSyncServer> server_;
};

TEST_F(PeriodicAdvertisingSyncServerTest, NotSupportedLocalError) {
  adapter()->fake_le()->set_sync_to_periodic_advertisement_error(
      bt::hci::Error(bt::HostError::kNotSupported));

  auto result = ::fidl::CreateEndpoints<
      ::fuchsia_bluetooth_le::PeriodicAdvertisingSync>();
  ASSERT_TRUE(result.is_ok());
  auto [client_end, server_end] = std::move(result.value());

  SyncEventHandler event_handler;
  fidl::Client<::fuchsia_bluetooth_le::PeriodicAdvertisingSync> client(
      std::move(client_end), dispatcher(), &event_handler);

  bt::gap::Adapter::LowEnergy::SyncOptions options;
  options.filter_duplicates = false;

  int closed_count = 0;
  auto closed_cb = [&closed_count]() { ++closed_count; };

  std::unique_ptr<PeriodicAdvertisingSyncServer> server =
      PeriodicAdvertisingSyncServer::Create(dispatcher(),
                                            std::move(server_end),
                                            adapter()->AsWeakPtr(),
                                            kPeerId,
                                            kAdvertisingSid,
                                            options,
                                            std::move(closed_cb));
  EXPECT_FALSE(server);

  RunLoopUntilIdle();
  EXPECT_EQ(closed_count, 0);
  ASSERT_EQ(event_handler.errors().size(), 1u);
  EXPECT_EQ(event_handler.errors()[0],
            fble::PeriodicAdvertisingSyncError::kNotSupportedLocal);
  ASSERT_TRUE(event_handler.fidl_error().has_value());
  EXPECT_EQ(event_handler.fidl_error().value().status(), ZX_ERR_NOT_SUPPORTED);
}

TEST_F(PeriodicAdvertisingSyncServerTest, EstablishSyncAndUnbindClient) {
  auto result = ::fidl::CreateEndpoints<
      ::fuchsia_bluetooth_le::PeriodicAdvertisingSync>();
  ASSERT_TRUE(result.is_ok());
  auto [client_end, server_end] = std::move(result.value());

  SyncEventHandler event_handler;
  fidl::Client<::fuchsia_bluetooth_le::PeriodicAdvertisingSync> client(
      std::move(client_end), dispatcher(), &event_handler);

  bt::gap::Adapter::LowEnergy::SyncOptions options;
  options.filter_duplicates = false;

  int closed_count = 0;
  auto closed_cb = [&closed_count]() { ++closed_count; };

  std::unique_ptr<PeriodicAdvertisingSyncServer> server =
      PeriodicAdvertisingSyncServer::Create(dispatcher(),
                                            std::move(server_end),
                                            adapter()->AsWeakPtr(),
                                            kPeerId,
                                            kAdvertisingSid,
                                            options,
                                            std::move(closed_cb));
  EXPECT_TRUE(server);
  RunLoopUntilIdle();
  ASSERT_TRUE(event_handler.errors().empty());
  ASSERT_TRUE(event_handler.established_events().empty());

  bt::hci::SyncId sync_id(1);
  ASSERT_EQ(adapter()->fake_le()->periodic_advertisement_syncs().size(), 1u);
  adapter()
      ->fake_le()
      ->periodic_advertisement_syncs()
      .begin()
      ->second.delegate.OnSyncEstablished(sync_id, kSyncParameters);

  RunLoopUntilIdle();
  ASSERT_EQ(event_handler.established_events().size(), 1u);
  auto event = event_handler.established_events()[0];
  EXPECT_EQ(event.peer_id(), ::fuchsia_bluetooth::PeerId(kPeerId.value()));
  EXPECT_EQ(event.advertising_sid(), kAdvertisingSid);
  EXPECT_EQ(event.periodic_advertising_interval(), kSyncParameters.interval);
  EXPECT_EQ(event.phy(), ::fuchsia_bluetooth_le::PhysicalLayer::kLe2M);
  EXPECT_EQ(event.subevents_count(), kSyncParameters.subevents_count);

  {
    auto _ = client.UnbindMaybeGetEndpoint();
  }
  RunLoopUntilIdle();
  EXPECT_EQ(closed_count, 1);
  EXPECT_EQ(adapter()->fake_le()->periodic_advertisement_syncs().size(), 0u);
}

TEST_F(PeriodicAdvertisingSyncServerSyncEstablishedTest,
       WatchAdvertisingReports) {
  // The watch request should hang until OnAdvertisingReport is called.
  std::vector<fidl::Result<WatchAdvertisingReport>> report_results;
  client()->WatchAdvertisingReport().Then(
      [&report_results](fidl::Result<WatchAdvertisingReport>& report) {
        report_results.emplace_back(std::move(report));
      });
  RunLoopUntilIdle();
  EXPECT_TRUE(report_results.empty());

  bt::AdvertisingData data_0;
  uint16_t company_id_0 = 0x98;
  bt::StaticByteBuffer manufacturer_bytes(0x04, 0x03);
  EXPECT_TRUE(
      data_0.SetManufacturerData(company_id_0, manufacturer_bytes.view()));
  bt::gap::PeriodicAdvertisingReport report_0{
      .data = std::move(data_0),
      .rssi = 8,
      .event_counter = 0,
  };
  delegate()->OnAdvertisingReport(kSyncId, report_0);
  RunLoopUntilIdle();
  ASSERT_EQ(report_results.size(), 1u);
  ASSERT_TRUE(report_results[0].is_ok());
  {
    auto reports = report_results[0]->reports();
    ASSERT_EQ(reports->size(), 1u);
    auto report = reports->front().periodic_advertising_report();
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->rssi(), report_0.rssi);
    EXPECT_EQ(report->event_counter(), report_0.event_counter);
    ASSERT_TRUE(report->data().has_value());
    ASSERT_TRUE(report->data()->manufacturer_data().has_value());
    auto manufacturer_data = report->data()->manufacturer_data().value();
    ASSERT_EQ(manufacturer_data.size(), 1u);
    ASSERT_EQ(manufacturer_data[0].company_id(), company_id_0);
    ASSERT_EQ(manufacturer_data[0].data(), manufacturer_bytes.ToVector());
  }

  // Queue a second report BEFORE calling WatchAdvertisingReport.
  bt::AdvertisingData data_1;
  uint16_t company_id_1 = 0x99;
  EXPECT_TRUE(
      data_1.SetManufacturerData(company_id_1, manufacturer_bytes.view()));
  bt::gap::PeriodicAdvertisingReport report_1{
      .data = std::move(data_1),
      .rssi = 9,
      .event_counter = 1,
  };
  delegate()->OnAdvertisingReport(kSyncId, report_1);
  RunLoopUntilIdle();
  ASSERT_EQ(report_results.size(), 1u);

  client()->WatchAdvertisingReport().Then(
      [&report_results](fidl::Result<WatchAdvertisingReport>& report) {
        report_results.emplace_back(std::move(report));
      });
  RunLoopUntilIdle();
  ASSERT_EQ(report_results.size(), 2u);
  ASSERT_TRUE(report_results[1].is_ok());
  {
    auto reports = report_results[1]->reports();
    ASSERT_EQ(reports->size(), 1u);
    auto report = reports->front().periodic_advertising_report();
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->rssi(), report_1.rssi);
    EXPECT_EQ(report->event_counter(), report_1.event_counter);
    ASSERT_TRUE(report->data().has_value());
    ASSERT_TRUE(report->data()->manufacturer_data().has_value());
    auto manufacturer_data = report->data()->manufacturer_data().value();
    ASSERT_EQ(manufacturer_data.size(), 1u);
    ASSERT_EQ(manufacturer_data[0].company_id(), company_id_1);
    ASSERT_EQ(manufacturer_data[0].data(), manufacturer_bytes.ToVector());
  }
}

TEST_F(PeriodicAdvertisingSyncServerSyncEstablishedTest, WatchBigInfoReports) {
  bt::hci_spec::BroadcastIsochronousGroupInfo big_report_0 = kBigInfo;
  delegate()->OnBigInfoReport(kSyncId, big_report_0);

  bt::gap::PeriodicAdvertisingSyncManager::BroadcastIsochronousGroupInfo
      big_report_1 = kBigInfo;
  big_report_1.num_bis = 99;
  delegate()->OnBigInfoReport(kSyncId, big_report_1);

  std::vector<fidl::Result<WatchAdvertisingReport>> report_results;
  client()->WatchAdvertisingReport().Then(
      [&report_results](fidl::Result<WatchAdvertisingReport>& report) {
        report_results.emplace_back(std::move(report));
      });
  RunLoopUntilIdle();
  ASSERT_EQ(report_results.size(), 1u);
  ASSERT_TRUE(report_results[0].is_ok());
  ASSERT_TRUE(report_results[0]->reports().has_value());
  auto reports = report_results[0]->reports().value();
  ASSERT_EQ(reports.size(), 2u);

  ASSERT_TRUE(reports[0].broadcast_isochronous_group_info_report().has_value());
  auto report_0 = reports[0].broadcast_isochronous_group_info_report().value();
  ASSERT_TRUE(report_0.info().has_value());
  EXPECT_EQ(report_0.info()->encryption(), big_report_0.encryption);
  EXPECT_EQ(report_0.info()->max_sdu_size(), big_report_0.max_sdu);
  EXPECT_EQ(report_0.info()->phy(), fble::PhysicalLayer::kLe1M);
  EXPECT_EQ(report_0.info()->streams_count(), big_report_0.num_bis);

  ASSERT_TRUE(reports[1].broadcast_isochronous_group_info_report().has_value());
  auto report_1 = reports[1].broadcast_isochronous_group_info_report().value();
  ASSERT_TRUE(report_1.info().has_value());
  EXPECT_EQ(report_1.info()->encryption(), big_report_1.encryption);
  EXPECT_EQ(report_1.info()->max_sdu_size(), big_report_1.max_sdu);
  EXPECT_EQ(report_1.info()->phy(), fble::PhysicalLayer::kLe1M);
  EXPECT_EQ(report_1.info()->streams_count(), big_report_1.num_bis);
}

TEST_F(PeriodicAdvertisingSyncServerSyncEstablishedTest, Cancel) {
  ASSERT_TRUE(client()->Cancel().is_ok());
  RunLoopUntilIdle();
  ASSERT_TRUE(event_handler().errors().empty());
  ASSERT_TRUE(event_handler().fidl_error().has_value());
  EXPECT_EQ(event_handler().fidl_error()->status(), ZX_ERR_CANCELED);
  EXPECT_EQ(closed_count(), 1);
  EXPECT_EQ(adapter()->fake_le()->periodic_advertisement_syncs().size(), 0u);
}

TEST_F(PeriodicAdvertisingSyncServerSyncEstablishedTest, OnSyncLost) {
  delegate()->OnSyncLost(
      kSyncId,
      bt::ToResult(pw::bluetooth::emboss::StatusCode::CONNECTION_TIMEOUT)
          .error_value());
  RunLoopUntilIdle();
  ASSERT_EQ(event_handler().errors().size(), 1u);
  EXPECT_EQ(event_handler().errors()[0],
            fble::PeriodicAdvertisingSyncError::kSynchronizationLost);
  ASSERT_TRUE(event_handler().fidl_error().has_value());
  EXPECT_EQ(event_handler().fidl_error()->status(), ZX_ERR_TIMED_OUT);
  EXPECT_EQ(closed_count(), 1);
  EXPECT_EQ(adapter()->fake_le()->periodic_advertisement_syncs().size(), 0u);
}

TEST_F(PeriodicAdvertisingSyncServerSyncEstablishedTest, MaxQueuedReports) {
  constexpr size_t max_queued_reports = 10;
  // Queue 1 more than the max.
  for (size_t i = 0; i < max_queued_reports + 1; i++) {
    bt::AdvertisingData data;
    bt::gap::PeriodicAdvertisingReport report{
        .data = std::move(data),
        .rssi = 9,
        .event_counter = static_cast<uint16_t>(i),
    };
    delegate()->OnAdvertisingReport(kSyncId, report);
  }

  std::vector<fidl::Result<WatchAdvertisingReport>> report_results;
  client()->WatchAdvertisingReport().Then(
      [&report_results](fidl::Result<WatchAdvertisingReport>& report) {
        report_results.emplace_back(std::move(report));
      });
  RunLoopUntilIdle();
  ASSERT_EQ(report_results.size(), 1u);
  ASSERT_TRUE(report_results[0].is_ok());
  auto reports = report_results[0]->reports().value();
  ASSERT_EQ(reports.size(), max_queued_reports);
  for (size_t i = 0; i < max_queued_reports; i++) {
    auto report = reports[i].periodic_advertising_report();
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->event_counter(), i + 1);
  }

  // This request should hang.
  client()->WatchAdvertisingReport().Then(
      [&report_results](fidl::Result<WatchAdvertisingReport>& report) {
        report_results.emplace_back(std::move(report));
      });
  RunLoopUntilIdle();
  ASSERT_EQ(report_results.size(), 1u);
}

}  // namespace bthost
