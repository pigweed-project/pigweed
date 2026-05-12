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

#include "pw_bluetooth_sapphire/internal/host/hci/discovery_filter.h"

#include <pw_assert/check.h>
#include <pw_bytes/endian.h>

#include "cpp-string/string_printf.h"
#include "pw_bluetooth_sapphire/internal/host/common/advertising_data.h"
#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"

namespace bt::hci {

void DiscoveryFilter::SetGeneralDiscoveryFlags() {
  set_flags(static_cast<uint8_t>(AdvFlag::kLEGeneralDiscoverableMode) |
            static_cast<uint8_t>(AdvFlag::kLELimitedDiscoverableMode));
}

bool DiscoveryFilter::Matches(
    const std::optional<std::reference_wrapper<const AdvertisingData>>
        advertising_data,
    bool connectable,
    int8_t rssi) const {
  // No need to check |advertising_data| for the |connectable_| filter.
  if (connectable_ && *connectable_ != connectable) {
    return false;
  }

  // If a pathloss filter is not set then apply the RSSI filter before
  // checking |advertising_data|. (An RSSI value of hci_spec::kRSSIInvalid means
  // that RSSI is not available, which we check for here).
  bool rssi_ok = !rssi_ || (rssi != hci_spec::kRSSIInvalid && rssi >= *rssi_);
  if (!pathloss_ && !rssi_ok) {
    return false;
  }

  // Any of these filters being set requires us to have a valid
  // |advertising_data| to pass.
  bool needs_ad_check = flags_ || !service_uuids_.empty() ||
                        !service_data_uuids_.empty() ||
                        !solicitation_uuids_.empty() ||
                        !name_substring_.empty() || manufacturer_code_;

  if (!advertising_data.has_value() && needs_ad_check) {
    return false;
  }

  // Pathloss is complicated because we can pass if it's set and we have no
  // |advertising_data| by passing RSSI instead.
  if (pathloss_) {
    if (!advertising_data.has_value() ||
        !advertising_data->get().tx_power().has_value()) {
      // If no RSSI filter was set OR if one was set but it didn't match the
      // scan result, we fail.
      if (!rssi_ || !rssi_ok) {
        return false;
      }
      // Otherwise we fall back to RSSI passing if tx_power was not set.
    } else {
      int8_t tx_power_lvl = *advertising_data->get().tx_power();
      if (tx_power_lvl < rssi) {
        bt_log(WARN,
               "gap",
               "reported tx-power level is less than RSSI, failed pathloss");
        return false;
      }
      int8_t pathloss = tx_power_lvl - rssi;
      if (pathloss > *pathloss_) {
        return false;
      }
      // mark the rssi_ok since we pass based on pathloss.
      rssi_ok = true;
    }
  }

  // If we made it here without advetising_data, and there's no need to check,
  // we pass if rssi passed (which also passes if RSSI filtering was not set)
  if (!advertising_data.has_value() && !needs_ad_check) {
    return rssi_ok;
  }

  PW_DCHECK(advertising_data.has_value());
  const AdvertisingData& ad = advertising_data->get();

  if (flags_) {
    if (all_flags_required_ && ad.flags() != flags_) {
      return false;
    }
    if (!ad.flags().has_value()) {
      return false;
    }
    uint8_t matched_flags = ad.flags().value() & *flags_;
    if (matched_flags == 0) {
      return false;
    }
  }

  if (!name_substring_.empty()) {
    if (!ad.local_name()) {
      return false;
    }
    // TODO(jamuraa): If this is an incomplete name should we match the first
    // part?
    if (ad.local_name()->name.find(name_substring_) == std::string_view::npos) {
      return false;
    }
  }

  if (manufacturer_code_) {
    if (ad.manufacturer_data_ids().count(*manufacturer_code_) == 0) {
      return false;
    }
  }

  if (!service_uuids_.empty()) {
    bool service_found = false;
    const auto& ad_service_uuids = ad.service_uuids();
    for (auto uuid : service_uuids_) {
      if (ad_service_uuids.count(uuid) != 0) {
        service_found = true;
        break;
      }
    }
    if (!service_found) {
      return false;
    }
  }

  if (!service_data_uuids_.empty()) {
    bool service_data_found = false;
    const auto& ad_data_uuids = ad.service_data_uuids();
    for (auto uuid : service_data_uuids_) {
      if (ad_data_uuids.count(uuid) != 0) {
        service_data_found = true;
        break;
      }
    }
    if (!service_data_found) {
      return false;
    }
  }

  if (!solicitation_uuids_.empty()) {
    bool solicitation_uuid_found = false;
    const auto& ad_solicitation_uuids = ad.solicitation_uuids();
    for (auto uuid : solicitation_uuids_) {
      if (ad_solicitation_uuids.count(uuid) != 0) {
        solicitation_uuid_found = true;
        break;
      }
    }
    if (!solicitation_uuid_found) {
      return false;
    }
  }

  // We haven't filtered it out, so it matches.
  return true;
}

void DiscoveryFilter::Reset() {
  service_uuids_.clear();
  service_data_uuids_.clear();
  solicitation_uuids_.clear();
  name_substring_.clear();
  connectable_.reset();
  manufacturer_code_.reset();
  pathloss_.reset();
  rssi_.reset();
}

static std::string UuidCsv(const std::vector<UUID>& uuids) {
  if (uuids.empty()) {
    return "unset";
  }

  bool added = false;
  std::string result;
  for (const UUID& uuid : uuids) {
    bt_lib_cpp_string::StringAppendf(
        &result, "%s%s", added ? ", " : "", uuid.ToString().c_str());
    added = true;
  }

  return result;
}

static std::string BoolAlpha(bool value) {
  if (value) {
    return "true";
  }

  return "false";
}

bool DiscoveryFilter::operator==(const DiscoveryFilter& other) const {
  if (flags_ != other.flags_) {
    return false;
  }

  if (all_flags_required_ != other.all_flags_required_) {
    return false;
  }

  if (service_uuids_ != other.service_uuids_) {
    return false;
  }

  if (service_data_uuids_ != other.service_data_uuids_) {
    return false;
  }

  if (solicitation_uuids_ != other.solicitation_uuids_) {
    return false;
  }

  if (name_substring_ != other.name_substring_) {
    return false;
  }

  if (connectable_ != other.connectable_) {
    return false;
  }

  if (manufacturer_code_ != other.manufacturer_code_) {
    return false;
  }

  if (pathloss_ != other.pathloss_) {
    return false;
  }

  if (rssi_ != other.rssi_) {
    return false;
  }

  return true;
}

std::string DiscoveryFilter::ToString() const {
  std::string result;

  bt_lib_cpp_string::StringAppendf(
      &result, "flags: {0x%02hhx}, ", flags_.has_value() ? flags_.value() : 0);

  bt_lib_cpp_string::StringAppendf(&result,
                                   "all flags required: {%s}, ",
                                   all_flags_required_ ? "true" : "false");

  bt_lib_cpp_string::StringAppendf(
      &result, "service uuids: {%s}, ", UuidCsv(service_uuids_).c_str());

  bt_lib_cpp_string::StringAppendf(&result,
                                   "service data uuids: {%s}, ",
                                   UuidCsv(service_data_uuids_).c_str());

  bt_lib_cpp_string::StringAppendf(&result,
                                   "solicitation uuids: {%s}, ",
                                   UuidCsv(solicitation_uuids_).c_str());

  bt_lib_cpp_string::StringAppendf(
      &result,
      "name substring: {%s}, ",
      name_substring_.empty() ? "unset" : name_substring_.c_str());

  bt_lib_cpp_string::StringAppendf(&result,
                                   "connectable: {%s}, ",
                                   connectable_.has_value()
                                       ? BoolAlpha(connectable_.value()).c_str()
                                       : "unset");

  bt_lib_cpp_string::StringAppendf(
      &result,
      "manufacturer code: {0x%02hhx}, ",
      manufacturer_code_.has_value() ? manufacturer_code_.has_value() : 0);

  bt_lib_cpp_string::StringAppendf(
      &result,
      "pathloss: {0x%02hhx}, ",
      pathloss_.has_value() ? pathloss_.has_value() : 0);

  bt_lib_cpp_string::StringAppendf(
      &result, "rssi: {0x%02hhx}", rssi_.has_value() ? rssi_.has_value() : 0);

  return result;
}

}  // namespace bt::hci
