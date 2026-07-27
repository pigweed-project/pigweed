// Copyright 2021 The Pigweed Authors
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
#include <limits>
#include <type_traits>

#include "pw_assert/assert.h"
#include "pw_containers/vector.h"
#include "pw_protobuf/internal/codegen.h"
#include "pw_protobuf/wire_format.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"
#include "pw_stream/stream.h"
#include "pw_varint/stream.h"
#include "pw_varint/varint.h"

namespace pw::protobuf {

/// @module{pw_protobuf}

/// A low-level, event-based protobuf wire format decoder that operates on a
/// stream.
///
/// The decoder processes an encoded message by iterating over its fields using
/// `Next()`. The caller can then check `FieldNumber()` and extract the values
/// of fields using the `Read*()` methods.
///
/// While individual read calls return `pw::Result`, `pw::StatusWithSize`, or
/// `pw::Status` objects, the decoder tracks all status returns and latches onto
/// the first error encountered. This status can be accessed via
/// `StreamDecoder::status()`.
///
/// In the case of errors during reading, decoding stops and returns with the
/// cursor on the field that caused the error. Unknown fields in the wire
/// encoding are skipped automatically during iteration.
///
/// @note This decoder does not provide in-memory data structures representing a
/// protobuf message and is intended for messages too large to fit in memory or
/// where streaming is required. For smaller messages where the complete data is
/// available in memory, prefer `MemoryDecoder` (`Decoder`), which avoids stream
/// overhead.
///
/// @code
///   stream::Reader& my_stream = GetProtoStream();
///   StreamDecoder decoder(my_stream);
///
///   while (decoder.Next().ok()) {
///     // FieldNumber() will always be valid if Next() returns OK.
///     switch (decoder.FieldNumber().value()) {
///       case 1:
///         Result<uint32_t> result = decoder.ReadUint32();
///         if (result.ok()) {
///           DoSomething(result.value());
///         }
///         break;
///       // ... and other fields.
///     }
///   }
/// @endcode
class StreamDecoder {
 public:
  /// `stream::Reader` for a `bytes` (or `string`) field in a streamed proto
  /// message.
  ///
  /// Shares the `StreamDecoder`'s reader, limiting it to the bounds of the
  /// field. If the `StreamDecoder`'s reader does not support seeking,
  /// `BytesReader` will also not support seeking.
  ///
  /// @warning When a `BytesReader` is active, any use of the parent
  /// `StreamDecoder` that created it will trigger a crash. To resume using the
  /// parent decoder, destroy the `BytesReader` first.
  class BytesReader : public stream::RelativeSeekableReader {
   public:
    ~BytesReader() override { decoder_.CloseBytesReader(*this); }

    constexpr size_t field_size() const { return end_offset_ - start_offset_; }

   private:
    friend class StreamDecoder;

    constexpr BytesReader(StreamDecoder& decoder,
                          size_t start_offset,
                          size_t end_offset)
        : decoder_(decoder),
          start_offset_(start_offset),
          end_offset_(end_offset),
          status_(OkStatus()) {}

    constexpr BytesReader(StreamDecoder& decoder, Status status)
        : decoder_(decoder),
          start_offset_(0),
          end_offset_(0),
          status_(status) {}

    StatusWithSize DoRead(ByteSpan destination) final;
    Status DoSeek(ptrdiff_t offset, Whence origin) final;

    StreamDecoder& decoder_;
    size_t start_offset_;
    size_t end_offset_;
    Status status_;
  };

  /// Constructs a `StreamDecoder` operating on `reader` with unbounded length.
  ///
  /// @param[in] reader Source stream reader containing serialized protobuf
  /// data.
  constexpr StreamDecoder(stream::Reader& reader)
      : StreamDecoder(reader, std::numeric_limits<size_t>::max()) {}

  /// Constructs a `StreamDecoder` with a specified maximum `length`.
  ///
  /// Where the length of the protobuf message is known in advance, the decoder
  /// can be prevented from reading from the stream beyond the known bounds by
  /// specifying the length. When a decoder constructed in this way goes out of
  /// scope, it automatically consumes any remaining bytes up to `length`,
  /// allowing the next `Read()` on the stream to start after the protobuf even
  /// if it was not fully parsed.
  ///
  /// @param[in] reader Source stream reader containing serialized protobuf
  /// data.
  /// @param[in] length Maximum number of bytes belonging to this protobuf
  /// message.
  constexpr StreamDecoder(stream::Reader& reader, size_t length)
      : reader_(reader),
        stream_bounds_({0, length}),
        position_(0),
        current_field_(kInitialFieldKey),
        delimited_field_size_(0),
        delimited_field_offset_(0),
        parent_(nullptr),
        field_consumed_(true),
        nested_reader_open_(false),
        status_(OkStatus()) {}

  StreamDecoder(const StreamDecoder& other) = delete;
  StreamDecoder& operator=(const StreamDecoder& other) = delete;

  ~StreamDecoder();

  /// Advances to the next field in the proto.
  ///
  /// If `Next()` returns `@OK`, there is guaranteed to be a valid protobuf
  /// field at the current position, which can then be consumed through one of
  /// the `Read*()` methods.
  ///
  /// @returns
  /// * @OK: Advanced to a valid proto field.
  /// * @OUT_OF_RANGE: Reached the end of the proto message.
  /// * @DATA_LOSS: Encountered invalid protobuf wire data.
  /// * Other errors encountered while reading from the underlying stream.
  Status Next();

  /// Returns the field number of the current field.
  ///
  /// @returns @Result{the field number of the current field}
  /// * @FAILED_PRECONDITION: The current field has already been consumed or
  ///   `Next()` has not been called successfully.
  /// * Other error statuses latched by the decoder.
  ///
  /// @pre Must only be called after a successful call to `Next()` and before
  /// any `Read*()` operation.
  constexpr Result<uint32_t> FieldNumber() const {
    if (field_consumed_) {
      return Status::FailedPrecondition();
    }

    return status_.ok() ? current_field_.field_number()
                        : Result<uint32_t>(status_);
  }

  //
  // TODO(frolv): Add Status Read*(T& value) APIs alongside the Result<T> ones.
  //

  /// Reads a proto `int32` value from the current position.
  Result<int32_t> ReadInt32() {
    return ReadVarintField<int32_t>(internal::VarintType::kNormal);
  }

  /// Reads repeated `int32` values from the current position using packed
  /// encoding into the provided span.
  ///
  /// @param[out] out Destination span for read values.
  /// @returns Status with the number of values successfully read.
  StatusWithSize ReadPackedInt32(span<int32_t> out) {
    return ReadPackedVarintField(
        as_writable_bytes(out), sizeof(int32_t), internal::VarintType::kNormal);
  }

  /// Reads repeated `int32` values from the current position into the vector,
  /// supporting either repeated single field elements or packed encoding.
  ///
  /// @param[out] out Vector where values will be appended.
  Status ReadRepeatedInt32(pw::Vector<int32_t>& out) {
    return ReadRepeatedVarintField<int32_t>(out, internal::VarintType::kNormal);
  }

  /// Reads a proto `uint32` value from the current position.
  Result<uint32_t> ReadUint32() {
    return ReadVarintField<uint32_t>(internal::VarintType::kUnsigned);
  }

  /// Reads repeated `uint32` values from the current position using packed
  /// encoding into the provided span.
  ///
  /// @param[out] out Destination span for read values.
  /// @returns Status with the number of values successfully read.
  StatusWithSize ReadPackedUint32(span<uint32_t> out) {
    return ReadPackedVarintField(as_writable_bytes(out),
                                 sizeof(uint32_t),
                                 internal::VarintType::kUnsigned);
  }

  /// Reads repeated `uint32` values from the current position into the vector,
  /// supporting either repeated single field elements or packed encoding.
  ///
  /// @param[out] out Vector where values will be appended.
  Status ReadRepeatedUint32(pw::Vector<uint32_t>& out) {
    return ReadRepeatedVarintField<uint32_t>(out,
                                             internal::VarintType::kUnsigned);
  }

  /// Reads repeated enum values from the current position using packed
  /// encoding into the provided span.
  ///
  /// @param[out] out Destination span for read values.
  /// @returns Status with the number of values successfully read.
  template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
  StatusWithSize ReadPackedEnum(span<T> out) {
    static_assert(sizeof(T) == sizeof(int32_t),
                  "Protobuf enums are always 4-byte integers");
    return ReadPackedVarintField(
        as_writable_bytes(out), sizeof(T), internal::VarintType::kUnsigned);
  }

  /// Reads repeated enum values from the current position into the vector,
  /// supporting either repeated single field elements or packed encoding.
  ///
  /// @param[out] out Vector where values will be appended.
  template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
  Status ReadRepeatedEnum(pw::Vector<T>& out) {
    static_assert(sizeof(T) == sizeof(int32_t),
                  "Protobuf enums are always 4-byte integers");
    if (out.full()) {
      return Status::ResourceExhausted();
    }
    const size_t old_size = out.size();
    out.resize(out.capacity());
    size_t size = old_size;
    Status status =
        ReadRepeatedVarintFieldGeneric(reinterpret_cast<std::byte*>(out.data()),
                                       out.capacity(),
                                       size,
                                       sizeof(T),
                                       internal::VarintType::kUnsigned);
    out.resize(size);
    return status;
  }

  /// Reads a proto `int64` value from the current position.
  Result<int64_t> ReadInt64() {
    return ReadVarintField<int64_t>(internal::VarintType::kNormal);
  }

  /// Reads repeated `int64` values from the current position using packed
  /// encoding into the provided span.
  ///
  /// @param[out] out Destination span for read values.
  /// @returns Status with the number of values successfully read.
  StatusWithSize ReadPackedInt64(span<int64_t> out) {
    return ReadPackedVarintField(
        as_writable_bytes(out), sizeof(int64_t), internal::VarintType::kNormal);
  }

  /// Reads repeated `int64` values from the current position into the vector,
  /// supporting either repeated single field elements or packed encoding.
  ///
  /// @param[out] out Vector where values will be appended.
  Status ReadRepeatedInt64(pw::Vector<int64_t>& out) {
    return ReadRepeatedVarintField<int64_t>(out, internal::VarintType::kNormal);
  }

  /// Reads a proto `uint64` value from the current position.
  Result<uint64_t> ReadUint64() {
    return ReadVarintField<uint64_t>(internal::VarintType::kUnsigned);
  }

  /// Reads repeated `uint64` values from the current position using packed
  /// encoding into the provided span.
  ///
  /// @param[out] out Destination span for read values.
  /// @returns Status with the number of values successfully read.
  StatusWithSize ReadPackedUint64(span<uint64_t> out) {
    return ReadPackedVarintField(as_writable_bytes(out),
                                 sizeof(uint64_t),
                                 internal::VarintType::kUnsigned);
  }

  /// Reads repeated `uint64` values from the current position into the vector,
  /// supporting either repeated single field elements or packed encoding.
  ///
  /// @param[out] out Vector where values will be appended.
  Status ReadRepeatedUint64(pw::Vector<uint64_t>& out) {
    return ReadRepeatedVarintField<uint64_t>(out,
                                             internal::VarintType::kUnsigned);
  }

  /// Reads a proto `sint32` value from the current position.
  Result<int32_t> ReadSint32() {
    return ReadVarintField<int32_t>(internal::VarintType::kZigZag);
  }

  /// Reads repeated `sint32` values from the current position using packed
  /// encoding into the provided span.
  ///
  /// @param[out] out Destination span for read values.
  /// @returns Status with the number of values successfully read.
  StatusWithSize ReadPackedSint32(span<int32_t> out) {
    return ReadPackedVarintField(
        as_writable_bytes(out), sizeof(int32_t), internal::VarintType::kZigZag);
  }

  /// Reads repeated `sint32` values from the current position into the vector,
  /// supporting either repeated single field elements or packed encoding.
  ///
  /// @param[out] out Vector where values will be appended.
  Status ReadRepeatedSint32(pw::Vector<int32_t>& out) {
    return ReadRepeatedVarintField<int32_t>(out, internal::VarintType::kZigZag);
  }

  /// Reads a proto `sint64` value from the current position.
  Result<int64_t> ReadSint64() {
    return ReadVarintField<int64_t>(internal::VarintType::kZigZag);
  }

  /// Reads repeated `sint64` values from the current position using packed
  /// encoding into the provided span.
  ///
  /// @param[out] out Destination span for read values.
  /// @returns Status with the number of values successfully read.
  StatusWithSize ReadPackedSint64(span<int64_t> out) {
    return ReadPackedVarintField(
        as_writable_bytes(out), sizeof(int64_t), internal::VarintType::kZigZag);
  }

  /// Reads repeated `sint64` values from the current position into the vector,
  /// supporting either repeated single field elements or packed encoding.
  ///
  /// @param[out] out Vector where values will be appended.
  Status ReadRepeatedSint64(pw::Vector<int64_t>& out) {
    return ReadRepeatedVarintField<int64_t>(out, internal::VarintType::kZigZag);
  }

  /// Reads a proto `bool` value from the current position.
  Result<bool> ReadBool() {
    return ReadVarintField<bool>(internal::VarintType::kUnsigned);
  }

  /// Reads repeated `bool` values from the current position using packed
  /// encoding into the provided span.
  ///
  /// @param[out] out Destination span for read values.
  /// @returns Status with the number of values successfully read.
  StatusWithSize ReadPackedBool(span<bool> out) {
    return ReadPackedVarintField(
        as_writable_bytes(out), sizeof(bool), internal::VarintType::kUnsigned);
  }

  /// Reads repeated `bool` values from the current position into the vector,
  /// supporting either repeated single field elements or packed encoding.
  ///
  /// @param[out] out Vector where values will be appended.
  Status ReadRepeatedBool(pw::Vector<bool>& out) {
    return ReadRepeatedVarintField<bool>(out, internal::VarintType::kUnsigned);
  }

  /// Reads a proto `fixed32` value from the current position.
  Result<uint32_t> ReadFixed32() { return ReadFixedField<uint32_t>(); }

  /// Reads repeated `fixed32` values from the current position using packed
  /// encoding into the provided span.
  ///
  /// @param[out] out Destination span for read values.
  /// @returns Status with the number of values successfully read.
  StatusWithSize ReadPackedFixed32(span<uint32_t> out) {
    return ReadPackedFixedField(as_writable_bytes(out), sizeof(uint32_t));
  }

  /// Reads repeated `fixed32` values from the current position into the vector,
  /// supporting either repeated single field elements or packed encoding.
  ///
  /// @param[out] out Vector where values will be appended.
  Status ReadRepeatedFixed32(pw::Vector<uint32_t>& out) {
    return ReadRepeatedFixedField<uint32_t>(out);
  }

  /// Reads a proto `fixed64` value from the current position.
  Result<uint64_t> ReadFixed64() { return ReadFixedField<uint64_t>(); }

  /// Reads repeated `fixed64` values from the current position using packed
  /// encoding into the provided span.
  ///
  /// @param[out] out Destination span for read values.
  /// @returns Status with the number of values successfully read.
  StatusWithSize ReadPackedFixed64(span<uint64_t> out) {
    return ReadPackedFixedField(as_writable_bytes(out), sizeof(uint64_t));
  }

  /// Reads repeated `fixed64` values from the current position into the vector,
  /// supporting either repeated single field elements or packed encoding.
  ///
  /// @param[out] out Vector where values will be appended.
  Status ReadRepeatedFixed64(pw::Vector<uint64_t>& out) {
    return ReadRepeatedFixedField<uint64_t>(out);
  }

  /// Reads a proto `sfixed32` value from the current position.
  Result<int32_t> ReadSfixed32() { return ReadFixedField<int32_t>(); }

  /// Reads repeated `sfixed32` values from the current position using packed
  /// encoding into the provided span.
  ///
  /// @param[out] out Destination span for read values.
  /// @returns Status with the number of values successfully read.
  StatusWithSize ReadPackedSfixed32(span<int32_t> out) {
    return ReadPackedFixedField(as_writable_bytes(out), sizeof(int32_t));
  }

  /// Reads repeated `sfixed32` values from the current position into the
  /// vector, supporting either repeated single field elements or packed
  /// encoding.
  ///
  /// @param[out] out Vector where values will be appended.
  Status ReadRepeatedSfixed32(pw::Vector<int32_t>& out) {
    return ReadRepeatedFixedField<int32_t>(out);
  }

  /// Reads a proto `sfixed64` value from the current position.
  Result<int64_t> ReadSfixed64() { return ReadFixedField<int64_t>(); }

  /// Reads repeated `sfixed64` values from the current position using packed
  /// encoding into the provided span.
  ///
  /// @param[out] out Destination span for read values.
  /// @returns Status with the number of values successfully read.
  StatusWithSize ReadPackedSfixed64(span<int64_t> out) {
    return ReadPackedFixedField(as_writable_bytes(out), sizeof(int64_t));
  }

  /// Reads repeated `sfixed64` values from the current position into the
  /// vector, supporting either repeated single field elements or packed
  /// encoding.
  ///
  /// @param[out] out Vector where values will be appended.
  Status ReadRepeatedSfixed64(pw::Vector<int64_t>& out) {
    return ReadRepeatedFixedField<int64_t>(out);
  }

  /// Reads a proto `float` value from the current position.
  Result<float> ReadFloat() {
    static_assert(sizeof(float) == sizeof(uint32_t),
                  "Float and uint32_t must be the same size for protobufs");
    return ReadFixedField<float>();
  }

  /// Reads repeated `float` values from the current position using packed
  /// encoding into the provided span.
  ///
  /// @param[out] out Destination span for read values.
  /// @returns Status with the number of values successfully read.
  StatusWithSize ReadPackedFloat(span<float> out) {
    static_assert(sizeof(float) == sizeof(uint32_t),
                  "Float and uint32_t must be the same size for protobufs");
    return ReadPackedFixedField(as_writable_bytes(out), sizeof(float));
  }

  /// Reads repeated `float` values from the current position into the vector,
  /// supporting either repeated single field elements or packed encoding.
  ///
  /// @param[out] out Vector where values will be appended.
  Status ReadRepeatedFloat(pw::Vector<float>& out) {
    return ReadRepeatedFixedField<float>(out);
  }

  /// Reads a proto `double` value from the current position.
  Result<double> ReadDouble() {
    static_assert(sizeof(double) == sizeof(uint64_t),
                  "Double and uint64_t must be the same size for protobufs");
    return ReadFixedField<double>();
  }

  /// Reads repeated `double` values from the current position using packed
  /// encoding into the provided span.
  ///
  /// @param[out] out Destination span for read values.
  /// @returns Status with the number of values successfully read.
  StatusWithSize ReadPackedDouble(span<double> out) {
    static_assert(sizeof(double) == sizeof(uint64_t),
                  "Double and uint64_t must be the same size for protobufs");
    return ReadPackedFixedField(as_writable_bytes(out), sizeof(double));
  }

  /// Reads repeated `double` values from the current position into the vector,
  /// supporting either repeated single field elements or packed encoding.
  ///
  /// @param[out] out Vector where values will be appended.
  Status ReadRepeatedDouble(pw::Vector<double>& out) {
    return ReadRepeatedFixedField<double>(out);
  }

  /// Reads a proto `string` value from the current position into the provided
  /// span.
  ///
  /// The string is copied into the provided buffer and the read size is
  /// returned. Since the span is updated with the size of the string, the
  /// string is NOT automatically null-terminated; this should be done manually
  /// if desired. `pw_string` provides utility methods to copy string data from
  /// spans into other targets.
  ///
  /// @param[out] out Destination span for the string data.
  ///
  /// @returns
  /// * @OK with the number of bytes read: String successfully read.
  /// * @RESOURCE_EXHAUSTED with 0 bytes: The buffer is too small to fit the
  ///   string value. No data is read, and the decoder's position remains on the
  ///   string field.
  StatusWithSize ReadString(span<char> out) {
    return ReadBytes(as_writable_bytes(out));
  }

  /// Reads a proto `bytes` value from the current position into the provided
  /// span.
  ///
  /// The value is copied into the provided buffer and the read size is
  /// returned. For larger bytes values that won't fit into memory, use
  /// `GetBytesReader()` to acquire a `stream::Reader` to the bytes instead.
  ///
  /// @param[out] out Destination span for the bytes data.
  ///
  /// @returns
  /// * @OK with the number of bytes read: Bytes successfully read.
  /// * @RESOURCE_EXHAUSTED with 0 bytes: The buffer is too small to fit the
  ///   bytes value. No data is read, and the decoder's position remains on the
  ///   bytes field.
  StatusWithSize ReadBytes(span<std::byte> out) {
    return ReadDelimitedField(out);
  }

  /// Returns a `stream::Reader` (`BytesReader`) for accessing a `bytes` (or
  /// `string`) field as a stream.
  ///
  /// The `BytesReader` shares the same stream as the decoder, using RAII to
  /// manage ownership of the stream.
  ///
  /// @warning When a `BytesReader` is active, any use of the parent
  /// `StreamDecoder` that created it will trigger a crash. To resume using the
  /// parent decoder, destroy the `BytesReader` first.
  ///
  /// @code
  ///   StreamDecoder decoder(my_stream);
  ///
  ///   while (decoder.Next().ok()) {
  ///     switch (decoder.FieldNumber().value()) {
  ///       case 1: {
  ///         // The BytesReader is created within a new C++ scope. While it is
  ///         // alive, the decoder cannot be used.
  ///         StreamDecoder::BytesReader reader = decoder.GetBytesReader();
  ///         reader.Read(some_buffer);
  ///         break;
  ///       }
  ///     }
  ///   }
  /// @endcode
  ///
  /// @returns A `BytesReader` stream reader targeting the current field. The
  /// reader supports seeking if the underlying `StreamDecoder` stream supports
  /// seeking.
  BytesReader GetBytesReader();

  /// Returns a decoder for a nested protobuf message located at the current
  /// position.
  ///
  /// The nested decoder shares the same stream as its parent, using RAII to
  /// manage ownership of the stream.
  ///
  /// @warning When a nested submessage is being decoded, any use of the parent
  /// decoder that created the nested decoder will trigger a crash. To resume
  /// using the parent decoder, destroy the submessage decoder first.
  ///
  /// @returns A `StreamDecoder` for reading the nested submessage.
  StreamDecoder GetNestedDecoder();

  /// Bounds of a payload interval within a reader.
  struct Bounds {
    size_t low;
    size_t high;
  };

  /// Gets the interval of the payload part of a length-delimited field.
  ///
  /// That is, the interval excluding the field key and the length prefix.
  /// The bounds are relative to the given reader.
  ///
  /// @returns @Result{the bounds of the payload interval}
  Result<Bounds> GetLengthDelimitedPayloadBounds();

 protected:
  // Specialized move constructor used only for codegen.
  //
  // Postcondition: The other decoder is invalidated and cannot be used as it
  //     acts like a parent decoder with an active child decoder.
  constexpr StreamDecoder(StreamDecoder&& other)
      : reader_(other.reader_),
        stream_bounds_(other.stream_bounds_),
        position_(other.position_),
        current_field_(other.current_field_),
        delimited_field_size_(other.delimited_field_size_),
        delimited_field_offset_(other.delimited_field_offset_),
        parent_(other.parent_),
        field_consumed_(other.field_consumed_),
        nested_reader_open_(other.nested_reader_open_),
        status_(other.status_) {
    PW_ASSERT(!nested_reader_open_);
    // Make the nested decoder look like it has an open child to block reads for
    // the remainder of the object's life, and an invalid status to ensure it
    // doesn't advance the stream on destruction.
    other.nested_reader_open_ = true;
    other.parent_ = nullptr;
    other.status_ = pw::Status::Cancelled();
  }

  // Reads proto values from the stream and decodes them into the structure
  // contained within message according to the description of fields in table.
  //
  // This is called by codegen subclass Read() functions that accept a typed
  // struct Message reference, using the appropriate codegen MessageField table
  // corresponding to that type.
  Status Read(span<std::byte> message,
              span<const internal::MessageField> table);

 private:
  friend class BytesReader;

  // The FieldKey class can't store an invalid key, so pick a random large key
  // to set as the initial value. This will be overwritten the first time Next()
  // is called, and FieldKey() fails if Next() is not called first -- ensuring
  // that users will never see this value.
  static constexpr FieldKey kInitialFieldKey =
      FieldKey(20000, WireType::kVarint);

  constexpr StreamDecoder(stream::Reader& reader,
                          StreamDecoder* parent,
                          size_t low,
                          size_t high)
      : reader_(reader),
        stream_bounds_({low, high}),
        position_(parent->position_),
        current_field_(kInitialFieldKey),
        delimited_field_size_(0),
        delimited_field_offset_(0),
        parent_(parent),
        field_consumed_(true),
        nested_reader_open_(false),
        status_(OkStatus()) {}

  // Creates an unusable decoder in an error state. This is required as
  // GetNestedEncoder does not have a way to report an error in its API.
  constexpr StreamDecoder(stream::Reader& reader,
                          StreamDecoder* parent,
                          Status status)
      : reader_(reader),
        stream_bounds_({0, std::numeric_limits<size_t>::max()}),
        position_(0),
        current_field_(kInitialFieldKey),
        delimited_field_size_(0),
        delimited_field_offset_(0),
        parent_(parent),
        field_consumed_(true),
        nested_reader_open_(false),
        status_(status) {
    PW_ASSERT(!status.ok());
  }

  Status Advance(size_t end_position);

  size_t RemainingBytes() {
    return stream_bounds_.high < std::numeric_limits<size_t>::max()
               ? stream_bounds_.high - position_
               : std::numeric_limits<size_t>::max();
  }

  void CloseBytesReader(BytesReader& reader);
  void CloseNestedDecoder(StreamDecoder& nested);

  Status ReadFieldKey();
  Status SkipField();

  Status ReadVarintField(span<std::byte> out, internal::VarintType decode_type);

  StatusWithSize ReadOneVarint(span<std::byte> out,
                               internal::VarintType decode_type);

  template <typename T>
  Result<T> ReadVarintField(internal::VarintType decode_type) {
    static_assert(
        std::is_same_v<T, bool> || std::is_same_v<T, uint32_t> ||
            std::is_same_v<T, int32_t> || std::is_same_v<T, uint64_t> ||
            std::is_same_v<T, int64_t>,
        "Protobuf varints must be of type bool, uint32_t, int32_t, uint64_t, "
        "or int64_t");
    using DecodedValue =
        std::conditional_t<std::is_signed<T>::value, int64_t, uint64_t>;
    static_assert(sizeof(DecodedValue) >= sizeof(T));

    DecodedValue result;
    if (Status status =
            ReadVarintField(as_writable_bytes(span(&result, 1)), decode_type);
        !status.ok()) {
      return status;
    }
    if (result > static_cast<DecodedValue>(std::numeric_limits<T>::max()) ||
        result < static_cast<DecodedValue>(std::numeric_limits<T>::lowest())) {
      // When a varint is too big to fit in an integer, the decoder returns
      // FAILED_PRECONDITION, so this mirrors that behavior.
      return Status::FailedPrecondition();
    }
    return static_cast<T>(result);
  }

  Status ReadFixedField(span<std::byte> out);

  template <typename T>
  Result<T> ReadFixedField() {
    static_assert(
        sizeof(T) == sizeof(uint32_t) || sizeof(T) == sizeof(uint64_t),
        "Protobuf fixed-size fields must be 32- or 64-bit");

    T result;
    if (Status status = ReadFixedField(as_writable_bytes(span(&result, 1)));
        !status.ok()) {
      return status;
    }

    return result;
  }

  StatusWithSize ReadDelimitedField(span<std::byte> out);

  StatusWithSize ReadPackedFixedField(span<std::byte> out, size_t elem_size);

  StatusWithSize ReadPackedVarintField(span<std::byte> out,
                                       size_t elem_size,
                                       internal::VarintType decode_type);

  template <typename T>
  Status ReadRepeatedFixedField(pw::Vector<T>& out) {
    static_assert(
        sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8,
        "Unsupported element size");
    if (out.full()) {
      return Status::ResourceExhausted();
    }
    const size_t old_size = out.size();
    out.resize(out.capacity());
    size_t size = old_size;
    Status status =
        ReadRepeatedFixedFieldGeneric(reinterpret_cast<std::byte*>(out.data()),
                                      out.capacity(),
                                      size,
                                      sizeof(T));
    out.resize(size);
    return status;
  }

  template <typename T>
  Status ReadRepeatedVarintField(pw::Vector<T>& out,
                                 internal::VarintType decode_type) {
    static_assert(
        sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8,
        "Unsupported element size");
    if (out.full()) {
      return Status::ResourceExhausted();
    }
    const size_t old_size = out.size();
    out.resize(out.capacity());
    size_t size = old_size;
    Status status =
        ReadRepeatedVarintFieldGeneric(reinterpret_cast<std::byte*>(out.data()),
                                       out.capacity(),
                                       size,
                                       sizeof(T),
                                       decode_type);
    out.resize(size);
    return status;
  }

  // Reads one varint field for each element in the repeated field, with a
  // runtime size to avoid instantiating the function multiple times. The size
  // is passed as its base 2 log to avoid repeated multiplications in the impl
  // (bit shift instead of multiply).
  Status ReadRepeatedVarintFieldGeneric(std::byte* data,
                                        size_t capacity,
                                        size_t& size,
                                        size_t elem_size,
                                        internal::VarintType decode_type);

  Status ReadRepeatedFixedFieldGeneric(std::byte* data,
                                       size_t capacity,
                                       size_t& size,
                                       size_t elem_size);

  template <typename Container>
  Status ReadStringOrBytesField(std::byte* raw_container) {
    auto& container = *reinterpret_cast<Container*>(raw_container);
    if (container.capacity() < delimited_field_size_) {
      return Status::ResourceExhausted();
    }
    container.resize(container.capacity());
    const auto sws = ReadDelimitedField(as_writable_bytes(span(container)));
    size_t size = sws.size();
    PW_DASSERT(size <= std::numeric_limits<uint16_t>::max());
    container.resize(static_cast<uint16_t>(size));
    return sws.status();
  }

  Status CheckOkToRead(WireType type);

  stream::Reader& reader_;
  Bounds stream_bounds_;
  size_t position_;

  FieldKey current_field_;
  size_t delimited_field_size_;
  size_t delimited_field_offset_;

  StreamDecoder* parent_;

  bool field_consumed_;
  bool nested_reader_open_;

  Status status_;

  friend class Message;
};

/// @endmodule

}  // namespace pw::protobuf
