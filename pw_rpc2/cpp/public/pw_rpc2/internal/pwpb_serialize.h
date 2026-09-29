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

#include <cstddef>

#include "pw_bytes/span.h"
#include "pw_protobuf/internal/codegen.h"
#include "pw_result/result.h"
#include "pw_rpc2/internal/serialize.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"
#include "pw_status/try.h"

namespace pw::rpc2::internal {

using PwpbMessageDescriptor =
    const span<const protobuf::internal::MessageField>*;

constexpr bool HasDecodeCallbacks(PwpbMessageDescriptor table) {
  for (const auto& field : *table) {
    if (field.callback_type() != protobuf::internal::CallbackType::kNone) {
      return true;
    }
    if (field.nested_message_fields() != nullptr &&
        HasDecodeCallbacks(field.nested_message_fields())) {
      return true;
    }
  }
  return false;
}

// True if every field described by `table` lies within a `Message` object.
//
// The descriptor table carries no type information, so this is a cheap guard
// against pairing a message struct with another message's table, which would
// make the encoder and decoder access memory outside of the struct.
template <typename Message>
constexpr bool FieldsFitIn(PwpbMessageDescriptor table) {
  for (const auto& field : *table) {
    if (field.field_offset() + field.field_size() > sizeof(Message)) {
      return false;
    }
  }
  return true;
}

StatusWithSize PwpbEncodedSizeBytes(span<const std::byte> message,
                                    PwpbMessageDescriptor table);

StatusWithSize PwpbEncode(span<const std::byte> message,
                          PwpbMessageDescriptor table,
                          span<std::byte> destination);

Status PwpbDecode(span<const std::byte> source,
                  PwpbMessageDescriptor table,
                  span<std::byte> message);

/// Low-level serializer bound to a specific PWPB message descriptor table.
template <PwpbMessageDescriptor kTable>
struct PwpbSerde {
  template <typename Message>
  static StatusWithSize EncodedSizeBytes(const Message& value) {
    CheckMessage<Message>();
    return PwpbEncodedSizeBytes(as_bytes(span(&value, 1)), kTable);
  }

  template <typename Message>
  static StatusWithSize Serialize(const Message& value,
                                  span<std::byte> destination) {
    CheckMessage<Message>();
    return PwpbEncode(as_bytes(span(&value, 1)), kTable, destination);
  }

  template <typename Message>
  static Result<Message> Deserialize(span<const std::byte> source) {
    CheckMessage<Message>();
    Message value{};
    PW_TRY(PwpbDecode(source, kTable, as_writable_bytes(span(&value, 1))));
    return value;
  }

 private:
  template <typename Message>
  static constexpr void CheckMessage() {
    static_assert(
        FieldsFitIn<Message>(kTable),
        "The message type does not match the PWPB descriptor table: "
        "a field in the table lies outside of the message struct. "
        "Verify that the message type passed to Serialize() or Deserialize() "
        "matches the proto message for which this serializer was configured.");
  }
};

/// Primary PwpbSerializer for a PWPB message type.
template <PwpbMessageDescriptor kTable, size_t kMaxSizeBytes>
struct PwpbSerializer {
  template <typename MessageType>
  static constexpr size_t MaxEncodedSize(const MessageType& value) {
    if constexpr (HasDecodeCallbacks(kTable)) {
      const StatusWithSize result = PwpbSerde<kTable>::EncodedSizeBytes(value);
      return result.ok() ? result.size() : kMaxSizeBytes;
    } else {
      return kMaxSizeBytes;
    }
  }

  template <typename MessageType>
  static StatusWithSize Serialize(const MessageType& value,
                                  span<std::byte> destination) {
    return PwpbSerde<kTable>::Serialize(value, destination);
  }

  template <typename MessageType>
  static Result<MessageType> Deserialize(span<const std::byte> source) {
    return PwpbSerde<kTable>::template Deserialize<MessageType>(source);
  }
};

}  // namespace pw::rpc2::internal
