// Copyright 2024 The Pigweed Authors
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
#include <type_traits>

namespace pw::channel {

/// @module{pw_channel}

/// Basic properties of a `Channel`. A `Channel` type can convert to any other
/// `Channel` for which it supports the required properties. For example, a
/// `kReadable` and `kWritable` channel may be passed to an API that only
/// requires `kReadable`.
enum Property : uint8_t {
  /// All data is guaranteed to be delivered in order. The channel is closed if
  /// data is lost.
  kReliable = 1 << 0,

  /// The channel supports reading.
  kReadable = 1 << 1,

  /// The channel supports writing.
  kWritable = 1 << 2,

  /// The channel supports seeking (changing the read/write position).
  kSeekable = 1 << 3,
};

/// The type of data exchanged in `Channel` read and write calls. Unlike
/// `Property`, `Channels` with different `DataType`s cannot be used
/// interchangeably.
enum class DataType : uint8_t { kByte = 0, kDatagram = 1 };

/// Positions from which to seek.
enum Whence : uint8_t {
  /// Seek from the beginning of the channel. The offset is a direct offset
  /// into the data.
  kBeginning,

  /// Seek from the current position in the channel. The offset is added to
  /// the current position. Use a negative offset to seek backwards.
  ///
  /// Implementations may only support seeking within a limited range from the
  /// current position.
  kCurrent,

  /// Seek from the end of the channel. The offset is added to the end
  /// position. Use a negative offset to seek backwards from the end.
  kEnd,
};

/// @}

template <DataType kDataType, Property... kProperties>
class Channel;

class AnyChannel;

template <typename Packet, Property... kProperties>
class PacketChannel;

template <typename Packet>
class AnyPacketChannel;

namespace internal {

template <DataType kDataType, Property... kProperties>
class BaseChannelImpl;

template <typename Packet, Property... kProperties>
class PacketChannelImpl;

template <typename Packet, Property... kProperties>
class BasePacketChannelImpl;

template <typename>
struct IsChannel : public std::false_type {};

template <DataType kDataType, Property... kProperties>
struct IsChannel<Channel<kDataType, kProperties...>> : public std::true_type {};

// Returns whether a sibling channel supports the required properties.
template <typename Self, typename Sibling>
using EnableIfConversionIsValid =
    std::enable_if_t<  // Sibling type must be a Channel
        IsChannel<Sibling>::value &&
        // Datagram and byte channels are not interchangeable
        (Sibling::data_type() == Self::data_type()) &&
        // Cannot use a unreliable channel as a reliable channel
        (!Sibling::reliable() || Self::reliable()) &&
        // Cannot use a non-readable channel as a readable channel
        (!Sibling::readable() || Self::readable()) &&
        // Cannot use a non-writable channel as a writable channel
        (!Sibling::writable() || Self::writable()) &&
        // Cannot use a non-seekable channel as a seekable channel
        (!Sibling::seekable() || Self::seekable())>;

// Performs the same checks as EnableIfConversionIsValid, but generates a
// static_assert with a helpful message if any condition is not met.
template <typename Self, typename Sibling>
[[maybe_unused]] static constexpr void CheckThatConversionIsValid() {
  if constexpr (!std::is_same_v<Sibling, AnyChannel>) {
    static_assert(IsChannel<Sibling>::value,
                  "Only conversions to other Channel types are supported");
    static_assert(Sibling::data_type() == Self::data_type(),
                  "Datagram and byte channels are not interchangeable");
    static_assert(!Sibling::reliable() || Self::reliable(),
                  "Cannot use a unreliable channel as a reliable channel");
    static_assert(!Sibling::readable() || Self::readable(),
                  "Cannot use a non-readable channel as a readable channel");
    static_assert(!Sibling::writable() || Self::writable(),
                  "Cannot use a non-writable channel as a writable channel");
    static_assert(!Sibling::seekable() || Self::seekable(),
                  "Cannot use a non-seekable channel as a seekable channel");
  }
}

template <Property>
constexpr bool PropertiesAreInOrderWithoutDuplicates() {
  return true;
}

template <Property kLhs, Property kRhs, Property... kOthers>
constexpr bool PropertiesAreInOrderWithoutDuplicates() {
  return (kLhs < kRhs) &&
         PropertiesAreInOrderWithoutDuplicates<kRhs, kOthers...>();
}

template <Property... kProperties>
[[maybe_unused]] static constexpr bool PropertiesAreValid() {
  static_assert(((kProperties != kSeekable) && ...),
                "Seekable channels are not yet implemented; see b/323624921");

  static_assert(((kProperties == kReadable) || ...) ||
                    ((kProperties == kWritable) || ...),
                "At least one of kReadable or kWritable must be provided");
  static_assert(sizeof...(kProperties) <= 4,
                "Too many properties given; no more than 4 may be specified "
                "(kReliable, kReadable, kWritable, kSeekable)");
  static_assert(PropertiesAreInOrderWithoutDuplicates<kProperties...>(),
                "Properties must be specified in the following order, without "
                "duplicates: kReliable, kReadable, kWritable, kSeekable");
  return true;
}

template <Property... kProperties>
[[maybe_unused]] static constexpr bool PacketChannelPropertiesAreValid() {
  static_assert(
      ((kProperties != kReliable) && ...),
      "PacketChannel only supports the kReadable and kWritable properties");
  return PropertiesAreValid<kProperties...>();
}

template <typename, typename>
struct CompatiblePacketChannels : public std::false_type {};

template <typename Packet,
          Property... kProperties,
          Property... kOtherProperties>
struct CompatiblePacketChannels<PacketChannel<Packet, kProperties...>,
                                PacketChannel<Packet, kOtherProperties...>>
    : public std::true_type {};

template <typename Self, typename Sibling>
using EnableIfConvertibleImpl =
    std::enable_if_t<CompatiblePacketChannels<Self, Sibling>::value &&
                     // Cannot use a non-readable channel as a readable channel
                     (!Sibling::readable() || Self::readable()) &&
                     // Cannot use a non-writable channel as a writable channel
                     (!Sibling::writable() || Self::writable())>;

template <typename Self, typename Sibling>
using EnableIfConvertible =
    EnableIfConvertibleImpl<std::remove_cv_t<Self>, std::remove_cv_t<Sibling>>;

// Performs the same checks as EnableIfConvertible, but generates a
// static_assert with a helpful message if any condition is not met.
template <typename SelfType, typename SiblingType>
[[maybe_unused]] static constexpr void CheckPacketChannelConversion() {
  using Self = std::remove_cv_t<SelfType>;
  using Sibling = std::remove_cv_t<SiblingType>;
  if constexpr (!std::is_same_v<Sibling,
                                AnyPacketChannel<typename Self::Packet>>) {
    static_assert(CompatiblePacketChannels<Self, Sibling>::value,
                  "Only conversions to other PacketChannels are supported");
    static_assert(!Sibling::readable() || Self::readable(),
                  "Cannot use a non-readable channel as a readable channel");
    static_assert(!Sibling::writable() || Self::writable(),
                  "Cannot use a non-writable channel as a writable channel");
  }
}

}  // namespace internal
}  // namespace pw::channel
