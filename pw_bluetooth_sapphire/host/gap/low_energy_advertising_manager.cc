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

#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_advertising_manager.h"

#include <pw_assert/check.h>

#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"

namespace bt::gap {

AdvertisementInstance::AdvertisementInstance() : id_(kInvalidAdvertisementId) {}

AdvertisementInstance::AdvertisementInstance(
    AdvertisementId advertisement_id,
    fit::function<void(AdvertisementId)> stop_advertising)
    : id_(advertisement_id), stop_advertising_(std::move(stop_advertising)) {
  PW_DCHECK(id_ != kInvalidAdvertisementId);
  PW_DCHECK(stop_advertising_ != nullptr);
}

AdvertisementInstance::~AdvertisementInstance() { Reset(); }

void AdvertisementInstance::Move(AdvertisementInstance* other) {
  // Destroy the old advertisement instance if active and clear the contents.
  Reset();

  // Transfer the old data over and clear |other| so that it no longer owns its
  // advertisement.
  stop_advertising_ = std::move(other->stop_advertising_);
  id_ = other->id_;
  other->id_ = kInvalidAdvertisementId;
}

void AdvertisementInstance::Reset() {
  if (stop_advertising_ && id_ != kInvalidAdvertisementId) {
    stop_advertising_(id_);
  }

  stop_advertising_ = nullptr;
  id_ = kInvalidAdvertisementId;
}

LowEnergyAdvertisingManager::LowEnergyAdvertisingManager(
    hci::LowEnergyAdvertiser* advertiser,
    hci::LocalAddressDelegate* local_addr_delegate)
    : advertiser_(advertiser),
      local_addr_delegate_(local_addr_delegate),
      weak_self_(this) {
  PW_DCHECK(advertiser_);
  PW_DCHECK(local_addr_delegate_);
}

void LowEnergyAdvertisingManager::set_slow_advertising_interval(uint32_t min,
                                                                uint32_t max) {
  if (min == 0 || max == 0) {
    bt_log(INFO,
           "gap-le",
           "ignoring request: no advertising interval configuration provided, "
           "using current values [%#.4x, %#.4x]",
           slow_interval_.min(),
           slow_interval_.max());
    return;
  }

  uint32_t limit_min = advertiser_->IsExtendedAdvertiser()
                           ? hci_spec::kLEExtendedAdvertisingIntervalMin
                           : hci_spec::kLEAdvertisingIntervalMin;
  uint32_t limit_max = advertiser_->IsExtendedAdvertiser()
                           ? hci_spec::kLEExtendedAdvertisingIntervalMax
                           : hci_spec::kLEAdvertisingIntervalMax;

  if (min < limit_min || max > limit_max || min > max) {
    bt_log(
        WARN,
        "gap-le",
        "ignoring request: advertising interval out of range: [%#.4x, %#.4x] "
        "(range: [%#.4x, %#.4x]) (current: [%#.4x, %#.4x])",
        min,
        max,
        limit_min,
        limit_max,
        slow_interval_.min(),
        slow_interval_.max());
    return;
  }

  bt_log(INFO,
         "gap-le",
         "setting slow advertising interval to [%#.4x, %#.4x]",
         min,
         max);
  slow_interval_ = {min, max};
}

void LowEnergyAdvertisingManager::set_fast_advertising_interval(uint32_t min,
                                                                uint32_t max) {
  if (min == 0 || max == 0) {
    bt_log(INFO,
           "gap-le",
           "ignoring request: no advertising interval configuration provided, "
           "using current values [%#.4x, %#.4x]",
           fast_interval_.min(),
           fast_interval_.max());
    return;
  }

  uint32_t limit_min = advertiser_->IsExtendedAdvertiser()
                           ? hci_spec::kLEExtendedAdvertisingIntervalMin
                           : hci_spec::kLEAdvertisingIntervalMin;
  uint32_t limit_max = advertiser_->IsExtendedAdvertiser()
                           ? hci_spec::kLEExtendedAdvertisingIntervalMax
                           : hci_spec::kLEAdvertisingIntervalMax;

  if (min < limit_min || max > limit_max || min > max) {
    bt_log(
        WARN,
        "gap-le",
        "ignoring request: advertising interval out of range: [%#.4x, %#.4x] "
        "(range: [%#.4x, %#.4x]) (current: [%#.4x, %#.4x])",
        min,
        max,
        limit_min,
        limit_max,
        fast_interval_.min(),
        fast_interval_.max());
    return;
  }

  bt_log(INFO,
         "gap-le",
         "setting fast advertising interval to [%#.4x, %#.4x]",
         min,
         max);
  fast_interval_ = {min, max};
}

void LowEnergyAdvertisingManager::set_very_fast_advertising_interval(
    uint32_t min, uint32_t max) {
  if (min == 0 || max == 0) {
    bt_log(INFO,
           "gap-le",
           "ignoring request: no advertising interval configuration provided, "
           "using current values [%#.4x, %#.4x]",
           very_fast_interval_.min(),
           very_fast_interval_.max());
    return;
  }

  uint32_t limit_min = advertiser_->IsExtendedAdvertiser()
                           ? hci_spec::kLEExtendedAdvertisingIntervalMin
                           : hci_spec::kLEAdvertisingIntervalMin;
  uint32_t limit_max = advertiser_->IsExtendedAdvertiser()
                           ? hci_spec::kLEExtendedAdvertisingIntervalMax
                           : hci_spec::kLEAdvertisingIntervalMax;

  if (min < limit_min || max > limit_max || min > max) {
    bt_log(
        WARN,
        "gap-le",
        "ignoring request: advertising interval out of range: [%#.4x, %#.4x] "
        "(range: [%#.4x, %#.4x]) (current: [%#.4x, %#.4x])",
        min,
        max,
        limit_min,
        limit_max,
        very_fast_interval_.min(),
        very_fast_interval_.max());
    return;
  }

  bt_log(INFO,
         "gap-le",
         "setting very fast advertising interval to [%#.4x, %#.4x]",
         min,
         max);
  very_fast_interval_ = {min, max};
}

void LowEnergyAdvertisingManager::set_slow_adv_max_tx_power(int8_t power) {
  if ((power < hci_spec::kLEAdvertisingTxPowerMin ||
       power > hci_spec::kLEAdvertisingTxPowerMax) &&
      power != hci_spec::kLEExtendedAdvertisingTxPowerNoPreference) {
    bt_log(INFO,
           "gap-le",
           "ignoring request: max tx power out of range: %#.4x "
           "(range: [%#.4x, %#.4x]) (current: %#.4x)",
           power,
           hci_spec::kLEAdvertisingTxPowerMin,
           hci_spec::kLEAdvertisingTxPowerMax,
           slow_adv_max_tx_power_);

    return;
  }

  bt_log(INFO, "gap-le", "setting slow max advertising tx power to %d", power);
  slow_adv_max_tx_power_ = power;
}

void LowEnergyAdvertisingManager::set_fast_adv_max_tx_power(int8_t power) {
  if ((power < hci_spec::kLEAdvertisingTxPowerMin ||
       power > hci_spec::kLEAdvertisingTxPowerMax) &&
      power != hci_spec::kLEExtendedAdvertisingTxPowerNoPreference) {
    bt_log(INFO,
           "gap-le",
           "ignoring request: max tx power out of range: %#.4x "
           "(range: [%#.4x, %#.4x]) (current: %#.4x)",
           power,
           hci_spec::kLEAdvertisingTxPowerMin,
           hci_spec::kLEAdvertisingTxPowerMax,
           fast_adv_max_tx_power_);

    return;
  }

  bt_log(INFO, "gap-le", "setting fast max advertising tx power to %d", power);
  fast_adv_max_tx_power_ = power;
}

void LowEnergyAdvertisingManager::set_very_fast_adv_max_tx_power(int8_t power) {
  if ((power < hci_spec::kLEAdvertisingTxPowerMin ||
       power > hci_spec::kLEAdvertisingTxPowerMax) &&
      power != hci_spec::kLEExtendedAdvertisingTxPowerNoPreference) {
    bt_log(INFO,
           "gap-le",
           "ignoring request: max tx power out of range: %#.4x "
           "(range: [%#.4x, %#.4x]) (current: %#.4x)",
           power,
           hci_spec::kLEAdvertisingTxPowerMin,
           hci_spec::kLEAdvertisingTxPowerMax,
           very_fast_adv_max_tx_power_);

    return;
  }

  bt_log(INFO,
         "gap-le",
         "setting very fast max advertising tx power to %d",
         power);
  very_fast_adv_max_tx_power_ = power;
}

void LowEnergyAdvertisingManager::StartAdvertising(
    AdvertisingData data,
    AdvertisingData scan_rsp,
    ConnectionCallback connect_callback,
    AdvertisingInterval interval,
    bool extended_pdu,
    bool anonymous,
    bool include_tx_power_level,
    std::optional<DeviceAddress::Type> address_type,
    AdvertisingStatusCallback status_callback) {
  // Can't be anonymous and connectable
  if (anonymous && connect_callback) {
    bt_log(DEBUG, "gap-le", "can't advertise anonymously and connectable!");
    status_callback(AdvertisementInstance(),
                    ToResult(HostError::kInvalidParameters));
    return;
  }

  // v5.1, Vol 3, Part C, Appendix A recommends the FAST1 parameters for
  // connectable advertising and FAST2 parameters for non-connectable
  // advertising. Some Bluetooth controllers reject the FAST1 parameters for
  // non-connectable advertising, hence we fall back to FAST2 in that case.
  if (interval == AdvertisingInterval::FAST1 && !connect_callback) {
    interval = AdvertisingInterval::FAST2;
  }

  hci::AdvertisingIntervalRange interval_range = slow_interval_;
  int8_t tx_power = hci_spec::kLEExtendedAdvertisingTxPowerNoPreference;
  switch (interval) {
    case AdvertisingInterval::FAST1:
      interval_range = very_fast_interval_;
      tx_power = very_fast_adv_max_tx_power_;
      break;
    case AdvertisingInterval::FAST2:
      interval_range = fast_interval_;
      tx_power = fast_adv_max_tx_power_;
      break;
    case AdvertisingInterval::SLOW:
      interval_range = slow_interval_;
      tx_power = slow_adv_max_tx_power_;
      break;
  }

  hci::LowEnergyAdvertiser::AdvertisingOptions options(
      interval_range,
      AdvFlag::kLEGeneralDiscoverableMode,
      extended_pdu,
      anonymous,
      include_tx_power_level,
      tx_power);

  auto self = weak_self_.GetWeakPtr();

  // TODO: https://fxbug.dev/42083437 - The address used for legacy advertising
  // must be coordinated via |local_addr_delegate_| however a unique address can
  // be generated and assigned to each advertising set when the controller
  // supports 5.0 extended advertising. hci::LowEnergyAdvertiser needs to be
  // revised to not use device addresses to distinguish between advertising
  // instances especially since |local_addr_delegate_| is likely to return the
  // same address if called frequently.
  //
  // Revisit this logic when multi-advertising is supported.
  local_addr_delegate_->EnsureLocalAddress(
      address_type,
      [self,
       advertising_data = std::move(data),
       scan_response = std::move(scan_rsp),
       options,
       connect_cb = std::move(connect_callback),
       status_cb = std::move(status_callback)](
          fit::result<HostError, const DeviceAddress> result) mutable {
        if (!self.is_alive()) {
          return;
        }

        if (result.is_error()) {
          status_cb(AdvertisementInstance(), fit::error(result.error_value()));
          return;
        }

        hci::LowEnergyAdvertiser::ConnectionCallback adv_conn_cb;
        if (connect_cb) {
          adv_conn_cb = [self, on_connect_cb = std::move(connect_cb)](
                            hci::AdvertisementId advertisement_id, auto link) {
            bt_log(DEBUG, "gap-le", "received new connection");

            if (!self.is_alive()) {
              return;
            }

            on_connect_cb(advertisement_id, std::move(link));
          };
        }
        auto status_cb_wrapper =
            [self, result_cb = std::move(status_cb)](
                hci::Result<hci::AdvertisementId> status) mutable {
              if (!self.is_alive()) {
                return;
              }

              if (status.is_error()) {
                result_cb(AdvertisementInstance(), status.take_error());
                return;
              }

              auto stop_advertising_cb = [self](AdvertisementId stop_id) {
                if (self.is_alive()) {
                  self->advertiser_->StopAdvertising(stop_id);
                }
              };
              result_cb(AdvertisementInstance(status.value(),
                                              std::move(stop_advertising_cb)),
                        fit::ok());
            };

        self->advertiser_->StartAdvertising(result.value(),
                                            advertising_data,
                                            scan_response,
                                            options,
                                            std::move(adv_conn_cb),
                                            std::move(status_cb_wrapper));
      });
}

}  // namespace bt::gap
