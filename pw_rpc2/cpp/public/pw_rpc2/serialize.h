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
#include <utility>

#include "pw_buf/buf.h"
#include "pw_bytes/span.h"
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"
#include "pw_status/try.h"

namespace pw::rpc2 {

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
    static size_t MaxEncodedSize(const ConstBuf& buf) { return buf.size(); }
    static StatusWithSize Serialize(const ConstBuf& buf,
                                    span<std::byte> destination) {
      if (destination.size() < buf.size()) {
        return StatusWithSize::ResourceExhausted();
      }
      std::memcpy(destination.data(), buf.data(), buf.size());
      return StatusWithSize(OkStatus(), buf.size());
    }
  };
};

/// Returns an upper bound on the number of bytes `value` serializes to.
///
/// This is the amount of transport buffer that must be reserved to write
/// `value`.
///
/// @warning For protobuf messages this is the maximum encoded size of the
/// message *type*, not the size of this particular value: pwpb cannot
/// determine the exact size without encoding. Writing a message therefore
/// reserves `kMaxEncodedSizeBytes` of transport buffer however sparsely
/// populated it is, and the reservation is truncated to the real size only
/// after encoding. Raw (`ConstBuf`) payloads report their exact size.
template <typename T>
inline size_t MaxEncodedSize(const T& value) {
  return SerializerFor<T>::type::MaxEncodedSize(value);
}

/// Generic helper to serialize a message into a byte span.
template <typename T>
inline StatusWithSize Serialize(const T& value, span<std::byte> destination) {
  return SerializerFor<T>::type::Serialize(value, destination);
}

namespace internal {

template <typename Serializer, typename T, typename = void>
struct HasDeserializeInto : std::false_type {};

template <typename Serializer, typename T>
struct HasDeserializeInto<
    Serializer,
    T,
    std::void_t<decltype(Serializer::DeserializeInto(
        std::declval<span<const std::byte>>(), std::declval<T&>()))>>
    : std::true_type {};

}  // namespace internal

/// Generic helper to deserialize a message in-place from a borrowed byte span.
template <typename T>
inline Status DeserializeInto(span<const std::byte> source, T& out) {
  static_assert(!std::is_same_v<T, ConstBuf>,
                "Cannot deserialize a ConstBuf from a borrowed span.");
  using Serializer = typename SerializerFor<T>::type;
  if constexpr (internal::HasDeserializeInto<Serializer, T>::value) {
    return Serializer::DeserializeInto(source, out);
  } else {
    PW_TRY_ASSIGN(auto res, Serializer::template Deserialize<T>(source));
    out = std::move(*res);
    return OkStatus();
  }
}

/// Generic helper to deserialize a message from a borrowed byte span.
///
/// Not available for `ConstBuf`: a borrowed span cannot become an owning
/// buffer. Use `ConstBuf::Unowned(source)` for raw payloads.
template <typename T>
inline Result<T> Deserialize(span<const std::byte> source) {
  static_assert(!std::is_same_v<T, ConstBuf>,
                "Cannot deserialize a ConstBuf from a borrowed span, "
                "since the result would not own its bytes. Use "
                "ConstBuf::Unowned(source) instead.");
  return SerializerFor<T>::type::template Deserialize<T>(source);
}

/// Generic helper to deserialize a message from a buffer.
///
/// Deserializing a `ConstBuf` is the identity operation, so raw and typed
/// call paths can share one code path.
template <typename T>
inline Result<T> Deserialize(ConstBuf&& source) {
  if constexpr (std::is_same_v<T, ConstBuf>) {
    return std::move(source);
  } else {
    return SerializerFor<T>::type::template Deserialize<T>(source);
  }
}

}  // namespace pw::rpc2
