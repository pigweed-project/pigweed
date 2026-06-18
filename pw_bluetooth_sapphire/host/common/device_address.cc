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

#include "pw_bluetooth_sapphire/internal/host/common/device_address.h"

#include <pw_assert/check.h>

#include "pw_bluetooth/hci_common.emb.h"
#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_preprocessor/compiler.h"
#include "pw_string/format.h"

namespace bt {
namespace {

std::string TypeToString(DeviceAddress::Type type) {
  switch (type) {
    case DeviceAddress::Type::kBREDR:
      return "(BD_ADDR) ";
    case DeviceAddress::Type::kLEPublic:
      return "(LE publ) ";
    case DeviceAddress::Type::kLERandom:
      return "(LE rand) ";
    case DeviceAddress::Type::kLEAnonymous:
      return "(LE anon) ";
  }

  return "(invalid) ";
}

}  // namespace

DeviceAddressBytes::DeviceAddressBytes() { SetToZero(); }

DeviceAddressBytes::DeviceAddressBytes(
    std::array<uint8_t, kDeviceAddressSize> bytes) {
  bytes_ = bytes;
}

DeviceAddressBytes::DeviceAddressBytes(const ByteBuffer& bytes) {
  PW_DCHECK(bytes.size() == bytes_.size());
  std::copy(bytes.cbegin(), bytes.cend(), bytes_.begin());
}

DeviceAddressBytes::DeviceAddressBytes(pw::bluetooth::emboss::BdAddrView view) {
  pw::bluetooth::emboss::MakeBdAddrView(&bytes_).CopyFrom(view);
}

std::string DeviceAddressBytes::ToString() const {
  constexpr size_t out_size = sizeof("00:00:00:00:00:00");
  char out[out_size] = "";
  // Ignore errors. If an error occurs, an empty string will be returned.
  pw::StatusWithSize result =
      pw::string::Format({out, sizeof(out)},
                         "%02X:%02X:%02X:%02X:%02X:%02X",
                         bytes_[5],
                         bytes_[4],
                         bytes_[3],
                         bytes_[2],
                         bytes_[1],
                         bytes_[0]);
  PW_DCHECK(result.ok());
  return out;
}

void DeviceAddressBytes::SetToZero() { bytes_.fill(0); }

std::size_t DeviceAddressBytes::Hash() const {
  uint64_t bytes_as_int = 0;
  int shift_amount = 0;
  for (const uint8_t& byte : bytes_) {
    bytes_as_int |= (static_cast<uint64_t>(byte) << shift_amount);
    shift_amount += 8;
  }

  std::hash<uint64_t> hash_func;
  return hash_func(bytes_as_int);
}

DeviceAddress::DeviceAddress() : type_(Type::kBREDR) {}

DeviceAddress::DeviceAddress(Type type, const DeviceAddressBytes& value)
    : type_(type), value_(value) {}

DeviceAddress::DeviceAddress(Type type,
                             std::array<uint8_t, kDeviceAddressSize> bytes)
    : DeviceAddress(type, DeviceAddressBytes(bytes)) {}

std::optional<pw::bluetooth::emboss::LEAddressType>
DeviceAddress::DeviceAddrToLeAddr(DeviceAddress::Type type) {
  switch (type) {
    case DeviceAddress::Type::kLEPublic:
      return pw::bluetooth::emboss::LEAddressType::PUBLIC;
    case DeviceAddress::Type::kLERandom:
      return pw::bluetooth::emboss::LEAddressType::RANDOM;
    case DeviceAddress::Type::kBREDR:
    case DeviceAddress::Type::kLEAnonymous:
      bt_log(DEBUG,
             "common",
             "invalid DeviceAddress::Type for LEAddressType: %u",
             static_cast<unsigned int>(type));
      return std::nullopt;
  }
}

std::optional<pw::bluetooth::emboss::LEPeerAddressType>
DeviceAddress::DeviceAddrToLePeerAddr(Type type) {
  switch (type) {
    case DeviceAddress::Type::kLEPublic:
      return pw::bluetooth::emboss::LEPeerAddressType::PUBLIC;
    case DeviceAddress::Type::kLERandom:
      return pw::bluetooth::emboss::LEPeerAddressType::RANDOM;
    case DeviceAddress::Type::kLEAnonymous:
      return pw::bluetooth::emboss::LEPeerAddressType::ANONYMOUS;
    case DeviceAddress::Type::kBREDR:
      bt_log(
          DEBUG, "common", "BR/EDR address not convertible to LE peer address");
      return std::nullopt;
  }
}

std::optional<pw::bluetooth::emboss::LEPeerAddressTypeNoAnon>
DeviceAddress::DeviceAddrToLePeerAddrNoAnon(Type type) {
  switch (type) {
    case DeviceAddress::Type::kLEPublic:
      return pw::bluetooth::emboss::LEPeerAddressTypeNoAnon::PUBLIC;
    case DeviceAddress::Type::kLERandom:
      return pw::bluetooth::emboss::LEPeerAddressTypeNoAnon::RANDOM;
    case DeviceAddress::Type::kBREDR:
    case DeviceAddress::Type::kLEAnonymous:
      bt_log(DEBUG,
             "common",
             "invalid DeviceAddress::Type for LEPeerAddressTypeNoAnon: %u",
             static_cast<unsigned int>(type));
      return std::nullopt;
  }
}

std::optional<pw::bluetooth::emboss::LEExtendedAddressType>
DeviceAddress::DeviceAddrToLeExtendedAddr(Type type) {
  switch (type) {
    case DeviceAddress::Type::kLEPublic:
      return pw::bluetooth::emboss::LEExtendedAddressType::PUBLIC;
    case DeviceAddress::Type::kLERandom:
      return pw::bluetooth::emboss::LEExtendedAddressType::RANDOM;
    case DeviceAddress::Type::kLEAnonymous:
      return pw::bluetooth::emboss::LEExtendedAddressType::ANONYMOUS;
    case DeviceAddress::Type::kBREDR:
      bt_log(DEBUG,
             "common",
             "BR/EDR address not convertible to LE extended address");
      return std::nullopt;
  }
}

std::optional<pw::bluetooth::emboss::LEOwnAddressType>
DeviceAddress::DeviceAddrToLeOwnAddr(Type type) {
  switch (type) {
    case DeviceAddress::Type::kLERandom:
      return pw::bluetooth::emboss::LEOwnAddressType::RANDOM;
    case DeviceAddress::Type::kLEPublic:
      return pw::bluetooth::emboss::LEOwnAddressType::PUBLIC;
    case DeviceAddress::Type::kBREDR:
    case DeviceAddress::Type::kLEAnonymous:
      bt_log(DEBUG,
             "common",
             "invalid DeviceAddress::Type for LEOwnAddressType: %u",
             static_cast<unsigned int>(type));
      return std::nullopt;
  }
}

std::optional<DeviceAddress::Type> DeviceAddress::LeAddrToDeviceAddr(
    pw::bluetooth::emboss::LEAddressType type) {
  switch (type) {
    case pw::bluetooth::emboss::LEAddressType::PUBLIC:
    case pw::bluetooth::emboss::LEAddressType::PUBLIC_IDENTITY: {
      return DeviceAddress::Type::kLEPublic;
    }
    case pw::bluetooth::emboss::LEAddressType::RANDOM:
    case pw::bluetooth::emboss::LEAddressType::RANDOM_IDENTITY: {
      return DeviceAddress::Type::kLERandom;
    }
    default: {
      bt_log(DEBUG,
             "common",
             "invalid LEAddressType: %u",
             static_cast<unsigned int>(type));
      return std::nullopt;
    }
  }
}

std::optional<DeviceAddress::Type> DeviceAddress::LeAddrToDeviceAddr(
    pw::bluetooth::emboss::LEPeerAddressType type) {
  switch (type) {
    case pw::bluetooth::emboss::LEPeerAddressType::PUBLIC: {
      return DeviceAddress::Type::kLEPublic;
    }
    case pw::bluetooth::emboss::LEPeerAddressType::RANDOM: {
      return DeviceAddress::Type::kLERandom;
    }
    case pw::bluetooth::emboss::LEPeerAddressType::ANONYMOUS: {
      return DeviceAddress::Type::kLEAnonymous;
    }
    default: {
      bt_log(DEBUG,
             "common",
             "invalid LEPeerAddressType: %u",
             static_cast<unsigned int>(type));
      return std::nullopt;
    }
  }
}

std::optional<DeviceAddress::Type> DeviceAddress::LeAddrToDeviceAddr(
    pw::bluetooth::emboss::LEPeerAddressTypeNoAnon type) {
  switch (type) {
    case pw::bluetooth::emboss::LEPeerAddressTypeNoAnon::PUBLIC: {
      return DeviceAddress::Type::kLEPublic;
    }
    case pw::bluetooth::emboss::LEPeerAddressTypeNoAnon::RANDOM: {
      return DeviceAddress::Type::kLERandom;
    }
    default: {
      bt_log(DEBUG,
             "common",
             "invalid LEPeerAddressTypeNoAnon: %u",
             static_cast<unsigned int>(type));
      return std::nullopt;
    }
  }
}

std::optional<DeviceAddress::Type> DeviceAddress::LeAddrToDeviceAddr(
    pw::bluetooth::emboss::LEExtendedAddressType type) {
  switch (type) {
    case pw::bluetooth::emboss::LEExtendedAddressType::PUBLIC:
    case pw::bluetooth::emboss::LEExtendedAddressType::PUBLIC_IDENTITY: {
      return DeviceAddress::Type::kLEPublic;
    }
    case pw::bluetooth::emboss::LEExtendedAddressType::RANDOM:
    case pw::bluetooth::emboss::LEExtendedAddressType::RANDOM_IDENTITY: {
      return DeviceAddress::Type::kLERandom;
    }
    case pw::bluetooth::emboss::LEExtendedAddressType::ANONYMOUS: {
      return DeviceAddress::Type::kLEAnonymous;
    }
    default: {
      bt_log(DEBUG,
             "common",
             "invalid LEExtendedAddressType: %u",
             static_cast<unsigned int>(type));
      return std::nullopt;
    }
  }
}

bool DeviceAddress::IsResolvablePrivate() const {
  // "The two most significant bits of [a RPA] shall be equal to 0 and 1".
  // (Vol 6, Part B, 1.3.2.2).
  uint8_t msb = value_.bytes()[5];
  return type_ == Type::kLERandom && (msb & 0b01000000) && (~msb & 0b10000000);
}

bool DeviceAddress::IsNonResolvablePrivate() const {
  // "The two most significant bits of [a NRPA] shall be equal to 0".
  // (Vol 6, Part B, 1.3.2.2).
  uint8_t msb = value_.bytes()[5];
  return type_ == Type::kLERandom && !(msb & 0b11000000);
}

bool DeviceAddress::IsStaticRandom() const {
  // "The two most significant bits of [a static random address] shall be
  // equal to 1". (Vol 6, Part B, 1.3.2.1).
  uint8_t msb = value_.bytes()[5];
  return type_ == Type::kLERandom && ((msb & 0b11000000) == 0b11000000);
}

std::size_t DeviceAddress::Hash() const {
  const Type type_for_hashing = IsPublic() ? Type::kBREDR : type_;
  std::size_t const h1(std::hash<Type>{}(type_for_hashing));
  std::size_t h2 = value_.Hash();

  return h1 ^ (h2 << 1);
}

std::string DeviceAddress::ToString() const {
  return TypeToString(type_) + value_.ToString();
}

}  // namespace bt

namespace std {

hash<bt::DeviceAddress>::result_type hash<bt::DeviceAddress>::operator()(
    argument_type const& value) const {
  return value.Hash();
}

}  // namespace std
