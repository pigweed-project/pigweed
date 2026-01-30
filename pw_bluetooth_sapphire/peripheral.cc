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

#include "pw_bluetooth_sapphire/peripheral.h"

namespace pw::bluetooth_sapphire {

static pw::sync::Mutex g_peripheral_lock;

using AdvertiseError = pw::bluetooth::low_energy::Peripheral2::AdvertiseError;
using LegacyAdvertising =
    pw::bluetooth::low_energy::Peripheral2::LegacyAdvertising;
using ExtendedAdvertising =
    pw::bluetooth::low_energy::Peripheral2::ExtendedAdvertising;
using ScanResponse = pw::bluetooth::low_energy::Peripheral2::ScanResponse;
using Anonymous =
    pw::bluetooth::low_energy::Peripheral2::ExtendedAdvertising::Anonymous;
using AdvertisedPeripheral2 = pw::bluetooth::low_energy::AdvertisedPeripheral2;

namespace {
bt::UUID UuidFrom(const pw::bluetooth::Uuid& uuid) {
  return bt::UUID(bt::BufferView(pw::as_bytes(uuid.As128BitSpan())));
}

pw::expected<bt::AdvertisingData, AdvertiseError> AdvertisingDataFrom(
    const pw::bluetooth::low_energy::AdvertisingData& data_in) {
  bt::AdvertisingData data_out;

  if (!data_out.SetLocalName(std::string(data_in.name))) {
    return pw::unexpected(AdvertiseError::kAdvertisingDataTooLong);
  }

  data_out.SetAppearance(static_cast<uint16_t>(data_in.appearance));

  for (const bluetooth::Uuid& service_uuid : data_in.service_uuids) {
    if (!data_out.AddServiceUuid(UuidFrom(service_uuid))) {
      return pw::unexpected(AdvertiseError::kAdvertisingDataTooLong);
    }
  }

  for (const bluetooth::low_energy::ServiceData& service_data :
       data_in.service_data) {
    if (!data_out.SetServiceData(UuidFrom(service_data.uuid),
                                 bt::BufferView(service_data.data))) {
      return pw::unexpected(AdvertiseError::kAdvertisingDataTooLong);
    }
  }

  for (const bluetooth::low_energy::ManufacturerData& manufacturer_data :
       data_in.manufacturer_data) {
    if (!data_out.SetManufacturerData(manufacturer_data.company_id,
                                      bt::BufferView(manufacturer_data.data))) {
      return pw::unexpected(AdvertiseError::kAdvertisingDataTooLong);
    }
  }

  for (const std::string_view uri : data_in.uris) {
    if (!data_out.AddUri(std::string(uri))) {
      return pw::unexpected(AdvertiseError::kAdvertisingDataTooLong);
    }
  }
  return data_out;
}

bt::DeviceAddress::Type DeviceAddressTypeFrom(
    pw::bluetooth::Address::Type address_type) {
  // TODO: https://pwbug.dev/377301546 - Support all types of random addresses
  // in DeviceAddress::Type.
  switch (address_type) {
    case bluetooth::Address::Type::kPublic:
      return bt::DeviceAddress::Type::kLEPublic;
    case bluetooth::Address::Type::kRandomStatic:
      return bt::DeviceAddress::Type::kLERandom;
    case bluetooth::Address::Type::kRandomResolvablePrivate:
      return bt::DeviceAddress::Type::kLERandom;
    case bluetooth::Address::Type::kRandomNonResolvablePrivate:
      return bt::DeviceAddress::Type::kLERandom;
  }
}

bt::sm::BondableMode BondableModeFrom(bool bondable) {
  return bondable ? bt::sm::BondableMode::Bondable
                  : bt::sm::BondableMode::NonBondable;
}

AdvertiseError AdvertiseErrorFrom(bt::hci::Result<> result) {
  if (result.error_value().is(bt::HostError::kNotSupported)) {
    return AdvertiseError::kNotSupported;
  } else if (result.error_value().is(bt::HostError::kInvalidParameters)) {
    return AdvertiseError::kInvalidParameters;
  } else if (result.error_value().is(bt::HostError::kAdvertisingDataTooLong)) {
    return AdvertiseError::kAdvertisingDataTooLong;
  } else if (result.error_value().is(bt::HostError::kScanResponseTooLong)) {
    return AdvertiseError::kScanResponseDataTooLong;
  }
  return AdvertiseError::kFailed;
}

}  // namespace

Peripheral::Peripheral(bt::gap::Adapter::WeakPtr adapter,
                       pw::async::Dispatcher& dispatcher)
    : dispatcher_(dispatcher), adapter_(std::move(adapter)) {}

Peripheral::~Peripheral() {
  weak_factory_.InvalidatePtrs();
  {
    std::lock_guard guard(lock());
    for (auto& [_, adv] : advertisements_) {
      adv.OnStopLocked(pw::Status::Cancelled());
    }
    advertisements_.clear();
  }
}

async2::OptionalValueFuture<Peripheral::AdvertiseResult> Peripheral::Advertise(
    const AdvertisingParameters& parameters) {
  pw::expected<bt::AdvertisingData, AdvertiseError> data_result =
      AdvertisingDataFrom(parameters.data);
  if (!data_result.has_value()) {
    return async2::OptionalValueFuture<AdvertiseResult>::Resolved(
        pw::unexpected(data_result.error()));
  }
  bt::AdvertisingData data = std::move(data_result.value());
  bt::AdvertisingData scan_response;
  bool include_tx_power_level = parameters.data.include_tx_power_level;

  // TODO: https://pwbug.dev/377301546 - Use parameters.interval_range & update
  // internal API to accept a range instead of AdvertisingInterval.
  bt::gap::AdvertisingInterval interval = bt::gap::AdvertisingInterval::SLOW;

  std::optional<bt::gap::Adapter::LowEnergy::ConnectableAdvertisingParameters>
      connectable;

  auto connection_cb =
      [self = self_](bt::gap::AdvertisementId advertisement_id,
                     bt::gap::Adapter::LowEnergy::ConnectionResult result) {
        if (self.is_alive()) {
          self->OnConnection(advertisement_id, std::move(result));
        }
      };

  std::optional<bt::DeviceAddress::Type> address_type;
  if (parameters.address_type.has_value()) {
    address_type = DeviceAddressTypeFrom(parameters.address_type.value());
  }

  bool anonymous = false;
  bool extended_pdu = false;

  if (const LegacyAdvertising* legacy =
          std::get_if<LegacyAdvertising>(&parameters.procedure)) {
    if (legacy->scan_response.has_value()) {
      pw::expected<bt::AdvertisingData, AdvertiseError> scan_response_result =
          AdvertisingDataFrom(legacy->scan_response.value());
      if (!scan_response_result.has_value()) {
        return async2::OptionalValueFuture<AdvertiseResult>::Resolved(
            pw::unexpected(scan_response_result.error()));
      }
      scan_response = std::move(scan_response_result.value());
    }

    if (legacy->connection_options.has_value()) {
      connectable.emplace();
      connectable->connection_cb = std::move(connection_cb);
      connectable->bondable_mode =
          BondableModeFrom(legacy->connection_options->bondable_mode);

      // TODO: https://pwbug.dev/377301546 - Use remaining connection options.
      // Requires modifying Adapter::LowEnergy::StartAdvertising.
    }
  } else if (const ExtendedAdvertising* extended =
                 std::get_if<ExtendedAdvertising>(&parameters.procedure)) {
    extended_pdu = true;

    if (std::get_if<Anonymous>(&extended->configuration)) {
      anonymous = true;
    } else if (const ScanResponse* scan_response_param =
                   std::get_if<ScanResponse>(&extended->configuration)) {
      pw::expected<bt::AdvertisingData, AdvertiseError> scan_response_result =
          AdvertisingDataFrom(*scan_response_param);
      if (!scan_response_result.has_value()) {
        return async2::OptionalValueFuture<AdvertiseResult>::Resolved(
            pw::unexpected(scan_response_result.error()));
      }
      scan_response = std::move(scan_response_result.value());
    } else if (const ConnectionOptions* connection_options =
                   std::get_if<ConnectionOptions>(&extended->configuration)) {
      connectable.emplace();
      connectable->connection_cb = std::move(connection_cb);
      connectable->bondable_mode =
          BondableModeFrom(connection_options->bondable_mode);

      // TODO: https://pwbug.dev/377301546 - Use remaining connection options.
      // Requires modifying Adapter::LowEnergy::StartAdvertising.
    }
  } else {
    // This should never happen unless additional procedures are added in the
    // future.
    bt_log(WARN, "api", "Advertising procedure not supported");
    return async2::OptionalValueFuture<AdvertiseResult>::Resolved(
        pw::unexpected(AdvertiseError::kNotSupported));
  }

  async2::OptionalValueProvider<Peripheral::AdvertiseResult> result_provider;
  auto result_future = result_provider.Get();
  auto callback = [self = self_, provider = std::move(result_provider)](
                      bt::gap::AdvertisementInstance instance,
                      bt::hci::Result<> status) mutable {
    if (self.is_alive()) {
      self->OnAdvertiseResult(std::move(instance), status, std::move(provider));
    }
  };

  // Post to BT dispatcher for thread safety.
  pw::Status post_status =
      dispatcher_.Post([adapter = adapter_,
                        adv_data = std::move(data),
                        scan_rsp = std::move(scan_response),
                        interval,
                        extended_pdu,
                        anonymous,
                        include_tx_power_level,
                        connection_options = std::move(connectable),
                        address_type,
                        cb = std::move(callback)](pw::async::Context&,
                                                  pw::Status status) mutable {
        if (!status.ok()) {
          // Dispatcher is shutting down.
          return;
        }
        adapter->le()->StartAdvertising(std::move(adv_data),
                                        std::move(scan_rsp),
                                        interval,
                                        extended_pdu,
                                        anonymous,
                                        include_tx_power_level,
                                        std::move(connection_options),
                                        address_type,
                                        std::move(cb));
      });
  PW_CHECK_OK(post_status);

  return result_future;
}

pw::sync::Mutex& Peripheral::lock() { return g_peripheral_lock; }

void Peripheral::AdvertisedPeripheralImpl::StopAdvertising() {
  std::lock_guard guard(Peripheral::lock());
  if (!peripheral_) {
    return;
  }

  peripheral_->StopAdvertising(id_);
}

async2::Poll<pw::Status> Peripheral::AdvertisedPeripheralImpl::PendStop(
    async2::Context& cx) {
  std::lock_guard guard(Peripheral::lock());
  if (stop_status_.has_value()) {
    return async2::Ready(stop_status_.value());
  }
  PW_ASYNC_STORE_WAKER(cx, waker_, "AdvPeripheralPendStop");
  return async2::Pending();
}

Peripheral::Advertisement::~Advertisement() { OnStopLocked(OkStatus()); }

void Peripheral::Advertisement::OnStopLocked(pw::Status status) {
  if (!advertised_peripheral_) {
    return;
  }

  advertised_peripheral_->stop_status_ = status;
  advertised_peripheral_->peripheral_ = nullptr;
  advertised_peripheral_->waker_.Wake();
  advertised_peripheral_ = nullptr;
}

void Peripheral::OnAdvertisedPeripheralDestroyedLocked(
    bt::gap::AdvertisementId advertisement_id) {
  auto iter = advertisements_.find(advertisement_id);
  if (iter == advertisements_.end()) {
    return;
  }
  iter->second.OnAdvertisedPeripheralDestroyedLocked();
  StopAdvertising(advertisement_id);
}

void Peripheral::StopAdvertising(bt::gap::AdvertisementId advertisement_id) {
  // Post to BT dispatcher for thread safety.
  pw::Status post_status =
      dispatcher_.Post([self = self_, id = advertisement_id](
                           pw::async::Context&, pw::Status status) {
        if (!self.is_alive() || !status.ok()) {
          return;
        }
        std::lock_guard guard(Peripheral::lock());
        // TODO: https://pwbug.dev/377301546 - Implement a callback for when
        // advertising is actually stopped. This just destroys the
        // AdvertisementInstance and does not wait for advertising to actually
        // stop, so it does not properly implement
        // AdvertisedPeripheral2::StopAdvertising().
        self->advertisements_.erase(id);
      });
  PW_CHECK_OK(post_status);
}

void Peripheral::OnAdvertiseResult(
    bt::gap::AdvertisementInstance instance,
    bt::hci::Result<> result,
    async2::OptionalValueProvider<AdvertiseResult> result_provider) {
  if (result.is_error()) {
    result_provider.Resolve(pw::unexpected(AdvertiseErrorFrom(result)));
    return;
  }

  bt::gap::AdvertisementId id = instance.id();

  AdvertisedPeripheralImpl* impl = new AdvertisedPeripheralImpl(id, this);
  AdvertisedPeripheral2::Ptr advertised_peripheral(impl);

  std::lock_guard guard(lock());
  auto [iter, inserted] =
      advertisements_.try_emplace(id, std::move(instance), impl);
  PW_CHECK(inserted);

  result_provider.Resolve(std::move(advertised_peripheral));
}

void Peripheral::OnConnection(
    bt::gap::AdvertisementId /*advertisement_id*/,
    bt::gap::Adapter::LowEnergy::ConnectionResult /*result*/) {
  // TODO: https://pwbug.dev/377301546 - Implement connection handling.
}

}  // namespace pw::bluetooth_sapphire
