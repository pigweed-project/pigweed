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

#include "pw_bluetooth_sapphire/internal/host/gap/android_vendor_capabilities.h"

#include <stdexcept>

#include "public/pw_bluetooth_sapphire/internal/host/gap/android_vendor_capabilities.h"
#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/transport/control_packets.h"

namespace bt::gap {

namespace android_emb = pw::bluetooth::vendor::android_hci;

namespace {
bool AsBool(const android_emb::Capability& capability) {
  return capability == android_emb::Capability::CAPABLE;
}
}  // namespace

bool AndroidVendorCapabilities::SupportsVersion(uint8_t major,
                                                uint8_t minor) const {
  if (version_major_ > major) {
    return true;
  }

  if (version_major_ == major && version_minor_ >= minor) {
    return true;
  }

  return false;
}

bool AndroidVendorCapabilities::IsVersion(uint8_t major, uint8_t minor) const {
  return version_major_ == major && version_minor_ == minor;
}

AndroidVendorCapabilities AndroidVendorCapabilities::New(
    const android_emb::LEGetVendorCapabilitiesCommandCompleteEventView& orig_e,
    const uint16_t override_vendor_capabilites_version) {
  auto mutable_event = hci::EventPacket::New<
      android_emb::LEGetVendorCapabilitiesCommandCompleteEventWriter>(
      hci_spec::kCommandCompleteEventCode,
      orig_e.BackingStorage().SizeInBytes());
  mutable_event.mutable_data().Write(
      orig_e.BackingStorage().data(), orig_e.BackingStorage().SizeInBytes(), 0);
  if (override_vendor_capabilites_version != 0) {
    uint8_t major_override = override_vendor_capabilites_version & 0xFF;
    uint8_t minor_override = override_vendor_capabilites_version >> 8;
    bt_log(INFO,
           "android_vendor_extensions",
           "Override Vendor Version (%d.%d) -> 0x%04X (%d.%d)",
           orig_e.version_supported().major_number().Read(),
           orig_e.version_supported().minor_number().Read(),
           override_vendor_capabilites_version,
           major_override,
           minor_override);
    // TODO(fxbug.dev/450278813): Remove these manual overrides
    mutable_event.unchecked_view_t().version_supported().major_number().Write(
        major_override);
    mutable_event.unchecked_view_t().version_supported().minor_number().Write(
        minor_override);
  }
  auto e = mutable_event.unchecked_view_t();

  AndroidVendorCapabilities c;

  if (e.status().Read() != pw::bluetooth::emboss::StatusCode::SUCCESS) {
    bt_log(INFO,
           "android_vendor_extensions",
           "refusing to parse non-success vendor capabilities");
    return c;
  }

  // Version 0.55
  c.max_simultaneous_advertisement_ = e.max_advt_instances().Read();
  c.supports_offloaded_rpa_ =
      AsBool(e.offloaded_resolution_of_private_address().Read());
  c.scan_results_storage_bytes_ = e.total_scan_results_storage().Read();
  c.irk_list_size_ = e.max_irk_list_sz().Read();
  c.supports_filtering_ = AsBool(e.filtering_support().Read());
  c.max_filters_ = e.max_filter().Read();
  c.supports_activity_energy_info_ =
      AsBool(e.activity_energy_info_support().Read());

  // There can be various versions of this command complete event sent by the
  // Controller. As fields are added, the version_supported field is incremented
  // to signify which fields are available. However, version_supported was only
  // introduced into the command in Version 0.95. Controllers that use the base
  // version, Version 0.55, don't have the version_supported field. As such, we
  // must jump through some hoops to figure out which version we received,
  // exactly.
  //
  // NOTE: Android's definition for this command complete event is available in
  // AOSP: LeGetVendorCapabilitiesComplete and friends
  // https://cs.android.com/android/platform/superproject/+/main:packages/modules/Bluetooth/system/gd/hci/hci_packets.pdl
  //
  // NOTE: An example implementation of how this command is filled in by a
  // Controller is available within AOSP:
  // le_get_vendor_capabilities_handler(...)
  // https://cs.android.com/android/platform/superproject/main/+/main:packages/modules/Bluetooth/system/gd/hci/controller.cc
  if (e.version_supported().major_number().IsComplete()) {
    c.version_major_ = e.version_supported().major_number().Read();
  }

  if (e.version_supported().minor_number().IsComplete()) {
    c.version_minor_ = e.version_supported().minor_number().Read();
  }
  bt_log(INFO,
         "android_vendor_extensions",
         "Vendor Version: %d.%d",
         c.version_major_,
         c.version_minor_);

  // If we didn't receive a version number from the Controller, assume it's the
  // base version, Version 0.55.
  if (c.version_major_ == 0 && c.version_minor_ == 0) {
    c.version_minor_ = 55;
  }

  // Version 0.95
  if (c.SupportsVersion(0, 95)) {
    c.max_advertisers_tracked_ = e.total_num_of_advt_tracked().Read();
    c.supports_extended_scan_ = AsBool(e.extended_scan_support().Read());
    c.supports_debug_logging_ = AsBool(e.debug_logging_supported().Read());
  }

  // Version 0.96 and beyond supports this, but version 0.99 it is absent.
  if (c.SupportsVersion(0, 96) && !c.IsVersion(0, 99)) {
    c.supports_offloading_le_address_generation_ =
        AsBool(e.le_address_generation_offloading_support().Read());
  }

  // Version 0.98
  if (c.SupportsVersion(0, 98) && !c.IsVersion(0, 99)) {
    c.a2dp_source_offload_capability_mask_ =
        e.a2dp_source_offload_capability_mask().BackingStorage().ReadUInt();
    c.supports_bluetooth_quality_report_ =
        AsBool(e.bluetooth_quality_report_support().Read());
    bt_log(INFO,
           "android_vendor_extensions",
           "Adding A2DP vendor extensions v1: 0x%02X",
           c.a2dp_source_offload_capability_mask_);
  } else if (c.IsVersion(0, 99)) {
    c.a2dp_source_offload_capability_mask_ =
        e.v99_a2dp_source_offload_capability_mask().BackingStorage().ReadUInt();
    bt_log(INFO,
           "android_vendor_extensions",
           "Adding A2DP vendor extensions v0.99: 0x%02X",
           c.a2dp_source_offload_capability_mask_);
    // 0.99 doesn't have the Supports Bluetooth Quality Report
  }

  // Version 1.03
  if (c.SupportsVersion(1, 03)) {
    c.supports_dynamic_audio_buffer_ =
        e.dynamic_audio_buffer_support().BackingStorage().ReadUInt();
  }

  // Version 1.04
  if (c.SupportsVersion(1, 04)) {
    c.a2dp_offload_v2_support_ = AsBool(e.a2dp_offload_v2_support().Read());
  }

  // Version 1.05
  if (c.SupportsVersion(1, 05)) {
    c.iso_link_feedback_support_ = AsBool(e.iso_link_feedback_support().Read());
    c.sniff_offload_support_ = AsBool(e.sniff_offload_support().Read());
  }

  return c;
}

}  // namespace bt::gap
