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
#include <vector>

#include "pw_bluetooth_sapphire/internal/host/common/device_class.h"
#include "pw_bluetooth_sapphire/internal/host/common/uuid.h"
#include "pw_bluetooth_sapphire/internal/host/gap/peer.h"
#include "pw_bluetooth_sapphire/internal/host/sm/types.h"

namespace bt::gap {

// A |BondingData| struct can be passed to the peer cache and allows for
// flexibility in adding new fields to cache.
struct BondingData {
  PeerId identifier;
  DeviceAddress address;
  std::optional<std::string> name;

  // Class of device of the peer, if known.
  std::optional<DeviceClass> device_class;

  // TODO(fxbug.dev/42102158): This should be optional to represent whether the
  // FIDL bonding data has LE data, instead of using DeviceAddresss's type()
  sm::PairingData le_pairing_data;
  std::optional<sm::LTK> bredr_link_key;
  std::vector<UUID> bredr_services;
};

}  // namespace bt::gap
