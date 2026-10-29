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
///
/// @deprecated pw_channel is deprecated; use pw::transport::Socket or
/// pw::transport::ReliableSocket.
enum Property : uint8_t {
  /// All data is guaranteed to be delivered in order. The channel is closed if
  /// data is lost.
  kReliable = 1 << 0,

  /// The channel supports reading.
  kReadable = 1 << 1,

  /// The channel supports writing.
  kWritable = 1 << 2,
};

/// The type of data exchanged in `Channel` read and write calls. Unlike
/// `Property`, `Channels` with different `DataType`s cannot be used
/// interchangeably.
///
/// @deprecated pw_channel is deprecated; use pw::transport::Socket or
/// pw::transport::ReliableSocket.
enum class DataType : uint8_t {
  kByte = 0,
  kDatagram = 1,
};

/// @endmodule

template <DataType kDataType, Property... kProperties>
class Channel;

class AnyChannel;

namespace internal {

template <DataType kDataType, Property... kProperties>
class BaseChannelImpl;

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
        (!Sibling::writable() || Self::writable())>;

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
  static_assert(((kProperties == kReadable) || ...) ||
                    ((kProperties == kWritable) || ...),
                "At least one of kReadable or kWritable must be provided");
  static_assert(sizeof...(kProperties) <= 3,
                "Too many properties given; no more than 3 may be specified "
                "(kReliable, kReadable, kWritable)");
  static_assert(PropertiesAreInOrderWithoutDuplicates<kProperties...>(),
                "Properties must be specified in the following order, without "
                "duplicates: kReliable, kReadable, kWritable");
  return true;
}

}  // namespace internal

}  // namespace pw::channel
