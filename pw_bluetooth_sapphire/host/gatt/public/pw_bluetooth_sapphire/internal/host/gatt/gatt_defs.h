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
#include <lib/fit/result.h>

#include "pw_bluetooth_sapphire/internal/host/att/att.h"
#include "pw_bluetooth_sapphire/internal/host/common/identifier.h"
#include "pw_bluetooth_sapphire/internal/host/common/uuid.h"

namespace bt::gatt {

// 16-bit Attribute Types defined by the GATT profile (Vol 3, Part G, 3.4).
namespace types {

inline constexpr uint16_t kPrimaryService16 = 0x2800;
inline constexpr uint16_t kSecondaryService16 = 0x2801;
inline constexpr uint16_t kIncludeDeclaration16 = 0x2802;
inline constexpr uint16_t kCharacteristicDeclaration16 = 0x2803;
inline constexpr uint16_t kCharacteristicExtProperties16 = 0x2900;
inline constexpr uint16_t kCharacteristicUserDescription16 = 0x2901;
inline constexpr uint16_t kClientCharacteristicConfig16 = 0x2902;
inline constexpr uint16_t kServerCharacteristicConfig16 = 0x2903;
inline constexpr uint16_t kCharacteristicFormat16 = 0x2904;
inline constexpr uint16_t kCharacteristicAggregateFormat16 = 0x2905;
inline constexpr uint16_t kGenericAttributeService16 = 0x1801;
inline constexpr uint16_t kServiceChangedCharacteristic16 = 0x2a05;
inline constexpr uint16_t kServerSupportedFeaturesCharacteristic16 = 0x2b3a;

inline constexpr UUID kPrimaryService(kPrimaryService16);
inline constexpr UUID kSecondaryService(kSecondaryService16);
inline constexpr UUID kIncludeDeclaration(kIncludeDeclaration16);
inline constexpr UUID kCharacteristicDeclaration(kCharacteristicDeclaration16);
inline constexpr UUID kCharacteristicExtProperties(
    kCharacteristicExtProperties16);
inline constexpr UUID kCharacteristicUserDescription(
    kCharacteristicUserDescription16);
inline constexpr UUID kClientCharacteristicConfig(
    kClientCharacteristicConfig16);
inline constexpr UUID kServerCharacteristicConfig(
    kServerCharacteristicConfig16);
inline constexpr UUID kCharacteristicFormat(kCharacteristicFormat16);
inline constexpr UUID kCharacteristicAggregateFormat(
    kCharacteristicAggregateFormat16);

// Defined Generic Attribute Profile Service (Vol 3, Part G, 7)
inline constexpr bt::UUID kGenericAttributeService(kGenericAttributeService16);
constexpr bt::UUID kServiceChangedCharacteristic(
    kServiceChangedCharacteristic16);
constexpr UUID kServerSupportedFeaturesCharacteristic(
    kServerSupportedFeaturesCharacteristic16);

}  // namespace types

// Represents the reliability mode during long and prepared write operations.
enum ReliableMode {
  kDisabled = 0x01,
  kEnabled = 0x02,
};

// Possible values that can be used in a "Characteristic Properties" bitfield.
// (see Vol 3, Part G, 3.3.1.1)
enum Property : uint8_t {
  kBroadcast = 0x01,
  kRead = 0x02,
  kWriteWithoutResponse = 0x04,
  kWrite = 0x08,
  kNotify = 0x10,
  kIndicate = 0x20,
  kAuthenticatedSignedWrites = 0x40,
  kExtendedProperties = 0x80,
};
using Properties = uint8_t;

// Values for "Characteristic Extended Properties" bitfield.
// (see Vol 3, Part G, 3.3.3.1)
enum ExtendedProperty : uint16_t {
  kReliableWrite = 0x0001,
  kWritableAuxiliaries = 0x0002,
};
using ExtendedProperties = uint16_t;

// Values for the "Client Characteristic Configuration" descriptor.
inline constexpr uint16_t kCCCNotificationBit = 0x0001;
inline constexpr uint16_t kCCCIndicationBit = 0x0002;

using PeerId = PeerId;

// An identity for a Characteristic within a RemoteService
// Characteristic IDs are guaranteed to equal the Value Handle for the
// characteristic
struct CharacteristicHandle {
  constexpr explicit CharacteristicHandle(att::Handle handle) : value(handle) {}
  CharacteristicHandle() = delete;
  CharacteristicHandle(const CharacteristicHandle& other) = default;

  CharacteristicHandle& operator=(const CharacteristicHandle& other) = default;

  inline bool operator<(const CharacteristicHandle& rhs) const {
    return this->value < rhs.value;
  }
  inline bool operator==(const CharacteristicHandle& rhs) const {
    return this->value == rhs.value;
  }

  att::Handle value;
};

// Descriptors are identified by their underlying ATT handle
struct DescriptorHandle {
  DescriptorHandle(att::Handle handle) : value(handle) {}
  DescriptorHandle() = delete;
  DescriptorHandle(const DescriptorHandle& other) = default;

  DescriptorHandle& operator=(const DescriptorHandle& other) = default;

  inline bool operator<(const DescriptorHandle& rhs) const {
    return this->value < rhs.value;
  }
  inline bool operator==(const DescriptorHandle& rhs) const {
    return this->value == rhs.value;
  }

  att::Handle value;
};

// An identifier uniquely identifies a local GATT service, characteristic, or
// descriptor.
using IdType = uint64_t;

// 0 is reserved as an invalid ID.
inline constexpr IdType kInvalidId = 0u;

// Types representing GATT discovery results.

enum class ServiceKind {
  PRIMARY,
  SECONDARY,
};

struct ServiceData {
  ServiceData() = default;
  ServiceData(ServiceKind kind,
              att::Handle start,
              att::Handle end,
              const UUID& type);

  ServiceKind kind;
  att::Handle range_start;
  att::Handle range_end;
  UUID type;

  // NOTE: In C++20 this can be generated via `= default` assignment.
  bool operator==(const ServiceData& other) const {
    return kind == other.kind && range_start == other.range_start &&
           range_end == other.range_end && type == other.type;
  }
};

// An immutable definition of a GATT Characteristic
struct CharacteristicData {
  CharacteristicData() = delete;
  CharacteristicData(Properties props,
                     std::optional<ExtendedProperties> ext_props,
                     att::Handle handle,
                     att::Handle value_handle,
                     const UUID& type);

  Properties properties;
  std::optional<ExtendedProperties> extended_properties;
  att::Handle handle;
  att::Handle value_handle;
  UUID type;

  // NOTE: In C++20 this can be generated via `= default` assignment.
  bool operator==(const CharacteristicData& other) const {
    return properties == other.properties &&
           extended_properties == other.extended_properties &&
           handle == other.handle && value_handle == other.value_handle &&
           type == other.type;
  }
};

// An immutable definition of a GATT Descriptor
struct DescriptorData {
  DescriptorData() = delete;
  DescriptorData(att::Handle handle, const UUID& type);

  const att::Handle handle;
  const UUID type;

  // NOTE: In C++20 this can be generated via `= default` assignment.
  bool operator==(const DescriptorData& other) const {
    return handle == other.handle && type == other.type;
  }
};

// Delegates for ATT read/write operations
using ReadResponder = fit::callback<void(fit::result<att::ErrorCode> status,
                                         const ByteBuffer& value)>;
using WriteResponder = fit::callback<void(fit::result<att::ErrorCode> status)>;

// No-op implementations of asynchronous event handlers
inline void NopReadHandler(PeerId, IdType, IdType, uint16_t, ReadResponder) {}
inline void NopWriteHandler(
    PeerId, IdType, IdType, uint16_t, const ByteBuffer&, WriteResponder) {}
inline void NopCCCallback(IdType, IdType, PeerId, bool, bool) {}
inline void NopDisconnectCallback(IdType, PeerId) {}
inline void NopSendIndication(IdType, IdType, PeerId, BufferView) {}

// Characteristic Declaration attribute value (Core Spec v5.2, Vol 3,
// Sec 3.3.1).
template <att::UUIDType Format>
struct CharacteristicDeclarationAttributeValue {
  Properties properties;
  att::Handle value_handle;
  att::AttributeType<Format> value_uuid;
} __attribute__((packed));

// Service Changed Characteristic attribute value (Core Spec v5.2, Vol 3, Part
// G, Sec 7.1).
struct ServiceChangedCharacteristicValue {
  att::Handle range_start_handle;
  att::Handle range_end_handle;
} __attribute__((packed));

}  // namespace bt::gatt

// Specialization of std::hash for std::unordered_set, std::unordered_map, etc.
namespace std {

template <>
struct hash<bt::gatt::CharacteristicHandle> {
  size_t operator()(const bt::gatt::CharacteristicHandle& id) const {
    return std::hash<uint16_t>()(id.value);
  }
};
template <>
struct hash<bt::gatt::DescriptorHandle> {
  size_t operator()(const bt::gatt::DescriptorHandle& id) const {
    return std::hash<uint16_t>()(id.value);
  }
};

}  // namespace std
