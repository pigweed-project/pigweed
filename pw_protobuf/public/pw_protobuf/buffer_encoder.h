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
#include <cstring>
#include <string_view>
#include <type_traits>

#include "pw_bytes/span.h"
#include "pw_protobuf/serialized_size.h"
#include "pw_protobuf/wire_format.h"
#include "pw_status/status.h"
#include "pw_varint/varint.h"

namespace pw::protobuf {

class BufferEncoder;

/// A lightweight, trivially copyable view of a BufferEncoder's state.
/// Passed by value to nested encoders to allow direct, zero-copy writes.
class BufferEncoderView {
 public:
  constexpr BufferEncoderView(ByteSpan buffer, size_t& offset, Status& status)
      : buffer_(buffer), offset_(offset), status_(status) {}

  /// Aggregated bounds check.
  /// Returns true if there is enough space for 'size' bytes.
  /// Sets status to ResourceExhausted on failure.
  bool EnsureSpace(size_t size) {
    if (!status_.ok())
      return false;
    if (offset_ + size > buffer_.size()) {
      status_ = Status::ResourceExhausted();
      return false;
    }
    return true;
  }

  // Low-level Unchecked write methods (assumes EnsureSpace was called)
  void UncheckedWriteVarint(uint64_t value) {
    offset_ +=
        pw::varint::EncodeLittleEndianBase128(value, buffer_.subspan(offset_));
  }

  void UncheckedWriteTag(uint32_t field_number, WireType wire_type) {
    UncheckedWriteVarint((field_number << 3) |
                         static_cast<uint32_t>(wire_type));
  }

  void UncheckedWriteBytes(const void* data, size_t size) {
    std::memcpy(buffer_.data() + offset_, data, size);
    offset_ += size;
  }

  /// Reserves 'size' bytes for a length prefix and returns the offset.
  /// Note: The reserved 'size' limits the maximum length of the nested message
  /// that can be patched later. A size of 2 (default) supports up to 16KB.
  /// If the nested message exceeds this limit, PatchLength will fail.
  size_t UncheckedReserveLength(size_t size = 2) {
    size_t len_offset = offset_;
    offset_ += size;
    return len_offset;
  }

  /// Patches the reserved length prefix in-place based on current offset.
  /// Sets status to ResourceExhausted if bounds are exceeded, or OutOfRange
  /// if the length doesn't fit in the reserved size.
  ///
  /// For sizes other than 1 or 2, this falls back to a generic padded varint
  /// encoding routine.
  template <size_t size = 2>
  void PatchLength(size_t len_offset) {
    if (!status_.ok()) {
      return;
    }

    if (len_offset + size > buffer_.size()) {
      status_ = Status::ResourceExhausted();
      return;
    }

    if (offset_ < len_offset + size) {
      status_ = Status::Internal();
      return;
    }

    // If the offset has exceeded the buffer size, we have already overflowed
    // the buffer during unchecked writes (causing UB/damage). However, we
    // must still set the status to ResourceExhausted here to ensure that
    // subsequent status checks (e.g. at the end of serialization) report the
    // error, preventing the corrupted buffer from being used.
    if (offset_ > buffer_.size()) {
      status_ = Status::ResourceExhausted();
      return;
    }

    size_t length = offset_ - (len_offset + size);

    static_assert(size <= pw::varint::kMaxVarint64SizeBytes,
                  "Varint size exceeds maximum supported size (10 bytes)");
    constexpr uint64_t max_value = pw::varint::MaxValueInBytes(size);

    if (length > max_value) {
      status_ = Status::OutOfRange();
      return;
    }

    if constexpr (size == 1) {
      buffer_[len_offset] = static_cast<std::byte>(length & 0x7F);
    } else if constexpr (size == 2) {
      // Fast 2-byte varint encoding (covers up to 16KB)
      buffer_[len_offset] = static_cast<std::byte>((length & 0x7F) | 0x80);
      buffer_[len_offset + 1] = static_cast<std::byte>((length >> 7) & 0x7F);
    } else {
      // Fall back to the generic padded varint encoding routine for size > 2.
      // We must pad the varint to exactly 'size' bytes to match the reserved
      // space and avoid copying the payload.
      EncodePaddedVarint(buffer_, len_offset, size, length);
    }
  }

  // Type-specific Checked and Unchecked write methods

  // Varint types
  void UncheckedWriteUint32(uint32_t field_number, uint32_t value) {
    UncheckedWriteTag(field_number, WireType::kVarint);
    UncheckedWriteVarint(value);
  }
  pw::Status WriteUint32(uint32_t field_number, uint32_t value) {
    if (EnsureSpace(TagSizeBytes(field_number) + kMaxSizeBytesUint32)) {
      UncheckedWriteUint32(field_number, value);
    }
    return status();
  }

  void UncheckedWriteUint64(uint32_t field_number, uint64_t value) {
    UncheckedWriteTag(field_number, WireType::kVarint);
    UncheckedWriteVarint(value);
  }
  pw::Status WriteUint64(uint32_t field_number, uint64_t value) {
    if (EnsureSpace(TagSizeBytes(field_number) + kMaxSizeBytesUint64)) {
      UncheckedWriteUint64(field_number, value);
    }
    return status();
  }

  void UncheckedWriteInt32(uint32_t field_number, int32_t value) {
    UncheckedWriteTag(field_number, WireType::kVarint);
    UncheckedWriteVarint(static_cast<uint64_t>(value));
  }
  pw::Status WriteInt32(uint32_t field_number, int32_t value) {
    if (EnsureSpace(TagSizeBytes(field_number) + kMaxSizeBytesInt32)) {
      UncheckedWriteInt32(field_number, value);
    }
    return status();
  }

  void UncheckedWriteInt64(uint32_t field_number, int64_t value) {
    UncheckedWriteTag(field_number, WireType::kVarint);
    UncheckedWriteVarint(static_cast<uint64_t>(value));
  }
  pw::Status WriteInt64(uint32_t field_number, int64_t value) {
    if (EnsureSpace(TagSizeBytes(field_number) + kMaxSizeBytesInt64)) {
      UncheckedWriteInt64(field_number, value);
    }
    return status();
  }

  void UncheckedWriteSint32(uint32_t field_number, int32_t value) {
    UncheckedWriteTag(field_number, WireType::kVarint);
    UncheckedWriteVarint(varint::ZigZagEncode(value));
  }
  pw::Status WriteSint32(uint32_t field_number, int32_t value) {
    if (EnsureSpace(TagSizeBytes(field_number) + kMaxSizeBytesSint32)) {
      UncheckedWriteSint32(field_number, value);
    }
    return status();
  }

  void UncheckedWriteSint64(uint32_t field_number, int64_t value) {
    UncheckedWriteTag(field_number, WireType::kVarint);
    UncheckedWriteVarint(varint::ZigZagEncode(value));
  }
  pw::Status WriteSint64(uint32_t field_number, int64_t value) {
    if (EnsureSpace(TagSizeBytes(field_number) + kMaxSizeBytesSint64)) {
      UncheckedWriteSint64(field_number, value);
    }
    return status();
  }

  void UncheckedWriteBool(uint32_t field_number, bool value) {
    UncheckedWriteTag(field_number, WireType::kVarint);
    UncheckedWriteVarint(value ? 1 : 0);
  }
  pw::Status WriteBool(uint32_t field_number, bool value) {
    if (EnsureSpace(TagSizeBytes(field_number) + kMaxSizeBytesBool)) {
      UncheckedWriteBool(field_number, value);
    }
    return status();
  }

  void UncheckedWriteEnum(uint32_t field_number, int32_t value) {
    UncheckedWriteInt32(field_number, value);
  }
  pw::Status WriteEnum(uint32_t field_number, int32_t value) {
    return WriteInt32(field_number, value);
  }

  template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
  void UncheckedWriteEnum(uint32_t field_number, T value) {
    UncheckedWriteInt32(field_number, static_cast<int32_t>(value));
  }

  template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
  pw::Status WriteEnum(uint32_t field_number, T value) {
    return WriteInt32(field_number, static_cast<int32_t>(value));
  }

  // Fixed32 types
  void UncheckedWriteFixed32(uint32_t field_number, uint32_t value) {
    UncheckedWriteTag(field_number, WireType::kFixed32);
    UncheckedWriteBytes(&value, sizeof(value));
  }
  pw::Status WriteFixed32(uint32_t field_number, uint32_t value) {
    if (EnsureSpace(TagSizeBytes(field_number) + kMaxSizeBytesFixed32)) {
      UncheckedWriteFixed32(field_number, value);
    }
    return status();
  }

  void UncheckedWriteSfixed32(uint32_t field_number, int32_t value) {
    UncheckedWriteTag(field_number, WireType::kFixed32);
    UncheckedWriteBytes(&value, sizeof(value));
  }
  pw::Status WriteSfixed32(uint32_t field_number, int32_t value) {
    if (EnsureSpace(TagSizeBytes(field_number) + kMaxSizeBytesSfixed32)) {
      UncheckedWriteSfixed32(field_number, value);
    }
    return status();
  }

  void UncheckedWriteFloat(uint32_t field_number, float value) {
    UncheckedWriteTag(field_number, WireType::kFixed32);
    UncheckedWriteBytes(&value, sizeof(value));
  }
  pw::Status WriteFloat(uint32_t field_number, float value) {
    if (EnsureSpace(TagSizeBytes(field_number) + kMaxSizeBytesFloat)) {
      UncheckedWriteFloat(field_number, value);
    }
    return status();
  }

  // Fixed64 types
  void UncheckedWriteFixed64(uint32_t field_number, uint64_t value) {
    UncheckedWriteTag(field_number, WireType::kFixed64);
    UncheckedWriteBytes(&value, sizeof(value));
  }
  pw::Status WriteFixed64(uint32_t field_number, uint64_t value) {
    if (EnsureSpace(TagSizeBytes(field_number) + kMaxSizeBytesFixed64)) {
      UncheckedWriteFixed64(field_number, value);
    }
    return status();
  }

  void UncheckedWriteSfixed64(uint32_t field_number, int64_t value) {
    UncheckedWriteTag(field_number, WireType::kFixed64);
    UncheckedWriteBytes(&value, sizeof(value));
  }
  pw::Status WriteSfixed64(uint32_t field_number, int64_t value) {
    if (EnsureSpace(TagSizeBytes(field_number) + kMaxSizeBytesSfixed64)) {
      UncheckedWriteSfixed64(field_number, value);
    }
    return status();
  }

  void UncheckedWriteDouble(uint32_t field_number, double value) {
    UncheckedWriteTag(field_number, WireType::kFixed64);
    UncheckedWriteBytes(&value, sizeof(value));
  }
  pw::Status WriteDouble(uint32_t field_number, double value) {
    if (EnsureSpace(TagSizeBytes(field_number) + kMaxSizeBytesDouble)) {
      UncheckedWriteDouble(field_number, value);
    }
    return status();
  }

  // Delimited types
  void UncheckedWriteBytes(uint32_t field_number, ConstByteSpan value) {
    UncheckedWriteTag(field_number, WireType::kDelimited);
    UncheckedWriteVarint(value.size());
    UncheckedWriteBytes(value.data(), value.size());
  }
  pw::Status WriteBytes(uint32_t field_number, ConstByteSpan value) {
    if (EnsureSpace(SizeOfDelimitedField(
            field_number, static_cast<uint32_t>(value.size())))) {
      UncheckedWriteBytes(field_number, value);
    }
    return status();
  }

  void UncheckedWriteString(uint32_t field_number, std::string_view value) {
    UncheckedWriteBytes(field_number, as_bytes(span(value)));
  }
  void UncheckedWriteString(uint32_t field_number,
                            const char* value,
                            size_t len) {
    UncheckedWriteBytes(field_number, as_bytes(span(value, len)));
  }
  pw::Status WriteString(uint32_t field_number, std::string_view value) {
    return WriteBytes(field_number, as_bytes(span(value)));
  }
  pw::Status WriteString(uint32_t field_number, const char* value, size_t len) {
    return WriteBytes(field_number, as_bytes(span(value, len)));
  }

  // Packed repeated varints helper
  template <typename T>
  void UncheckedWritePackedVarints(uint32_t field_number,
                                   span<const T> values,
                                   bool zigzag = false) {
    size_t payload_size = 0;
    for (T val : values) {
      uint64_t encoded_val;
      if constexpr (std::is_signed_v<T>) {
        encoded_val =
            zigzag ? varint::ZigZagEncode(val) : static_cast<uint64_t>(val);
      } else {
        encoded_val = static_cast<uint64_t>(val);
      }
      payload_size += varint::EncodedSize(encoded_val);
    }
    UncheckedWriteTag(field_number, WireType::kDelimited);
    UncheckedWriteVarint(payload_size);
    for (T val : values) {
      uint64_t encoded_val;
      if constexpr (std::is_signed_v<T>) {
        encoded_val =
            zigzag ? varint::ZigZagEncode(val) : static_cast<uint64_t>(val);
      } else {
        encoded_val = static_cast<uint64_t>(val);
      }
      UncheckedWriteVarint(encoded_val);
    }
  }

  template <typename T>
  pw::Status WritePackedVarints(uint32_t field_number,
                                span<const T> values,
                                bool zigzag = false) {
    size_t payload_size = 0;
    for (T val : values) {
      uint64_t encoded_val;
      if constexpr (std::is_signed_v<T>) {
        encoded_val =
            zigzag ? varint::ZigZagEncode(val) : static_cast<uint64_t>(val);
      } else {
        encoded_val = static_cast<uint64_t>(val);
      }
      payload_size += varint::EncodedSize(encoded_val);
    }
    if (EnsureSpace(SizeOfDelimitedField(
            field_number, static_cast<uint32_t>(payload_size)))) {
      UncheckedWritePackedVarints(field_number, values, zigzag);
    }
    return status();
  }

  // Packed fixed-size repeated helper
  void UncheckedWritePackedFixed(uint32_t field_number,
                                 span<const std::byte> values,
                                 [[maybe_unused]] size_t elem_size) {
    UncheckedWriteTag(field_number, WireType::kDelimited);
    UncheckedWriteVarint(values.size());
    UncheckedWriteBytes(values.data(), values.size());
  }
  pw::Status WritePackedFixed(uint32_t field_number,
                              span<const std::byte> values,
                              [[maybe_unused]] size_t elem_size) {
    if (EnsureSpace(SizeOfDelimitedField(
            field_number, static_cast<uint32_t>(values.size())))) {
      UncheckedWritePackedFixed(field_number, values, elem_size);
    }
    return status();
  }

  // Type-specific packed repeated methods
  void UncheckedWritePackedUint32(uint32_t field_number,
                                  span<const uint32_t> values) {
    UncheckedWritePackedVarints(field_number, values);
  }
  pw::Status WritePackedUint32(uint32_t field_number,
                               span<const uint32_t> values) {
    return WritePackedVarints(field_number, values);
  }

  void UncheckedWritePackedUint64(uint32_t field_number,
                                  span<const uint64_t> values) {
    UncheckedWritePackedVarints(field_number, values);
  }
  pw::Status WritePackedUint64(uint32_t field_number,
                               span<const uint64_t> values) {
    return WritePackedVarints(field_number, values);
  }

  void UncheckedWritePackedInt32(uint32_t field_number,
                                 span<const int32_t> values) {
    UncheckedWritePackedVarints(field_number, values);
  }
  pw::Status WritePackedInt32(uint32_t field_number,
                              span<const int32_t> values) {
    return WritePackedVarints(field_number, values);
  }

  void UncheckedWritePackedInt64(uint32_t field_number,
                                 span<const int64_t> values) {
    UncheckedWritePackedVarints(field_number, values);
  }
  pw::Status WritePackedInt64(uint32_t field_number,
                              span<const int64_t> values) {
    return WritePackedVarints(field_number, values);
  }

  void UncheckedWritePackedSint32(uint32_t field_number,
                                  span<const int32_t> values) {
    UncheckedWritePackedVarints(field_number, values, /*zigzag=*/true);
  }
  pw::Status WritePackedSint32(uint32_t field_number,
                               span<const int32_t> values) {
    return WritePackedVarints(field_number, values, /*zigzag=*/true);
  }

  void UncheckedWritePackedSint64(uint32_t field_number,
                                  span<const int64_t> values) {
    UncheckedWritePackedVarints(field_number, values, /*zigzag=*/true);
  }
  pw::Status WritePackedSint64(uint32_t field_number,
                               span<const int64_t> values) {
    return WritePackedVarints(field_number, values, /*zigzag=*/true);
  }

  void UncheckedWritePackedBool(uint32_t field_number,
                                span<const bool> values) {
    UncheckedWritePackedVarints(field_number, values);
  }
  pw::Status WritePackedBool(uint32_t field_number, span<const bool> values) {
    return WritePackedVarints(field_number, values);
  }

  void UncheckedWritePackedEnum(uint32_t field_number,
                                span<const int32_t> values) {
    UncheckedWritePackedVarints(field_number, values);
  }
  pw::Status WritePackedEnum(uint32_t field_number,
                             span<const int32_t> values) {
    return WritePackedVarints(field_number, values);
  }

  template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
  void UncheckedWritePackedEnum(uint32_t field_number, span<const T> values) {
    static_assert(sizeof(T) == sizeof(int32_t),
                  "Protobuf enums are always 4-byte integers");
    UncheckedWritePackedVarints(
        field_number,
        span<const int32_t>(reinterpret_cast<const int32_t*>(values.data()),
                            values.size()));
  }

  template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
  pw::Status WritePackedEnum(uint32_t field_number, span<const T> values) {
    static_assert(sizeof(T) == sizeof(int32_t),
                  "Protobuf enums are always 4-byte integers");
    return WritePackedVarints(
        field_number,
        span<const int32_t>(reinterpret_cast<const int32_t*>(values.data()),
                            values.size()));
  }

  void UncheckedWritePackedFixed32(uint32_t field_number,
                                   span<const uint32_t> values) {
    UncheckedWritePackedFixed(field_number, as_bytes(values), sizeof(uint32_t));
  }
  pw::Status WritePackedFixed32(uint32_t field_number,
                                span<const uint32_t> values) {
    return WritePackedFixed(field_number, as_bytes(values), sizeof(uint32_t));
  }

  void UncheckedWritePackedSfixed32(uint32_t field_number,
                                    span<const int32_t> values) {
    UncheckedWritePackedFixed(field_number, as_bytes(values), sizeof(int32_t));
  }
  pw::Status WritePackedSfixed32(uint32_t field_number,
                                 span<const int32_t> values) {
    return WritePackedFixed(field_number, as_bytes(values), sizeof(int32_t));
  }

  void UncheckedWritePackedFloat(uint32_t field_number,
                                 span<const float> values) {
    UncheckedWritePackedFixed(field_number, as_bytes(values), sizeof(float));
  }
  pw::Status WritePackedFloat(uint32_t field_number, span<const float> values) {
    return WritePackedFixed(field_number, as_bytes(values), sizeof(float));
  }

  void UncheckedWritePackedFixed64(uint32_t field_number,
                                   span<const uint64_t> values) {
    UncheckedWritePackedFixed(field_number, as_bytes(values), sizeof(uint64_t));
  }
  pw::Status WritePackedFixed64(uint32_t field_number,
                                span<const uint64_t> values) {
    return WritePackedFixed(field_number, as_bytes(values), sizeof(uint64_t));
  }

  void UncheckedWritePackedSfixed64(uint32_t field_number,
                                    span<const int64_t> values) {
    UncheckedWritePackedFixed(field_number, as_bytes(values), sizeof(int64_t));
  }
  pw::Status WritePackedSfixed64(uint32_t field_number,
                                 span<const int64_t> values) {
    return WritePackedFixed(field_number, as_bytes(values), sizeof(int64_t));
  }

  void UncheckedWritePackedDouble(uint32_t field_number,
                                  span<const double> values) {
    UncheckedWritePackedFixed(field_number, as_bytes(values), sizeof(double));
  }
  pw::Status WritePackedDouble(uint32_t field_number,
                               span<const double> values) {
    return WritePackedFixed(field_number, as_bytes(values), sizeof(double));
  }

  // Vector/Repeated support helpers (templated to avoid header dependencies)
  template <typename VectorType>
  void UncheckedWriteRepeatedUint32(uint32_t field_number,
                                    const VectorType& values) {
    UncheckedWritePackedUint32(field_number,
                               span(values.data(), values.size()));
  }
  template <typename VectorType>
  pw::Status WriteRepeatedUint32(uint32_t field_number,
                                 const VectorType& values) {
    return WritePackedUint32(field_number, span(values.data(), values.size()));
  }

  template <typename VectorType>
  void UncheckedWriteRepeatedUint64(uint32_t field_number,
                                    const VectorType& values) {
    UncheckedWritePackedUint64(field_number,
                               span(values.data(), values.size()));
  }
  template <typename VectorType>
  pw::Status WriteRepeatedUint64(uint32_t field_number,
                                 const VectorType& values) {
    return WritePackedUint64(field_number, span(values.data(), values.size()));
  }

  template <typename VectorType>
  void UncheckedWriteRepeatedInt32(uint32_t field_number,
                                   const VectorType& values) {
    UncheckedWritePackedInt32(field_number, span(values.data(), values.size()));
  }
  template <typename VectorType>
  pw::Status WriteRepeatedInt32(uint32_t field_number,
                                const VectorType& values) {
    return WritePackedInt32(field_number, span(values.data(), values.size()));
  }

  template <typename VectorType>
  void UncheckedWriteRepeatedInt64(uint32_t field_number,
                                   const VectorType& values) {
    UncheckedWritePackedInt64(field_number, span(values.data(), values.size()));
  }
  template <typename VectorType>
  pw::Status WriteRepeatedInt64(uint32_t field_number,
                                const VectorType& values) {
    return WritePackedInt64(field_number, span(values.data(), values.size()));
  }

  template <typename VectorType>
  void UncheckedWriteRepeatedSint32(uint32_t field_number,
                                    const VectorType& values) {
    UncheckedWritePackedSint32(field_number,
                               span(values.data(), values.size()));
  }
  template <typename VectorType>
  pw::Status WriteRepeatedSint32(uint32_t field_number,
                                 const VectorType& values) {
    return WritePackedSint32(field_number, span(values.data(), values.size()));
  }

  template <typename VectorType>
  void UncheckedWriteRepeatedSint64(uint32_t field_number,
                                    const VectorType& values) {
    UncheckedWritePackedSint64(field_number,
                               span(values.data(), values.size()));
  }
  template <typename VectorType>
  pw::Status WriteRepeatedSint64(uint32_t field_number,
                                 const VectorType& values) {
    return WritePackedSint64(field_number, span(values.data(), values.size()));
  }

  template <typename VectorType>
  void UncheckedWriteRepeatedBool(uint32_t field_number,
                                  const VectorType& values) {
    UncheckedWritePackedBool(field_number, span(values.data(), values.size()));
  }
  template <typename VectorType>
  pw::Status WriteRepeatedBool(uint32_t field_number,
                               const VectorType& values) {
    return WritePackedBool(field_number, span(values.data(), values.size()));
  }

  template <typename VectorType>
  void UncheckedWriteRepeatedEnum(uint32_t field_number,
                                  const VectorType& values) {
    // Cast the vector memory to int32_t since enums are underlying ints
    UncheckedWritePackedEnum(
        field_number,
        span(reinterpret_cast<const int32_t*>(values.data()), values.size()));
  }
  template <typename VectorType>
  pw::Status WriteRepeatedEnum(uint32_t field_number,
                               const VectorType& values) {
    return WritePackedEnum(
        field_number,
        span(reinterpret_cast<const int32_t*>(values.data()), values.size()));
  }

  template <typename VectorType>
  void UncheckedWriteRepeatedFixed32(uint32_t field_number,
                                     const VectorType& values) {
    UncheckedWritePackedFixed32(field_number,
                                span(values.data(), values.size()));
  }
  template <typename VectorType>
  pw::Status WriteRepeatedFixed32(uint32_t field_number,
                                  const VectorType& values) {
    return WritePackedFixed32(field_number, span(values.data(), values.size()));
  }

  template <typename VectorType>
  void UncheckedWriteRepeatedSfixed32(uint32_t field_number,
                                      const VectorType& values) {
    UncheckedWritePackedSfixed32(field_number,
                                 span(values.data(), values.size()));
  }
  template <typename VectorType>
  pw::Status WriteRepeatedSfixed32(uint32_t field_number,
                                   const VectorType& values) {
    return WritePackedSfixed32(field_number,
                               span(values.data(), values.size()));
  }

  template <typename VectorType>
  void UncheckedWriteRepeatedFloat(uint32_t field_number,
                                   const VectorType& values) {
    UncheckedWritePackedFloat(field_number, span(values.data(), values.size()));
  }
  template <typename VectorType>
  pw::Status WriteRepeatedFloat(uint32_t field_number,
                                const VectorType& values) {
    return WritePackedFloat(field_number, span(values.data(), values.size()));
  }

  template <typename VectorType>
  void UncheckedWriteRepeatedFixed64(uint32_t field_number,
                                     const VectorType& values) {
    UncheckedWritePackedFixed64(field_number,
                                span(values.data(), values.size()));
  }
  template <typename VectorType>
  pw::Status WriteRepeatedFixed64(uint32_t field_number,
                                  const VectorType& values) {
    return WritePackedFixed64(field_number, span(values.data(), values.size()));
  }

  template <typename VectorType>
  void UncheckedWriteRepeatedSfixed64(uint32_t field_number,
                                      const VectorType& values) {
    UncheckedWritePackedSfixed64(field_number,
                                 span(values.data(), values.size()));
  }
  template <typename VectorType>
  pw::Status WriteRepeatedSfixed64(uint32_t field_number,
                                   const VectorType& values) {
    return WritePackedSfixed64(field_number,
                               span(values.data(), values.size()));
  }

  template <typename VectorType>
  void UncheckedWriteRepeatedDouble(uint32_t field_number,
                                    const VectorType& values) {
    UncheckedWritePackedDouble(field_number,
                               span(values.data(), values.size()));
  }
  template <typename VectorType>
  pw::Status WriteRepeatedDouble(uint32_t field_number,
                                 const VectorType& values) {
    return WritePackedDouble(field_number, span(values.data(), values.size()));
  }

  Status status() const { return status_; }
  size_t offset() const { return offset_; }
  ByteSpan buffer() const { return buffer_; }

  size_t size() const { return offset_; }
  operator ConstByteSpan() const { return buffer_.first(offset_); }
  operator ByteSpan() { return buffer_.first(offset_); }

 private:
  // Encodes an unsigned integer as a varint of a fixed size, padding it with
  // continuation bytes (0x80) if the value is smaller than the reserved size.
  // This ensures the encoded varint occupies exactly 'size' bytes, which is
  // required to avoid shifting the payload that was already written after it.
  static void EncodePaddedVarint(ByteSpan buffer,
                                 size_t offset,
                                 size_t size,
                                 uint64_t value) {
    uint64_t temp = value;
    for (size_t i = 0; i < size; ++i) {
      if (i == size - 1) {
        buffer[offset + i] = static_cast<std::byte>(temp & 0x7F);
      } else {
        buffer[offset + i] = static_cast<std::byte>((temp & 0x7F) | 0x80);
        temp >>= 7;
      }
    }
  }

  ByteSpan buffer_;
  size_t& offset_;
  Status& status_;
};

/// The root encoder that owns the buffer, offset, and status.
class BufferEncoder : public BufferEncoderView {
 public:
  constexpr BufferEncoder(ByteSpan buffer)
      : BufferEncoderView(buffer, offset_impl_, status_impl_),
        offset_impl_(0),
        status_impl_(OkStatus()) {}

  // Disallow copy/assign to avoid confusion about who owns the buffer and
  // to prevent dangerous reference copying.
  BufferEncoder(const BufferEncoder&) = delete;
  BufferEncoder& operator=(const BufferEncoder&) = delete;

  BufferEncoderView view() { return *this; }

 private:
  size_t offset_impl_;
  Status status_impl_;
};

}  // namespace pw::protobuf
