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

#include "pw_bluetooth_sapphire/internal/host/testing/fake_peer.h"

#include <pw_assert/check.h>

#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/l2cap_defs.h"
#include "pw_bluetooth_sapphire/internal/host/testing/fake_controller.h"

namespace bt::testing {
using pw::bluetooth::emboss::LEExtendedAdvertisingReportDataWriter;

FakePeer::FakePeer(const DeviceAddress& address,
                   pw::async::Dispatcher& pw_dispatcher,
                   bool connectable,
                   bool scannable,
                   bool send_advertising_report)
    : controller_(nullptr),
      address_(address),
      name_("FakePeer"),
      connected_(false),
      connectable_(connectable),
      scannable_(scannable),
      advertising_enabled_(true),
      directed_(false),
      address_resolved_(false),
      send_advertising_report_(send_advertising_report),
      connect_status_(pw::bluetooth::emboss::StatusCode::SUCCESS),
      connect_response_(pw::bluetooth::emboss::StatusCode::SUCCESS),
      force_pending_connect_(false),
      supports_ll_conn_update_procedure_(true),
      le_features_(hci_spec::LESupportedFeatures{0}),
      l2cap_(fit::bind_member<&FakePeer::SendPacket>(this)),
      gatt_server_(this),
      sdp_server_(pw_dispatcher) {
  signaling_server_.RegisterWithL2cap(&l2cap_);
  gatt_server_.RegisterWithL2cap(&l2cap_);
  sdp_server_.RegisterWithL2cap(&l2cap_);
}

void FakePeer::set_scan_response(const ByteBuffer& data) {
  PW_DCHECK(scannable_);
  scan_response_ = DynamicByteBuffer(data);
}

DynamicByteBuffer FakePeer::CreateInquiryResponseEvent(
    pw::bluetooth::emboss::InquiryMode mode) const {
  PW_DCHECK(address_.type() == DeviceAddress::Type::kBREDR);

  if (mode == pw::bluetooth::emboss::InquiryMode::STANDARD) {
    size_t packet_size =
        pw::bluetooth::emboss::InquiryResultEvent::MinSizeInBytes() +
        pw::bluetooth::emboss::InquiryResult::IntrinsicSizeInBytes();
    auto packet =
        hci::EventPacket::New<pw::bluetooth::emboss::InquiryResultEventWriter>(
            hci_spec::kInquiryResultEventCode, packet_size);
    auto view = packet.view_t();
    view.num_responses().Write(1);
    view.responses()[0].bd_addr().CopyFrom(address_.value().view());
    view.responses()[0].page_scan_repetition_mode().Write(
        pw::bluetooth::emboss::PageScanRepetitionMode::R0_);
    view.responses()[0].class_of_device().BackingStorage().WriteUInt(
        class_of_device_.to_int());
    return DynamicByteBuffer{packet.data()};
  }

  constexpr size_t packet_size =
      pw::bluetooth::emboss::InquiryResultWithRssiEvent::MinSizeInBytes() +
      pw::bluetooth::emboss::InquiryResultWithRssi::IntrinsicSizeInBytes();
  auto packet = hci::EventPacket::New<
      pw::bluetooth::emboss::InquiryResultWithRssiEventWriter>(
      hci_spec::kInquiryResultEventCode, packet_size);
  auto view = packet.view_t();

  // TODO(jamuraa): simulate clock offset and RSSI
  view.num_responses().Write(1);
  auto response = view.responses()[0];
  response.bd_addr().CopyFrom(address_.value().view());
  response.page_scan_repetition_mode().Write(
      pw::bluetooth::emboss::PageScanRepetitionMode::R0_);
  response.class_of_device().BackingStorage().WriteUInt(
      class_of_device_.to_int());
  response.clock_offset().BackingStorage().WriteUInt(0);
  response.rssi().Write(-30);
  return DynamicByteBuffer{packet.data()};
}

void FakePeer::AddLink(hci_spec::ConnectionHandle handle) {
  PW_DCHECK(!HasLink(handle));
  logical_links_.insert(handle);

  if (logical_links_.size() == 1u) {
    set_connected(true);
  }
}

void FakePeer::RemoveLink(hci_spec::ConnectionHandle handle) {
  PW_DCHECK(HasLink(handle));
  logical_links_.erase(handle);
  if (logical_links_.empty())
    set_connected(false);
}

bool FakePeer::HasLink(hci_spec::ConnectionHandle handle) const {
  return logical_links_.count(handle) != 0u;
}

FakePeer::HandleSet FakePeer::Disconnect() {
  set_connected(false);
  return std::move(logical_links_);
}

void FakePeer::OnRxL2CAP(hci_spec::ConnectionHandle conn,
                         const ByteBuffer& pdu) {
  if (pdu.size() < sizeof(l2cap::BasicHeader)) {
    bt_log(WARN, "fake-hci", "malformed L2CAP packet!");
    return;
  }
  l2cap_.HandlePdu(conn, pdu);
}

void FakePeer::SendPacket(hci_spec::ConnectionHandle conn,
                          l2cap::ChannelId cid,
                          const ByteBuffer& packet) const {
  controller()->SendL2CAPBFrame(conn, cid, packet);
}

void FakePeer::WriteScanResponseReport(
    pw::bluetooth::emboss::LEAdvertisingReportDataWriter report) const {
  PW_DCHECK(scannable_);
  PW_DCHECK(scan_response_.size() < 0xFF);
  report.data_length().Write(static_cast<uint8_t>(scan_response_.size()));
  report = pw::bluetooth::emboss::MakeLEAdvertisingReportDataView(
      report.BackingStorage().data(),
      report.SizeInBytes() + scan_response_.size());

  report.event_type().Write(
      pw::bluetooth::emboss::LEAdvertisingEventType::SCAN_RESPONSE);

  report.address().CopyFrom(address_.value().view());
  report.address_type().Write(
      address_.type() == DeviceAddress::Type::kLERandom
          ? pw::bluetooth::emboss::LEAddressType::RANDOM
          : pw::bluetooth::emboss::LEAddressType::PUBLIC);

  if (scan_response_.data() != nullptr) {
    std::memcpy(report.data().BackingStorage().data(),
                scan_response_.data(),
                scan_response_.size());
  }

  report.rssi().Write(rssi());
}

DynamicByteBuffer FakePeer::BuildLegacyAdvertisingReportEvent(
    bool include_scan_rsp) const {
  PW_DCHECK(advertising_data_.size() <= hci_spec::kMaxLEAdvertisingDataLength);
  std::vector<size_t> reports_sizes;
  reports_sizes.push_back(
      pw::bluetooth::emboss::LEAdvertisingReportData::MinSizeInBytes() +
      advertising_data_.size());
  size_t reports_size = reports_sizes[0];

  if (include_scan_rsp) {
    PW_DCHECK(scannable_);
    reports_sizes.push_back(
        pw::bluetooth::emboss::LEAdvertisingReportData::MinSizeInBytes() +
        scan_response_.size());
    reports_size += reports_sizes[1];
  }

  size_t packet_size =
      reports_size +
      pw::bluetooth::emboss::LEAdvertisingReportSubevent::MinSizeInBytes();
  auto event = hci::EventPacket::New<
      pw::bluetooth::emboss::LEAdvertisingReportSubeventWriter>(
      hci_spec::kLEMetaEventCode, packet_size);

  auto view = event.view_t();
  view.le_meta_event().subevent_code().Write(
      hci_spec::kLEAdvertisingReportSubeventCode);

  PW_DCHECK(reports_sizes.size() < 0xFF);
  view.num_reports().Write(static_cast<uint8_t>(reports_sizes.size()));

  // Initially construct an incomplete view to write the |data_length| field.
  auto report = pw::bluetooth::emboss::MakeLEAdvertisingReportDataView(
      view.reports().BackingStorage().data(), reports_sizes[0]);
  report.data_length().Write(static_cast<int32_t>(advertising_data_.size()));
  // Remake view with the proper size, taking into account |data_length|.
  report = pw::bluetooth::emboss::MakeLEAdvertisingReportDataView(
      view.reports().BackingStorage().data(), reports_sizes[0]);
  if (directed_) {
    report.event_type().Write(
        pw::bluetooth::emboss::LEAdvertisingEventType::CONNECTABLE_DIRECTED);
  } else if (connectable_) {
    report.event_type().Write(pw::bluetooth::emboss::LEAdvertisingEventType::
                                  CONNECTABLE_AND_SCANNABLE_UNDIRECTED);
  } else if (scannable_) {
    report.event_type().Write(
        pw::bluetooth::emboss::LEAdvertisingEventType::SCANNABLE_UNDIRECTED);
  } else {
    report.event_type().Write(pw::bluetooth::emboss::LEAdvertisingEventType::
                                  NON_CONNECTABLE_UNDIRECTED);
  }
  if (address_.type() == DeviceAddress::Type::kLERandom) {
    report.address_type().Write(
        address_resolved_
            ? pw::bluetooth::emboss::LEAddressType::RANDOM_IDENTITY
            : pw::bluetooth::emboss::LEAddressType::RANDOM);
  } else {
    report.address_type().Write(
        address_resolved_
            ? pw::bluetooth::emboss::LEAddressType::PUBLIC_IDENTITY
            : pw::bluetooth::emboss::LEAddressType::PUBLIC);
  }
  report.address().CopyFrom(address_.value().view());

  if (advertising_data_.data() != nullptr) {
    std::memcpy(report.data().BackingStorage().data(),
                advertising_data_.data(),
                advertising_data_.size());
  }

  report.rssi().Write(rssi());

  if (include_scan_rsp) {
    WriteScanResponseReport(
        pw::bluetooth::emboss::MakeLEAdvertisingReportDataView(
            view.reports().BackingStorage().data() + reports_sizes[0],
            reports_sizes[1]));
  }

  return DynamicByteBuffer(event.data());
}

DynamicByteBuffer FakePeer::BuildLegacyScanResponseReportEvent() const {
  PW_DCHECK(scannable_);
  PW_DCHECK(scan_response_.size() <= hci_spec::kMaxLEAdvertisingDataLength);
  size_t report_size =
      pw::bluetooth::emboss::LEAdvertisingReportData::MinSizeInBytes() +
      scan_response_.size();
  size_t packet_size =
      report_size +
      pw::bluetooth::emboss::LEAdvertisingReportSubevent::MinSizeInBytes();
  auto event = hci::EventPacket::New<
      pw::bluetooth::emboss::LEAdvertisingReportSubeventWriter>(
      hci_spec::kLEMetaEventCode, packet_size);
  auto view = event.view_t();
  view.le_meta_event().subevent_code().Write(
      hci_spec::kLEAdvertisingReportSubeventCode);
  view.num_reports().Write(1);

  WriteScanResponseReport(
      pw::bluetooth::emboss::MakeLEAdvertisingReportDataView(
          view.reports().BackingStorage().data(), report_size));

  return DynamicByteBuffer(event.data());
}

void FakePeer::FillExtendedAdvertisingReport(
    LEExtendedAdvertisingReportDataWriter report,
    const ByteBuffer& data,
    bool is_fragmented,
    bool is_scan_response) const {
  if (use_extended_advertising_pdus_) {
    report.event_type().directed().Write(directed_);
    report.event_type().connectable().Write(connectable_);
    report.event_type().scannable().Write(scannable_);
    report.event_type().scan_response().Write(is_scan_response);

    if (is_fragmented) {
      report.event_type().data_status().Write(
          pw::bluetooth::emboss::LEAdvertisingDataStatus::INCOMPLETE);
    } else {
      report.event_type().data_status().Write(
          pw::bluetooth::emboss::LEAdvertisingDataStatus::COMPLETE);
    }
  } else {
    report.event_type().legacy().Write(true);
    if (is_scan_response) {
      report.event_type().scan_response().Write(true);
    }

    if (directed_) {  // ADV_DIRECT_IND
      report.event_type().directed().Write(true);
      report.event_type().connectable().Write(true);
    } else if (connectable_) {  // ADV_IND
      report.event_type().connectable().Write(true);
      report.event_type().scannable().Write(true);
    } else if (scannable_) {  // ADV_SCAN_IND
      report.event_type().scannable().Write(true);
    }
    // else ADV_NONCONN_IND
  }

  if (address_.type() == DeviceAddress::Type::kLERandom) {
    if (address_resolved_) {
      report.address_type().Write(
          pw::bluetooth::emboss::LEExtendedAddressType::RANDOM_IDENTITY);
    } else {
      report.address_type().Write(
          pw::bluetooth::emboss::LEExtendedAddressType::RANDOM);
    }
  } else {
    if (address_resolved_) {
      report.address_type().Write(
          pw::bluetooth::emboss::LEExtendedAddressType::PUBLIC_IDENTITY);
    } else {
      report.address_type().Write(
          pw::bluetooth::emboss::LEExtendedAddressType::PUBLIC);
    }
  }

  report.address().bd_addr().CopyFrom(address_.value().view().bd_addr());
  report.primary_phy().Write(
      pw::bluetooth::emboss::LEPrimaryAdvertisingPHY::LE_1M);
  report.secondary_phy().Write(
      pw::bluetooth::emboss::LESecondaryAdvertisingPHY::NONE);
  report.advertising_sid().Write(advertising_sid());
  report.tx_power().Write(tx_power());
  report.rssi().Write(rssi());
  report.periodic_advertising_interval().Write(periodic_advertising_interval());

  // skip direct_address_type and direct_address for now since we don't use it

  PW_DCHECK(data.size() < 0xFF);
  report.data_length().Write(static_cast<uint8_t>(data.size()));

  if (!data.empty()) {
    std::memcpy(
        report.data().BackingStorage().begin(), data.data(), data.size());
  }
}

void FakePeer::AddPeriodicAdvertisement(
    uint8_t advertising_sid,
    DynamicByteBuffer data,
    std::optional<hci_spec::BroadcastIsochronousGroupInfo> big_info) {
  auto [_, inserted] = periodic_advertisements_.try_emplace(
      advertising_sid,
      PeriodicAdvertisement{
          .data = std::move(data), .event_counter = 0, .big_info = big_info});
  PW_CHECK(inserted);
}

void FakePeer::RemovePeriodicAdvertisement(uint8_t advertising_sid) {
  PW_CHECK(periodic_advertisements_.erase(advertising_sid));
}

DynamicByteBuffer FakePeer::BuildExtendedAdvertisingReports(
    const ByteBuffer& data, bool is_scan_response) const {
  namespace LEExtendedAdvertisingReportData =
      pw::bluetooth::emboss::LEExtendedAdvertisingReportData;
  using pw::bluetooth::emboss::LEExtendedAdvertisingReportDataWriter;
  using pw::bluetooth::emboss::LEExtendedAdvertisingReportSubeventWriter;

  size_t num_full_reports =
      data.size() / LEExtendedAdvertisingReportData::data_length_max();
  size_t full_report_size =
      pw::bluetooth::emboss::LEExtendedAdvertisingReportData::MinSizeInBytes() +
      LEExtendedAdvertisingReportData::data_length_max();
  size_t last_report_size =
      pw::bluetooth::emboss::LEExtendedAdvertisingReportData::MinSizeInBytes() +
      (data.size() % LEExtendedAdvertisingReportData::data_length_max());

  size_t reports_size = num_full_reports * full_report_size + last_report_size;
  size_t packet_size =
      pw::bluetooth::emboss::LEExtendedAdvertisingReportSubevent::
          MinSizeInBytes() +
      reports_size;

  auto event = hci::EventPacket::New<LEExtendedAdvertisingReportSubeventWriter>(
      hci_spec::kLEMetaEventCode, packet_size);
  auto packet = event.view<LEExtendedAdvertisingReportSubeventWriter>();
  packet.le_meta_event().subevent_code().Write(
      hci_spec::kLEExtendedAdvertisingReportSubeventCode);

  uint8_t num_reports = static_cast<uint8_t>(num_full_reports + 1);
  packet.num_reports().Write(num_reports);

  for (size_t i = 0; i < num_full_reports; i++) {
    bool is_fragmented = false;
    if (num_reports > 1) {
      is_fragmented = true;
    }

    LEExtendedAdvertisingReportDataWriter report(
        packet.reports().BackingStorage().begin() + full_report_size * i,
        full_report_size);
    FillExtendedAdvertisingReport(
        report, data, is_fragmented, is_scan_response);
  }

  LEExtendedAdvertisingReportDataWriter report(
      packet.reports().BackingStorage().begin() +
          full_report_size * num_full_reports,
      last_report_size);
  FillExtendedAdvertisingReport(
      report, data, /*is_fragmented=*/false, is_scan_response);

  return event.release();
}

DynamicByteBuffer FakePeer::BuildExtendedAdvertisingReportEvent() const {
  PW_DCHECK(advertising_data_.size() <=
            hci_spec::kMaxLEExtendedAdvertisingDataLength);
  return BuildExtendedAdvertisingReports(advertising_data_,
                                         /*is_scan_response=*/false);
}

DynamicByteBuffer FakePeer::BuildExtendedScanResponseEvent() const {
  PW_DCHECK(scannable_);
  PW_DCHECK(scan_response_.size() <=
            hci_spec::kMaxLEExtendedAdvertisingDataLength);
  return BuildExtendedAdvertisingReports(scan_response_,
                                         /*is_scan_response=*/true);
}

DynamicByteBuffer FakePeer::BuildPeriodicAdvertisingReportEvent(
    uint16_t sync_handle, uint8_t advertising_sid) {
  auto iter = periodic_advertisements_.find(advertising_sid);
  PW_CHECK(iter != periodic_advertisements_.end());

  size_t packet_size =
      pw::bluetooth::emboss::LEPeriodicAdvertisingReportSubeventV2::
          MinSizeInBytes() +
      iter->second.data.size();

  auto event = hci::EventPacket::New<
      pw::bluetooth::emboss::LEPeriodicAdvertisingReportSubeventV2Writer>(
      hci_spec::kLEMetaEventCode, packet_size);
  auto view = event.view_t();
  view.le_meta_event().subevent_code_enum().Write(
      pw::bluetooth::emboss::LeSubEventCode::PERIODIC_ADVERTISING_REPORT_V2);
  view.sync_handle().Write(sync_handle);
  view.tx_power().Write(tx_power());
  view.rssi().Write(rssi());
  view.cte_type().Write(pw::bluetooth::emboss::LECteType::NO_CTE);
  uint16_t event_counter = iter->second.event_counter++;
  view.periodic_event_counter().Write(event_counter);
  view.subevent().Write(0xFF);  // No subevents
  view.data_status().Write(
      pw::bluetooth::emboss::LEPeriodicAdvertisingDataStatus::COMPLETE);
  view.data_length().Write(iter->second.data.size());
  std::memcpy(view.data().BackingStorage().data(),
              iter->second.data.data(),
              iter->second.data.size());
  return event.release();
}

std::optional<DynamicByteBuffer> FakePeer::BuildBigInfoAdvertisingReportEvent(
    hci_spec::SyncHandle sync_handle, uint8_t advertising_sid) const {
  auto iter = periodic_advertisements_.find(advertising_sid);
  PW_CHECK(iter != periodic_advertisements_.end());
  if (!iter->second.big_info.has_value()) {
    return std::nullopt;
  }
  hci_spec::BroadcastIsochronousGroupInfo info = iter->second.big_info.value();

  size_t packet_size =
      pw::bluetooth::emboss::LEBigInfoAdvertisingReportSubeventView::
          MinSizeInBytes()
              .Read();

  auto event = hci::EventPacket::New<
      pw::bluetooth::emboss::LEBigInfoAdvertisingReportSubeventWriter>(
      hci_spec::kLEMetaEventCode, packet_size);
  auto view = event.view_t();
  view.le_meta_event().subevent_code_enum().Write(
      pw::bluetooth::emboss::LeSubEventCode::BIG_INFO_ADVERTISING_REPORT);
  view.sync_handle().Write(sync_handle);
  view.num_bis().Write(info.num_bis);
  view.nse().Write(info.nse);
  view.iso_interval().Write(info.iso_interval);
  view.bn().Write(info.bn);
  view.pto().Write(info.pto);
  view.irc().Write(info.irc);
  view.max_pdu().Write(info.max_pdu);
  view.sdu_interval().Write(info.sdu_interval);
  view.max_sdu().Write(info.max_sdu);
  view.phy().Write(info.phy);
  view.framing().Write(info.framing);
  view.encryption().Write(static_cast<uint8_t>(info.encryption));
  return event.release();
}

std::optional<FakePeer::PeriodicAdvertisingSyncTransfer>
FakePeer::FindPeriodicAdvertisingSyncTransfer(DeviceAddress peer_address,
                                              uint8_t sid) {
  auto iter = received_periodic_advertising_sync_transfers_.find(
      std::make_pair(peer_address, sid));
  if (iter == received_periodic_advertising_sync_transfers_.end()) {
    return std::nullopt;
  }
  return iter->second;
}

}  // namespace bt::testing
