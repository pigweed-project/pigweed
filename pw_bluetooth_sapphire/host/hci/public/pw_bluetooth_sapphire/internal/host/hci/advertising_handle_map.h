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

#include "pw_bluetooth_sapphire/internal/host/common/identifier.h"
#include "pw_bluetooth_sapphire/internal/host/common/inspect.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"

namespace bt::hci {

using AdvertisementId = bt::AdvertisementId;

// This class allocates AdvertisingHandles and AdvertisementIds and provides
// mappings between them.
//
// Extended advertising HCI commands refer to an advertising set via an
// AdvertisingHandle. An AdvertisingHandle is an eight bit unsigned integer that
// uniquely identifies an advertising set. We also use a unique AdvertisingId to
// identify a client's advertising request. We sometimes need to retrieve the
// local address of an advertising request to create advertising commands. Thus,
// we frequently need to convert between an AdvertisingHandle, AdvertisementId,
// and DeviceAddress.
class AdvertisingHandleMap {
 public:
  // Instantiate an AdvertisingHandleMap. The capacity parameter specifies the
  // maximum number of mappings that this instance will support. Setting the
  // capacity also restricts the range of advertising handles
  // AdvertisingHandleMap will return: [1, capacity).
  explicit AdvertisingHandleMap(
      uint8_t capacity = hci_spec::kMaxAdvertisingHandle);

  // Allocate an AdvertisingHandle and a unique AdvertisementId and map them to
  // |address|. The insertion may fail if there are already
  // hci_spec::kMaxAdvertisingHandles in the container.
  std::optional<AdvertisementId> Insert(const DeviceAddress& address);

  // Get the AdvertisingHandle corresponding to an AdvertisementId. |id| MUST
  // exist in the map, or else this function will panic.
  hci_spec::AdvertisingHandle GetHandle(AdvertisementId id) const;

  // Get the DeviceAddress corresponding to an AdvertisementId. |id| MUST
  // exist in the map, or else this function will panic.
  DeviceAddress GetAddress(AdvertisementId id) const;

  // Get the AdvertisementId of the corresponding |handle|. Returns nullopt if
  // no advertisement using |handle| exists.
  std::optional<AdvertisementId> GetId(
      hci_spec::AdvertisingHandle handle) const;

  // Get the maximum number of mappings the AdvertisingHandleMap will support.
  uint8_t capacity() const { return capacity_; }

  // Retrieve the advertising handle that was most recently generated. This
  // function is primarily used by unit tests so as to avoid hardcoding values
  // or making assumptions about the starting point or ordering of advertising
  // handle generation.
  std::optional<hci_spec::AdvertisingHandle> LastUsedHandleForTesting() const;

  // Get the number of unique mappings in the container
  std::size_t Size() const { return map_.size(); }

  // Return true if the container has no mappings, false otherwise
  bool Empty() const { return map_.empty(); }

  // Remove all mappings in the container
  void Clear() {
    map_.clear();
    handle_to_id_.clear();
  }

  void Erase(AdvertisementId id);

  void AttachInspect(inspect::Node& parent);

 private:
  struct Value {
    hci_spec::AdvertisingHandle handle;
    DeviceAddress address;
    inspect::Node node;
  };

  inspect::Node node_;

  // Tracks the maximum number of elements that can be stored in this container.
  //
  // NOTE: AdvertisingHandles have a range of [1, capacity_).
  uint8_t capacity_;

  // Generate the next valid, available, and within range AdvertisingHandle.
  // This function may fail if there are already
  // hci_spec::kMaxAdvertisingHandles in the container: there are no more valid
  // AdvertisingHandles.
  std::optional<hci_spec::AdvertisingHandle> NextHandle();

  // The last generated advertising handle used as a hint to generate the next
  // handle.
  hci_spec::AdvertisingHandle last_handle_ =
      hci_spec::kMinAdvertisingHandle - 1;

  AdvertisementId::value_t next_advertisement_id_ = 1;

  std::unordered_map<AdvertisementId, Value> map_;
  std::unordered_map<hci_spec::AdvertisingHandle, AdvertisementId>
      handle_to_id_;
};

}  // namespace bt::hci
