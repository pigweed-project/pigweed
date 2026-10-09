// Copyright 2026 The Pigweed Authors
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

#include <cstring>
#include <type_traits>

#include "pw_buf/buf.h"
#include "pw_bytes/span.h"
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"

namespace pw::rpc2::internal {

/// Primary template for Serializer deduction.
///
/// Specialized by the code generator or custom integrations to define the
/// serialization backend for a given message type.
template <typename T, typename = void>
struct SerializerFor;

template <typename T>
struct SerializerFor<T, std::void_t<typename T::Serializer>> {
  using type = typename T::Serializer;
};

template <>
struct SerializerFor<ConstBuf> {
  struct type {
    static StatusWithSize Serialize(const ConstBuf& bytes,
                                    span<std::byte> destination) {
      if (destination.size() < bytes.size()) {
        return StatusWithSize::ResourceExhausted();
      }
      if (!bytes.empty()) {
        std::memcpy(destination.data(), bytes.data(), bytes.size());
      }
      return StatusWithSize(OkStatus(), bytes.size());
    }
  };
};

/// Returns an upper bound on the number of bytes `value` serializes to.
///
/// @warning For protobuf messages without callback fields this is the
/// maximum encoded size of the message *type*, not the size of this
/// particular value. For messages with callback fields it is computed by a
/// sizing pass that runs the encode callbacks, so side-effecting callbacks run
/// once more when the message is written.
template <typename T>
inline size_t MaxEncodedSize(const T& value) {
  static_assert(!std::is_same_v<T, ConstBuf>,
                "Use ConstBuf::size() directly for raw messages.");
  return SerializerFor<T>::type::MaxEncodedSize(value);
}

template <typename Serializer, typename T, typename = void>
inline constexpr bool kHasReservationSize = false;

template <typename Serializer, typename T>
inline constexpr bool
    kHasReservationSize<Serializer,
                        T,
                        std::void_t<decltype(Serializer::ReservationSize(
                            std::declval<const T&>(), size_t{}))>> = true;

/// Returns the number of payload bytes to reserve to write `value` in a
/// packet whose payload can hold at most `payload_limit` bytes.
///
/// This is `MaxEncodedSize(value)`, unless that exceeds `payload_limit` and
/// the serializer can compute a smaller bound for this particular value, which
/// it does by providing
/// `static size_t ReservationSize(const T&, size_t payload_limit)`. The pwpb
/// serializer does: a message type sized for a large link stays usable on a
/// smaller one as long as each value fits, at the cost of a sizing pass for
/// values of types whose maximum does not fit.
template <typename T>
inline size_t ReservationSize(const T& value, size_t payload_limit) {
  static_assert(!std::is_same_v<T, ConstBuf>,
                "Use ConstBuf::size() directly for raw messages.");
  using Serializer = typename SerializerFor<T>::type;
  if constexpr (kHasReservationSize<Serializer, T>) {
    return Serializer::ReservationSize(value, payload_limit);
  } else {
    return Serializer::MaxEncodedSize(value);
  }
}

/// Generic helper to serialize a message into a byte span.
template <typename T>
inline StatusWithSize Serialize(const T& value, span<std::byte> destination) {
  return SerializerFor<T>::type::Serialize(value, destination);
}

/// Generic helper to deserialize a message from a borrowed byte span.
///
/// Not available for `ConstBuf`: a borrowed span cannot become an owning
/// buffer. Use `ConstBuf::Unowned(source)` for raw messages.
template <typename T>
inline Result<T> Deserialize(span<const std::byte> source) {
  static_assert(!std::is_same_v<T, ConstBuf>,
                "Cannot deserialize a ConstBuf from a borrowed span, "
                "since the result would not own its bytes. Use "
                "ConstBuf::Unowned(source) instead.");
  return SerializerFor<T>::type::template Deserialize<T>(source);
}

}  // namespace pw::rpc2::internal
