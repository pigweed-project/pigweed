// Copyright 2020 The Pigweed Authors
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

#include "pw_protobuf/wire_format.h"
#include "pw_varint/varint.h"

namespace pw::protobuf {

/// @module{pw_protobuf}

/// Field sizes that directly map to fixed wire types:
inline constexpr size_t kMaxSizeBytesFixed32 = 4;
inline constexpr size_t kMaxSizeBytesFixed64 = 8;
inline constexpr size_t kMaxSizeBytesSfixed32 = 4;
inline constexpr size_t kMaxSizeBytesSfixed64 = 8;
inline constexpr size_t kMaxSizeBytesFloat = kMaxSizeBytesFixed32;
inline constexpr size_t kMaxSizeBytesDouble = kMaxSizeBytesFixed64;

/// Field sizes that map to varints:
inline constexpr size_t kMaxSizeBytesUint32 = varint::kMaxVarint32SizeBytes;
inline constexpr size_t kMaxSizeBytesUint64 = varint::kMaxVarint64SizeBytes;
inline constexpr size_t kMaxSizeBytesSint32 = varint::kMaxVarint32SizeBytes;
inline constexpr size_t kMaxSizeBytesSint64 = varint::kMaxVarint64SizeBytes;
/// The `int32` field type does not use zigzag encoding, so negative values can
/// result in the worst-case varint size (`kMaxVarint64SizeBytes`).
inline constexpr size_t kMaxSizeBytesInt32 = varint::kMaxVarint64SizeBytes;
inline constexpr size_t kMaxSizeBytesInt64 = varint::kMaxVarint64SizeBytes;
/// The `bool` field type is backed by a varint, but has a limited value range
/// (`1` byte).
inline constexpr size_t kMaxSizeBytesBool = 1;

inline constexpr size_t kMaxSizeBytesEnum = kMaxSizeBytesInt32;

inline constexpr size_t kMaxSizeOfFieldNumber = varint::kMaxVarint32SizeBytes;

inline constexpr size_t kMaxSizeOfLength = varint::kMaxVarint32SizeBytes;

/// Calculates the serialized size of a proto tag (field number + wire type).
///
/// @param[in] field_number The field number for the field.
/// @returns The size in bytes of the field's encoded tag.
///
/// @pre `field_number` must be a valid field number
/// (`ValidFieldNumber(field_number)`).
template <typename T>
constexpr size_t TagSizeBytes(T field_number) {
  static_assert((std::is_enum<T>() || std::is_integral<T>()) &&
                    sizeof(T) <= sizeof(uint32_t),
                "Field numbers must be 32-bit enums or integers");
  // The wire type does not impact the serialized size, so use kVarint (0),
  // which is optimized out by the compiler.
  return varint::EncodedSize(
      FieldKey(static_cast<uint32_t>(field_number), WireType::kVarint));
}

/// Calculates the size in bytes of a varint field (`uint32`/`64`, `int32`/`64`,
/// `sint32`/`64`, `enum`).
template <typename T, typename U>
constexpr size_t SizeOfVarintField(T field_number, U value) {
  return TagSizeBytes(field_number) +
         varint::EncodedSize(static_cast<uint64_t>(value));
}

/// Calculates the size of a delimited field (`string`, `bytes`, nested message,
/// packed repeated) excluding the payload data itself.
///
/// Accounts for the field tag and length varint only. `length_bytes` defaults
/// to `kMaxSizeOfLength` (`kMaxVarint32SizeBytes`).
template <typename T>
constexpr size_t SizeOfDelimitedFieldWithoutValue(
    T field_number,
    uint32_t length_bytes = std::numeric_limits<uint32_t>::max()) {
  return TagSizeBytes(field_number) + varint::EncodedSize(length_bytes);
}

/// Calculates the total size of a delimited field (`string`, `bytes`, nested
/// message, packed repeated) including the payload data itself.
template <typename T>
constexpr size_t SizeOfDelimitedField(T field_number, uint32_t length_bytes) {
  return SizeOfDelimitedFieldWithoutValue(field_number, length_bytes) +
         length_bytes;
}

/// Calculates the size of a proto field in the wire format.
///
/// This is the size of a final serialized protobuf entry, including the key
/// (field number + wire type), encoded payload size (for length-delimited
/// types), and payload data.
///
/// @param[in] field_number The field number for the field.
/// @param[in] type The wire type (`WireType`) of the field.
/// @param[in] data_size_bytes The size of the payload data in bytes.
///
/// @returns The total encoded size of the field in bytes.
///
/// @pre `field_number` must be a valid field number
/// (`ValidFieldNumber(field_number)`).
/// @pre `data_size_bytes` must be smaller than or equal to
/// `std::numeric_limits<uint32_t>::max()`.
template <typename T>
constexpr size_t SizeOfField(T field_number,
                             WireType type,
                             size_t data_size_bytes) {
  if (type == WireType::kDelimited) {
    PW_DASSERT(data_size_bytes <= std::numeric_limits<uint32_t>::max());
    return SizeOfDelimitedField(field_number,
                                static_cast<uint32_t>(data_size_bytes));
  }
  return TagSizeBytes(field_number) + data_size_bytes;
}

/// Functions for calculating the worst-case serialized size of each protobuf
/// field type. Varint fields (`int32`, `uint64`, etc.) accept a value argument
/// that defaults to the largest-to-encode value for the type.
template <typename T>
constexpr size_t SizeOfFieldFloat(T field_number) {
  return TagSizeBytes(field_number) + sizeof(float);
}
template <typename T>
constexpr size_t SizeOfFieldDouble(T field_number) {
  return TagSizeBytes(field_number) + sizeof(double);
}
template <typename T>
constexpr size_t SizeOfFieldInt32(T field_number, int32_t value = -1) {
  return SizeOfVarintField(field_number, value);
}
template <typename T>
constexpr size_t SizeOfFieldInt64(T field_number, int64_t value = -1) {
  return SizeOfVarintField(field_number, value);
}
template <typename T>
constexpr size_t SizeOfFieldSint32(
    T field_number, int32_t value = std::numeric_limits<int32_t>::min()) {
  return SizeOfVarintField(field_number, varint::ZigZagEncode(value));
}
template <typename T>
constexpr size_t SizeOfFieldSint64(
    T field_number, int64_t value = std::numeric_limits<int64_t>::min()) {
  return SizeOfVarintField(field_number, varint::ZigZagEncode(value));
}
template <typename T>
constexpr size_t SizeOfFieldUint32(
    T field_number, uint32_t value = std::numeric_limits<uint32_t>::max()) {
  return SizeOfVarintField(field_number, value);
}
template <typename T>
constexpr size_t SizeOfFieldUint64(
    T field_number, uint64_t value = std::numeric_limits<uint64_t>::max()) {
  return SizeOfVarintField(field_number, value);
}
template <typename T>
constexpr size_t SizeOfFieldFixed32(T field_number) {
  return TagSizeBytes(field_number) + sizeof(uint32_t);
}
template <typename T>
constexpr size_t SizeOfFieldFixed64(T field_number) {
  return TagSizeBytes(field_number) + sizeof(uint64_t);
}
template <typename T>
constexpr size_t SizeOfFieldSfixed32(T field_number) {
  return TagSizeBytes(field_number) + sizeof(uint32_t);
}
template <typename T>
constexpr size_t SizeOfFieldSfixed64(T field_number) {
  return TagSizeBytes(field_number) + sizeof(uint64_t);
}
template <typename T>
constexpr size_t SizeOfFieldBool(T field_number) {
  return TagSizeBytes(field_number) + 1;
}
template <typename T>
constexpr size_t SizeOfFieldString(T field_number, uint32_t length_bytes) {
  return SizeOfDelimitedField(field_number, length_bytes);
}
template <typename T>
constexpr size_t SizeOfFieldBytes(T field_number, uint32_t length_bytes) {
  return SizeOfDelimitedField(field_number, length_bytes);
}
template <typename T, typename U = int32_t>
constexpr size_t SizeOfFieldEnum(T field_number, U value = static_cast<U>(-1)) {
  static_assert((std::is_enum<U>() || std::is_integral<U>()) &&
                    sizeof(U) <= sizeof(uint32_t),
                "Enum values must be 32-bit enums or integers");
  return SizeOfFieldInt32(field_number, static_cast<int32_t>(value));
}

/// @endmodule

}  // namespace pw::protobuf
