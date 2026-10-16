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

#include "pw_bluetooth_sapphire/internal/host/common/device_class.h"

#include <pw_assert/check.h>

#include <algorithm>
#include <climits>

namespace bt {

namespace {

DeviceClass::ServiceClass bit_no_to_service_class(uint8_t bit_no) {
  PW_DCHECK(bit_no >= 13);
  PW_DCHECK(bit_no < 24);
  switch (bit_no) {
    case 13:
      return DeviceClass::ServiceClass::kLimitedDiscoverableMode;
    case 14:
      return DeviceClass::ServiceClass::kLEAudio;
    case 15:
      return DeviceClass::ServiceClass::kReserved;
    case 16:
      return DeviceClass::ServiceClass::kPositioning;
    case 17:
      return DeviceClass::ServiceClass::kNetworking;
    case 18:
      return DeviceClass::ServiceClass::kRendering;
    case 19:
      return DeviceClass::ServiceClass::kCapturing;
    case 20:
      return DeviceClass::ServiceClass::kObjectTransfer;
    case 21:
      return DeviceClass::ServiceClass::kAudio;
    case 22:
      return DeviceClass::ServiceClass::kTelephony;
    case 23:
      return DeviceClass::ServiceClass::kInformation;
  };
  // Should be unreachable.
  return DeviceClass::ServiceClass::kInformation;
}

std::string service_class_to_string(const DeviceClass::ServiceClass& serv) {
  switch (serv) {
    case DeviceClass::ServiceClass::kLimitedDiscoverableMode:
      return "Limited Discoverable Mode";
    case DeviceClass::ServiceClass::kLEAudio:
      return "LE Audio";
    case DeviceClass::ServiceClass::kReserved:
      return "Reserved";
    case DeviceClass::ServiceClass::kPositioning:
      return "Positioning";
    case DeviceClass::ServiceClass::kNetworking:
      return "Networking";
    case DeviceClass::ServiceClass::kRendering:
      return "Rendering";
    case DeviceClass::ServiceClass::kCapturing:
      return "Capturing";
    case DeviceClass::ServiceClass::kObjectTransfer:
      return "Object Transfer";
    case DeviceClass::ServiceClass::kAudio:
      return "Audio";
    case DeviceClass::ServiceClass::kTelephony:
      return "Telephony";
    case DeviceClass::ServiceClass::kInformation:
      return "Information";
  }
}

}  // namespace

DeviceClass::DeviceClass() : DeviceClass(MajorClass::kUnspecified) {}

DeviceClass::DeviceClass(MajorClass major_class)
    : bytes_{0x00, static_cast<uint8_t>(major_class), 0x00} {}

DeviceClass::DeviceClass(std::initializer_list<uint8_t> bytes) {
  PW_DCHECK(bytes.size() == bytes_.size());
  std::copy(bytes.begin(), bytes.end(), bytes_.begin());
}

DeviceClass::DeviceClass(uint32_t value) {
  PW_DCHECK(value < 1 << 24);  // field should only populate 24 bits
  bytes_ = {
      static_cast<uint8_t>((value >> 0) & 0xFF),
      static_cast<uint8_t>((value >> 8) & 0xFF),
      static_cast<uint8_t>((value >> 16) & 0xFF),
  };
}

uint32_t DeviceClass::to_int() const {
  uint32_t out = 0;
  out |= bytes_[0];
  out |= static_cast<uint32_t>(bytes_[1]) << CHAR_BIT;
  out |= static_cast<uint32_t>(bytes_[2]) << (2 * CHAR_BIT);
  return out;
}

void DeviceClass::SetServiceClasses(
    const std::unordered_set<ServiceClass>& classes) {
  for (const auto& c : classes) {
    uint8_t bit = static_cast<uint8_t>(c);
    if (bit >= 16) {
      bytes_[2] |= 0x01 << (bit - 16);
    } else if (bit >= 8) {
      bytes_[1] |= 0x01 << (bit - 8);
    }
  }
}

std::unordered_set<DeviceClass::ServiceClass> DeviceClass::GetServiceClasses()
    const {
  std::unordered_set<ServiceClass> classes;
  for (uint8_t bit_no = 13; bit_no < 16; bit_no++) {
    if (bytes_[1] & (0x01 << (bit_no - 8))) {
      classes.emplace(bit_no_to_service_class(bit_no));
    }
  }
  for (uint8_t bit_no = 16; bit_no < 24; bit_no++) {
    if (bytes_[2] & (0x01 << (bit_no - 16))) {
      classes.emplace(bit_no_to_service_class(bit_no));
    }
  }
  return classes;
}

std::string DeviceClass::ToString() const {
  std::string service_desc;
  auto classes = GetServiceClasses();
  if (!classes.empty()) {
    auto it = classes.begin();
    service_desc = " (" + service_class_to_string(*it);
    ++it;
    for (; it != classes.end(); ++it) {
      service_desc += ", " + service_class_to_string(*it);
    }
    service_desc = service_desc + ")";
  }
  switch (major_class()) {
    case MajorClass::kMiscellaneous:
      return "Miscellaneous" + service_desc;
    case MajorClass::kComputer:
      return "Computer" + service_desc;
    case MajorClass::kPhone:
      return "Phone" + service_desc;
    case MajorClass::kLAN:
      return "LAN" + service_desc;
    case MajorClass::kAudioVideo:
      return "A/V" + service_desc;
    case MajorClass::kPeripheral:
      return "Peripheral" + service_desc;
    case MajorClass::kImaging:
      return "Imaging" + service_desc;
    case MajorClass::kWearable:
      return "Wearable" + service_desc;
    case MajorClass::kToy:
      return "Toy" + service_desc;
    case MajorClass::kHealth:
      return "Health Device" + service_desc;
    case MajorClass::kUnspecified:
      return "Unspecified" + service_desc;
  };

  return "(unknown)";
}

}  // namespace bt
