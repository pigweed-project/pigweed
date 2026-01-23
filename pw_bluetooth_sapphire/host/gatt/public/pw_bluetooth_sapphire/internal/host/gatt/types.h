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

#include "pw_bluetooth_sapphire/internal/host/att/attribute.h"
#include "pw_bluetooth_sapphire/internal/host/common/uuid.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/gatt_defs.h"

namespace bt::gatt {

// TODO(armansito): Rename this file to "local_types.h" and add the Local*
// prefix to the types here.

class Characteristic;
using CharacteristicPtr = std::unique_ptr<Characteristic>;

// Represents a single remote or local GATT service. A Service object simply
// represents the composition/structure of a GATT service, such as its type,
// characteristics, includes, etc and is not intended to carry service state.
class Service final {
 public:
  Service(bool primary, const UUID& type);
  ~Service() = default;

  bool primary() const { return primary_; }
  const UUID& type() const { return type_; }

  // The list of characteristics that have been added to this service.
  const std::vector<CharacteristicPtr>& characteristics() const {
    return characteristics_;
  }

  // Passes the ownership of this service's characteristics to the caller.
  std::vector<CharacteristicPtr> ReleaseCharacteristics() {
    return std::move(characteristics_);
  }

  // Adds the given characteristic to this service.
  void AddCharacteristic(CharacteristicPtr&& chr);

  // TODO(armansito): Support included services.

 private:
  bool primary_;
  UUID type_;
  std::vector<CharacteristicPtr> characteristics_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(Service);
};

using ServicePtr = std::unique_ptr<Service>;

class Descriptor;
using DescriptorPtr = std::unique_ptr<Descriptor>;

// Represents a single remote or local GATT characteristic. This represents the
// composition/structure of a characteristic and is not intended to carry state.
class Characteristic final {
 public:
  Characteristic(IdType id,
                 const UUID& type,
                 uint8_t properties,
                 uint16_t extended_properties,
                 const att::AccessRequirements& read_permissions,
                 const att::AccessRequirements& write_permissions,
                 const att::AccessRequirements& update_permissions);
  ~Characteristic() = default;

  IdType id() const { return id_; }
  const UUID& type() const { return type_; }
  uint8_t properties() const { return properties_; }
  uint16_t extended_properties() const { return extended_properties_; }

  const att::AccessRequirements& read_permissions() const {
    return read_permissions_;
  }

  const att::AccessRequirements& write_permissions() const {
    return write_permissions_;
  }

  const att::AccessRequirements& update_permissions() const {
    return update_permissions_;
  }

  const std::vector<DescriptorPtr>& descriptors() const { return descriptors_; }

  // Passes the ownership of this characteristic's descriptors to the caller.
  std::vector<DescriptorPtr> ReleaseDescriptors() {
    return std::move(descriptors_);
  }

  void AddDescriptor(DescriptorPtr&& desc);

 private:
  IdType id_;
  UUID type_;
  uint8_t properties_;
  uint16_t extended_properties_;
  att::AccessRequirements read_permissions_;
  att::AccessRequirements write_permissions_;
  att::AccessRequirements update_permissions_;
  std::vector<DescriptorPtr> descriptors_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(Characteristic);
};

// Represents a single remote or local GATT characteristic descriptor. This
// represents the composition/structure of a characteristic and is not intended
// to carry state.
class Descriptor final {
 public:
  Descriptor(IdType id,
             const UUID& type,
             const att::AccessRequirements& read_permissions,
             const att::AccessRequirements& write_permissions);
  ~Descriptor() = default;

  IdType id() const { return id_; }
  const UUID& type() const { return type_; }

  const att::AccessRequirements& read_permissions() const {
    return read_permissions_;
  }

  const att::AccessRequirements& write_permissions() const {
    return write_permissions_;
  }

 private:
  IdType id_;
  UUID type_;
  att::AccessRequirements read_permissions_;
  att::AccessRequirements write_permissions_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(Descriptor);
};

// Define after the class definitions since deleting incomplete types is not
// permitted in C++23.
inline void Service::AddCharacteristic(CharacteristicPtr&& chr) {
  characteristics_.push_back(std::forward<CharacteristicPtr>(chr));
}

inline void Characteristic::AddDescriptor(DescriptorPtr&& desc) {
  descriptors_.push_back(std::forward<DescriptorPtr>(desc));
}

}  // namespace bt::gatt
