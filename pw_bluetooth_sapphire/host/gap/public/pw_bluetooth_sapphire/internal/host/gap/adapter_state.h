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

#pragma once
#include <cstdint>

#include "pw_bluetooth/controller.h"
#include "pw_bluetooth_sapphire/internal/host/common/device_address.h"
#include "pw_bluetooth_sapphire/internal/host/gap/android_vendor_capabilities.h"
#include "pw_bluetooth_sapphire/internal/host/gap/gap.h"
#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_state.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/lmp_feature_set.h"
#include "pw_bluetooth_sapphire/internal/host/transport/acl_data_channel.h"

namespace bt::gap {

// The member variables in this class consist of controller settings that are
// shared between LE and BR/EDR controllers. LE and BR/EDR specific state is
// stored in corresponding data structures.
struct AdapterState final {
  TechnologyType type() const {
    // Note: we don't support BR/EDR only controllers.
    if (IsBREDRSupported()) {
      return TechnologyType::kDualMode;
    }
    return TechnologyType::kLowEnergy;
  }

  // Returns true if the indicated feature is supported by Controller.
  bool IsControllerFeatureSupported(
      pw::bluetooth::Controller::FeaturesBits feature) const {
    return feature & controller_features;
  }

  // Helpers for querying LMP capabilities.
  inline bool IsBREDRSupported() const {
    return !features.HasBit(/*page=*/0u,
                            hci_spec::LMPFeature::kBREDRNotSupported);
  }

  inline bool IsLowEnergySupported() const {
    return features.HasBit(/*page=*/0u, hci_spec::LMPFeature::kLESupportedHost);
  }

  inline bool IsLocalSecureConnectionsSupported() const {
    return features.HasBit(
               /*page=*/1u,
               hci_spec::LMPFeature::kSecureConnectionsHostSupport) &&
           features.HasBit(
               /*page=*/2u,
               hci_spec::LMPFeature::kSecureConnectionsControllerSupport);
  }

  inline bool IsSecureConnectionHostSupportSupported() const {
    return features.HasBit(/*page=*/1,
                           hci_spec::LMPFeature::kSecureConnectionsHostSupport);
  }

  inline auto SupportedCommands() const {
    return pw::bluetooth::emboss::MakeSupportedCommandsView(
        supported_commands, sizeof(supported_commands));
  }

  inline bool IsControllerRemotePublicKeyValidationSupported() const {
    // We don't actually need to send the command:  "Note: If this command is
    // supported, then the Controller must support remote public key validation"
    // (v6.0, Vol 4, Part E, Sec. 7.4.9).
    return SupportedCommands().read_local_simple_pairing_options().Read();
  }

  // HCI version supported by the controller.
  pw::bluetooth::emboss::CoreSpecificationVersion hci_version;

  // The Features that are supported by this adapter.
  hci_spec::LMPFeatureSet features;

  // Features reported by Controller.
  pw::bluetooth::Controller::FeaturesBits controller_features{0};

  // Bitmask list of HCI commands that the controller supports.
  uint8_t supported_commands[64] = {0};

  // This returns Bluetooth Controller address. This address has the following
  // meaning based on the controller capabilities:
  //   - On BR/EDR this is the Bluetooth Controller Address, or BD_ADDR.
  //   - On LE this is the Public Device Address. This value can be used as the
  //     device's identity address. This value can be zero if a Public Device
  //     Address is not used.
  //   - On BR/EDR/LE this is the LE Public Device Address AND the BD_ADDR.
  DeviceAddressBytes controller_address;

  // The BR/EDR ACL data buffer size. We store this here as it is needed on
  // dual-mode controllers even if the host stack is compiled for LE-only.
  hci::DataBufferInfo bredr_data_buffer_info;

  // The SCO buffer size.
  hci::DataBufferInfo sco_buffer_info;

  // BLE-specific state.
  LowEnergyState low_energy_state;

  // Android vendor extensions capabilities
  // NOTE: callers should separately check that the controller actually supports
  // android vendor extensions first.
  std::optional<AndroidVendorCapabilities> android_vendor_capabilities;

  // True if Android LE Batch Scan was successfully enabled during
  // initialization.
  bool android_batch_scan_enabled = false;

  // Local name
  std::string local_name;
};

}  // namespace bt::gap
