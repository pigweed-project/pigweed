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

#include <cstdint>
#include <type_traits>

#include "pw_function/function.h"
#include "pw_preprocessor/compiler.h"
#include "pw_protobuf/wire_format.h"
#include "pw_result/result.h"
#include "pw_span/span.h"
#include "pw_status/status.h"

// TODO: b/259746255 - Remove this manual application of -Wconversion when all
// of
//     Pigweed builds with it.
PW_MODIFY_DIAGNOSTICS_PUSH();
PW_MODIFY_DIAGNOSTIC(error, "-Wconversion");

namespace pw::protobuf {
namespace internal {

// Varints can be encoded as an unsigned type, a signed type with normal
// encoding, or a signed type with zigzag encoding.
enum class VarintType {
  kUnsigned = 0,
  kNormal = 1,
  kZigZag = 2,
};

enum class CallbackType {
  kNone = 0,
  kSingleField = 1,
  kOneOfGroup = 2,
};

// Represents a field in a code generated message struct that can be the target
// for decoding or source of encoding.
//
// An instance of this class exists for every field in every protobuf in the
// binary, thus it is size critical to ensure efficiency while retaining enough
// information to describe the layout of the generated message struct.
//
// Limitations imposed:
//  - Element size of a repeated fields must be no larger than 15 bytes.
//    (8 byte int64/fixed64/double is the largest supported element).
//  - Individual field size (including repeated and nested messages) must be no
//    larger than 64 KB. (This is already the maximum size of pw::Vector).
//
// A complete codegen struct is represented by a span<MessageField>,
// holding a pointer to the MessageField members themselves, and the number of
// fields in the struct. These spans are global data, one span per protobuf
// message (including the size), and one MessageField per field in the message.
//
// Nested messages are handled with a pointer from the MessageField in the
// parent to a pointer to the (global data) span. Since the size of the nested
// message is stored as part of the global span, the cost of a nested message
// is only the size of a pointer to that span.
class MessageField {
 public:
  static constexpr unsigned int kMaxFieldSize = (1u << 16) - 1;

  constexpr MessageField(uint32_t field_number,
                         WireType wire_type,
                         size_t elem_size,
                         VarintType varint_type,
                         bool is_string,
                         bool is_fixed_size,
                         bool is_repeated,
                         bool is_optional,
                         CallbackType callback_type,
                         size_t field_offset,
                         size_t field_size,
                         const span<const MessageField>* nested_message_fields)
      : field_number_(field_number),
        field_info_(static_cast<uint32_t>(wire_type) << kWireTypeShift |
                    static_cast<uint32_t>(elem_size) << kElemSizeShift |
                    static_cast<uint32_t>(varint_type) << kVarintTypeShift |
                    static_cast<uint32_t>(is_string) << kIsStringShift |
                    static_cast<uint32_t>(is_fixed_size) << kIsFixedSizeShift |
                    static_cast<uint32_t>(is_repeated) << kIsRepeatedShift |
                    static_cast<uint32_t>(is_optional) << kIsOptionalShift |
                    static_cast<uint32_t>(callback_type) << kCallbackTypeShift |
                    static_cast<uint32_t>(field_size) << kFieldSizeShift),
        field_offset_(field_offset),
        nested_message_fields_(nested_message_fields) {}

  constexpr uint32_t field_number() const { return field_number_; }
  constexpr WireType wire_type() const {
    return static_cast<WireType>((field_info_ >> kWireTypeShift) &
                                 kWireTypeMask);
  }
  constexpr size_t elem_size() const {
    return (field_info_ >> kElemSizeShift) & kElemSizeMask;
  }
  constexpr VarintType varint_type() const {
    return static_cast<VarintType>((field_info_ >> kVarintTypeShift) &
                                   kVarintTypeMask);
  }
  constexpr bool is_string() const {
    return (field_info_ >> kIsStringShift) & 1;
  }
  constexpr bool is_fixed_size() const {
    return (field_info_ >> kIsFixedSizeShift) & 1;
  }
  constexpr bool is_repeated() const {
    return (field_info_ >> kIsRepeatedShift) & 1;
  }
  constexpr bool is_optional() const {
    return (field_info_ >> kIsOptionalShift) & 1;
  }
  constexpr CallbackType callback_type() const {
    return static_cast<CallbackType>((field_info_ >> kCallbackTypeShift) &
                                     kCallbackTypeMask);
  }
  constexpr size_t field_offset() const { return field_offset_; }
  constexpr size_t field_size() const {
    return (field_info_ >> kFieldSizeShift) & kFieldSizeMask;
  }
  constexpr const span<const MessageField>* nested_message_fields() const {
    return nested_message_fields_;
  }

  constexpr bool operator==(uint32_t field_number) const {
    return field_number == field_number_;
  }

 private:
  // field_info_ packs multiple fields into a single word as follows:
  //
  //   wire_type      : 3
  //   varint_type    : 2
  //   is_string      : 1
  //   is_fixed_size  : 1
  //   is_repeated    : 1
  //   [unused space] : 1
  //   -
  //   elem_size      : 4
  //   callback_type  : 2
  //   is_optional    : 1
  //   -
  //   field_size     : 16
  //
  // The protobuf field type is spread among a few fields (wire_type,
  // varint_type, is_string, elem_size). The exact field type (e.g. int32, bool,
  // message, etc.), from which all of that information can be derived, can be
  // represented in 4 bits. If more bits are needed in the future, these could
  // be consolidated into a single field type enum.
  static constexpr unsigned int kWireTypeShift = 29u;
  static constexpr unsigned int kWireTypeMask = (1u << 3) - 1;
  static constexpr unsigned int kVarintTypeShift = 27u;
  static constexpr unsigned int kVarintTypeMask = (1u << 2) - 1;
  static constexpr unsigned int kIsStringShift = 26u;
  static constexpr unsigned int kIsFixedSizeShift = 25u;
  static constexpr unsigned int kIsRepeatedShift = 24u;
  // Unused space: bit 23 (previously use_callback).
  static constexpr unsigned int kElemSizeShift = 19u;
  static constexpr unsigned int kElemSizeMask = (1u << 4) - 1;
  static constexpr unsigned int kCallbackTypeShift = 17;
  static constexpr unsigned int kCallbackTypeMask = (1u << 2) - 1;
  static constexpr unsigned int kIsOptionalShift = 16u;
  static constexpr unsigned int kFieldSizeShift = 0u;
  static constexpr unsigned int kFieldSizeMask = kMaxFieldSize;

  uint32_t field_number_;
  uint32_t field_info_;
  size_t field_offset_;
  // TODO: b/234875722 - Could be replaced by a class MessageDescriptor*
  const span<const MessageField>* nested_message_fields_;
};
static_assert(sizeof(MessageField) <= sizeof(size_t) * 4,
              "MessageField should be four words or less");

// Traits for a generated PWPB Message struct, specialized by codegen.
template <typename T>
struct MessageTraits {
  static constexpr bool kIsMessage = false;
};

template <typename...>
constexpr std::false_type kInvalidMessageStruct{};

}  // namespace internal

class StreamEncoder;
class StreamDecoder;

// Callback for a structure member that cannot be represented by a data type.
// Holds either a callback for encoding a field, or a callback for decoding
// a field.
template <typename StreamEncoder, typename StreamDecoder>
union Callback {
  constexpr Callback() : encode_() {}
  ~Callback() { encode_ = nullptr; }

  // Set the encoder callback.
  template <typename F>
  void SetEncoder(F&& encode) {
    static_assert(
        std::is_convertible_v<F, Function<Status(StreamEncoder&)>>,
        "Encode function signature must be Status(Message::StreamEncoder&)");
    encode_ = [enc = std::forward<F>(encode)](
                  ::pw::protobuf::StreamEncoder& base_encoder) mutable {
      return enc(static_cast<StreamEncoder&>(base_encoder));
    };
  }

  // Set the decoder callback.
  template <typename F>
  void SetDecoder(F&& decode) {
    static_assert(
        std::is_convertible_v<F, Function<Status(StreamDecoder&)>>,
        "Decode function signature must be Status(Message::StreamDecoder&)");
    decode_ = [dec = std::forward<F>(decode)](
                  ::pw::protobuf::StreamDecoder& base_decoder) mutable {
      return dec(static_cast<StreamDecoder&>(base_decoder));
    };
  }

  // Allow moving of callbacks by moving the member.
  constexpr Callback(Callback&& other) = default;
  constexpr Callback& operator=(Callback&& other) = default;

  // Copying a callback does not copy the functions.
  constexpr Callback(const Callback&) : encode_() {}
  constexpr Callback& operator=(const Callback&) {
    encode_ = nullptr;
    return *this;
  }

  // Evaluate to true if the encoder or decoder callback is set.
  explicit operator bool() const { return encode_ || decode_; }

 private:
  friend StreamDecoder;
  friend StreamEncoder;

  // Called by StreamEncoder to encode the structure member.
  // Returns OkStatus() if this has not been set by the caller, the default
  // behavior of a field without an encoder is the same as default-initialized
  // field.
  Status Encode(StreamEncoder& encoder) const {
    if (encode_) {
      return encode_(encoder);
    }
    return OkStatus();
  }

  // Called by StreamDecoder to decode the structure member when the field
  // is present. If the callback is unset, returns OkStatus to ignore the field.
  Status Decode(StreamDecoder& decoder) const {
    if (decode_) {
      return decode_(decoder);
    }
    return OkStatus();
  }

  Function<Status(::pw::protobuf::StreamEncoder& encoder)> encode_;
  Function<Status(::pw::protobuf::StreamDecoder& decoder)> decode_;
};

enum class NullFields : uint32_t {};

/// Callback for a oneof structure member.
/// A oneof callback will only be invoked once per struct member.
template <typename StreamEncoder,
          typename StreamDecoder,
          typename Fields = NullFields>
struct OneOf {
 public:
  constexpr OneOf() : invoked_(false), encode_() {}
  ~OneOf() { encode_ = nullptr; }

  // Set the encoder callback.
  template <typename F>
  void SetEncoder(F&& encode) {
    static_assert(
        std::is_convertible_v<F, Function<Status(StreamEncoder&)>>,
        "Encode function signature must be Status(Message::StreamEncoder&)");
    encode_ = [enc = std::forward<F>(encode)](
                  ::pw::protobuf::StreamEncoder& base_encoder) mutable {
      return enc(static_cast<StreamEncoder&>(base_encoder));
    };
  }

  // Set the decoder callback.
  template <typename F>
  void SetDecoder(F&& decode) {
    static_assert(
        std::is_convertible_v<F, Function<Status(Fields, StreamDecoder&)>>,
        "Decode function signature must be Status(Fields, "
        "Message::StreamDecoder&)");
    decode_ = [dec = std::forward<F>(decode)](
                  uint32_t field,
                  ::pw::protobuf::StreamDecoder& base_decoder) mutable {
      return dec(static_cast<Fields>(field),
                 static_cast<StreamDecoder&>(base_decoder));
    };
  }

  // Allow moving of callbacks by moving the member.
  constexpr OneOf(OneOf&& other) = default;
  constexpr OneOf& operator=(OneOf&& other) = default;

  // Copying a callback does not copy the functions.
  constexpr OneOf(const OneOf&) : encode_() {}
  constexpr OneOf& operator=(const OneOf&) {
    encode_ = nullptr;
    return *this;
  }

  // Evaluate to true if the encoder or decoder callback is set.
  explicit operator bool() const { return encode_ || decode_; }

 private:
  friend StreamDecoder;
  friend StreamEncoder;

  constexpr void ResetForNewWrite() const { invoked_ = false; }

  Status Encode(StreamEncoder& encoder) const {
    if (encode_) {
      if (invoked_) {
        // The oneof has already been encoded.
        return OkStatus();
      }

      invoked_ = true;
      return encode_(encoder);
    }
    return OkStatus();
  }

  Status Decode(Fields field, StreamDecoder& decoder) const {
    if (decode_) {
      if (invoked_) {
        // Multiple fields from the same oneof exist in the serialized message.
        return Status::DataLoss();
      }

      invoked_ = true;
      return decode_(static_cast<uint32_t>(field), decoder);
    }
    return OkStatus();
  }

  mutable bool invoked_;
  union {
    Function<Status(::pw::protobuf::StreamEncoder& encoder)> encode_;
    Function<Status(uint32_t field, ::pw::protobuf::StreamDecoder& decoder)>
        decode_;
  };
};

template <typename T>
constexpr bool IsTriviallyComparable() {
  static_assert(internal::kInvalidMessageStruct<T>,
                "Not a generated message struct");
  return false;
}

}  // namespace pw::protobuf

PW_MODIFY_DIAGNOSTICS_POP();
