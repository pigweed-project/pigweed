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

#include "pw_bytes/span.h"
#include "pw_protobuf/decoder.h"
#include "pw_protobuf/stream_decoder.h"
#include "pw_result/result.h"
#include "pw_status/try.h"
#include "pw_string/string.h"

namespace pw::protobuf {

/// Specifies which occurrence of a field to find.
enum class Occurrence {
  /// Returns the first occurrence of the field.
  kFirst,
  /// Returns the last occurrence of the field.
  /// For non-repeated fields, this is proto spec compliant.
  kLast,
};

namespace internal {

Status AdvanceToField(Decoder& decoder, uint32_t field_number);
Status AdvanceToField(StreamDecoder& decoder, uint32_t field_number);

template <typename Derived, typename T>
class FinderBase {
 public:
  /// @brief Finds the last occurrence of the field.
  ///
  /// This method iterates through the message, calling `Next()` on the derived
  /// class repeatedly until the field is no longer found. The last successfully
  /// read value is returned.
  ///
  /// @returns @Result{the last occurrence of the field}
  /// * @NOT_FOUND: The field is not present in the message.
  /// * Other errors from the underlying decoder.
  Result<T> Last() {
    Result<T> last_result = Status::NotFound();
    Result<T> result;
    Derived& self = static_cast<Derived&>(*this);
    while ((result = self.Next()).ok()) {
      last_result = result;
    }
    // If the loop terminated because the field was not found, return the
    // last successful result. Otherwise, return the error that caused the loop
    // to stop (e.g., DATA_LOSS, FAILED_PRECONDITION).
    if (result.status().IsNotFound()) {
      return last_result;
    }
    return result.status();
  }

  /// @brief Finds the first or last occurrence of the field.
  ///
  /// @param occurrence Whether to find the first or last occurrence.
  ///
  /// @returns @Result{the field}
  Result<T> Find(Occurrence occurrence) {
    Derived& self = static_cast<Derived&>(*this);
    return occurrence == Occurrence::kFirst ? self.Next() : self.Last();
  }
};

class RawFinder : public FinderBase<RawFinder, ConstByteSpan> {
 public:
  constexpr RawFinder(ConstByteSpan message, uint32_t field_number)
      : decoder_(message), field_number_(field_number) {}

  Result<ConstByteSpan> Next() {
    PW_TRY(AdvanceToField(decoder_, field_number_));
    return decoder_.RawFieldBytes();
  }

 private:
  Decoder decoder_;
  uint32_t field_number_;
};

}  // namespace internal

/// @submodule{pw_protobuf,find}

/// A stateful finder that scans an in-memory protobuf buffer (`ConstByteSpan`)
/// for occurrences of a specific field.
template <typename T, auto kReadFn>
class Finder : public internal::FinderBase<Finder<T, kReadFn>, T> {
 public:
  constexpr Finder(ConstByteSpan message, uint32_t field_number)
      : decoder_(message), field_number_(field_number) {}

  Result<T> Next() {
    T output;
    PW_TRY(internal::AdvanceToField(decoder_, field_number_));
    PW_TRY((decoder_.*kReadFn)(&output));
    return output;
  }

 private:
  Decoder decoder_;
  uint32_t field_number_;
};

/// A stateful finder that scans a stream (`stream::Reader`) for occurrences of
/// a specific field.
template <typename T, auto kReadFn>
class StreamFinder : public internal::FinderBase<StreamFinder<T, kReadFn>, T> {
 public:
  constexpr StreamFinder(stream::Reader& reader, uint32_t field_number)
      : decoder_(reader), field_number_(field_number) {}

  Result<T> Next() {
    PW_TRY(internal::AdvanceToField(decoder_, field_number_));
    Result<T> result = (decoder_.*kReadFn)();

    // The StreamDecoder returns a NOT_FOUND if trying to read the wrong type
    // for a field. Remap this to FAILED_PRECONDITION for consistency with the
    // non-stream Find.
    return result.status().IsNotFound()
               ? Result<T>(Status::FailedPrecondition())
               : result;
  }

 private:
  StreamDecoder decoder_;
  uint32_t field_number_;
};

/// A stateful finder for `enum` fields in an in-memory protobuf buffer.
template <typename T>
class EnumFinder : private Finder<uint32_t, &Decoder::ReadUint32> {
 public:
  using Finder::Finder;

  Result<T> Next() {
    Result<uint32_t> result = Finder::Next();
    if (!result.ok()) {
      return result.status();
    }
    return static_cast<T>(result.value());
  }
};

/// A stateful finder for `enum` fields in a protobuf stream.
template <typename T>
class EnumStreamFinder
    : private StreamFinder<uint32_t, &StreamDecoder::ReadUint32> {
 public:
  using StreamFinder::StreamFinder;

  Result<T> Next() {
    Result<uint32_t> result = StreamFinder::Next();
    if (!result.ok()) {
      return result.status();
    }
    return static_cast<T>(result.value());
  }
};

/// @endsubmodule

namespace internal {
template <typename T, auto kReadFn>
Result<T> Find(ConstByteSpan message,
               uint32_t field_number,
               Occurrence occurrence) {
  Finder<T, kReadFn> finder(message, field_number);
  return finder.Find(occurrence);
}

template <typename T, auto kReadFn>
Result<T> Find(stream::Reader& reader,
               uint32_t field_number,
               Occurrence occurrence) {
  StreamFinder<T, kReadFn> finder(reader, field_number);
  return finder.Find(occurrence);
}

}  // namespace internal

/// @submodule{pw_protobuf,find}

/// @brief Scans a serialized protobuf message for a `uint32` field.
///
/// @param message The serialized message to search.
/// @param field_number Protobuf field number of the field.
/// @param occurrence Whether to find the first or last occurrence.
///
/// @returns @Result{the field}
/// * @NOT_FOUND: The field is not present.
/// * @DATA_LOSS: The serialized message is not a valid protobuf.
/// * @FAILED_PRECONDITION: The field exists, but is not the correct type.
inline Result<uint32_t> FindUint32(ConstByteSpan message,
                                   uint32_t field_number,
                                   Occurrence occurrence) {
  return internal::Find<uint32_t, &Decoder::ReadUint32>(
      message, field_number, occurrence);
}

template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
inline Result<uint32_t> FindUint32(ConstByteSpan message,
                                   T field,
                                   Occurrence occurrence) {
  return FindUint32(message, static_cast<uint32_t>(field), occurrence);
}

/// @brief Scans a serialized protobuf message for a `uint32` field.
///
/// @param message_stream The serialized message to search.
/// @param field_number Protobuf field number of the field.
/// @param occurrence Specifies which occurrence to find.
///
/// @returns @Result{the field}
/// * @NOT_FOUND: The field is not present.
/// * @DATA_LOSS: The serialized message is not a valid protobuf.
/// * @FAILED_PRECONDITION: The field exists, but is not the correct type.
inline Result<uint32_t> FindUint32(stream::Reader& message_stream,
                                   uint32_t field_number,
                                   Occurrence occurrence) {
  return internal::Find<uint32_t, &StreamDecoder::ReadUint32>(
      message_stream, field_number, occurrence);
}

template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
inline Result<uint32_t> FindUint32(stream::Reader& message_stream,
                                   T field,
                                   Occurrence occurrence) {
  return FindUint32(message_stream, static_cast<uint32_t>(field), occurrence);
}

using Uint32Finder = Finder<uint32_t, &Decoder::ReadUint32>;
using Uint32StreamFinder = StreamFinder<uint32_t, &StreamDecoder::ReadUint32>;

/// @brief Scans a serialized protobuf message for an `int32` field.
///
/// @param message The serialized message to search.
/// @param field_number Protobuf field number of the field.
/// @param occurrence Whether to find the first or last occurrence.
///
/// @returns @Result{the field}
/// * @NOT_FOUND: The field is not present.
/// * @DATA_LOSS: The serialized message is not a valid protobuf.
/// * @FAILED_PRECONDITION: The field exists, but is not the correct type.
inline Result<int32_t> FindInt32(ConstByteSpan message,
                                 uint32_t field_number,
                                 Occurrence occurrence) {
  return internal::Find<int32_t, &Decoder::ReadInt32>(
      message, field_number, occurrence);
}

template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
inline Result<int32_t> FindInt32(ConstByteSpan message,
                                 T field,
                                 Occurrence occurrence) {
  return FindInt32(message, static_cast<uint32_t>(field), occurrence);
}

/// @brief Scans a serialized protobuf message for an `int32` field.
///
/// @param message_stream The serialized message to search.
/// @param field_number Protobuf field number of the field.
/// @param occurrence Specifies which occurrence to find.
///
/// @returns @Result{the field}
/// * @NOT_FOUND: The field is not present.
/// * @DATA_LOSS: The serialized message is not a valid protobuf.
/// * @FAILED_PRECONDITION: The field exists, but is not the correct type.
inline Result<int32_t> FindInt32(stream::Reader& message_stream,
                                 uint32_t field_number,
                                 Occurrence occurrence) {
  return internal::Find<int32_t, &StreamDecoder::ReadInt32>(
      message_stream, field_number, occurrence);
}

template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
inline Result<int32_t> FindInt32(stream::Reader& message_stream,
                                 T field,
                                 Occurrence occurrence) {
  return FindInt32(message_stream, static_cast<uint32_t>(field), occurrence);
}

using Int32Finder = Finder<int32_t, &Decoder::ReadInt32>;
using Int32StreamFinder = StreamFinder<int32_t, &StreamDecoder::ReadInt32>;

/// @brief Scans a serialized protobuf message for an `sint32` field.
///
/// @param message The serialized message to search.
/// @param field_number Protobuf field number of the field.
/// @param occurrence Whether to find the first or last occurrence.
///
/// @returns @Result{the field}
/// * @NOT_FOUND: The field is not present.
/// * @DATA_LOSS: The serialized message is not a valid protobuf.
/// * @FAILED_PRECONDITION: The field exists, but is not the correct type.
inline Result<int32_t> FindSint32(ConstByteSpan message,
                                  uint32_t field_number,
                                  Occurrence occurrence) {
  return internal::Find<int32_t, &Decoder::ReadSint32>(
      message, field_number, occurrence);
}

template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
inline Result<int32_t> FindSint32(ConstByteSpan message,
                                  T field,
                                  Occurrence occurrence) {
  return FindSint32(message, static_cast<uint32_t>(field), occurrence);
}

/// @brief Scans a serialized protobuf message for an `sint32` field.
///
/// @param message_stream The serialized message to search.
/// @param field_number Protobuf field number of the field.
/// @param occurrence Specifies which occurrence to find.
///
/// @returns @Result{the field}
/// * @NOT_FOUND: The field is not present.
/// * @DATA_LOSS: The serialized message is not a valid protobuf.
/// * @FAILED_PRECONDITION: The field exists, but is not the correct type.
inline Result<int32_t> FindSint32(stream::Reader& message_stream,
                                  uint32_t field_number,
                                  Occurrence occurrence) {
  return internal::Find<int32_t, &StreamDecoder::ReadSint32>(
      message_stream, field_number, occurrence);
}

template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
inline Result<int32_t> FindSint32(stream::Reader& message_stream,
                                  T field,
                                  Occurrence occurrence) {
  return FindSint32(message_stream, static_cast<uint32_t>(field), occurrence);
}

using Sint32Finder = Finder<int32_t, &Decoder::ReadSint32>;
using Sint32StreamFinder = StreamFinder<int32_t, &StreamDecoder::ReadSint32>;

/// @brief Scans a serialized protobuf message for a `uint64` field.
///
/// @param message The serialized message to search.
/// @param field_number Protobuf field number of the field.
/// @param occurrence Whether to find the first or last occurrence.
///
/// @returns @Result{the field}
/// * @NOT_FOUND: The field is not present.
/// * @DATA_LOSS: The serialized message is not a valid protobuf.
/// * @FAILED_PRECONDITION: The field exists, but is not the correct type.
inline Result<uint64_t> FindUint64(ConstByteSpan message,
                                   uint32_t field_number,
                                   Occurrence occurrence) {
  return internal::Find<uint64_t, &Decoder::ReadUint64>(
      message, field_number, occurrence);
}

template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
inline Result<uint64_t> FindUint64(ConstByteSpan message,
                                   T field,
                                   Occurrence occurrence) {
  return FindUint64(message, static_cast<uint32_t>(field), occurrence);
}

/// @brief Scans a serialized protobuf message for a `uint64` field.
///
/// @param message_stream The serialized message to search.
/// @param field_number Protobuf field number of the field.
/// @param occurrence Specifies which occurrence to find.
///
/// @returns @Result{the field}
/// * @NOT_FOUND: The field is not present.
/// * @DATA_LOSS: The serialized message is not a valid protobuf.
/// * @FAILED_PRECONDITION: The field exists, but is not the correct type.
inline Result<uint64_t> FindUint64(stream::Reader& message_stream,
                                   uint32_t field_number,
                                   Occurrence occurrence) {
  return internal::Find<uint64_t, &StreamDecoder::ReadUint64>(
      message_stream, field_number, occurrence);
}

template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
inline Result<uint64_t> FindUint64(stream::Reader& message_stream,
                                   T field,
                                   Occurrence occurrence) {
  return FindUint64(message_stream, static_cast<uint32_t>(field), occurrence);
}

using Uint64Finder = Finder<uint64_t, &Decoder::ReadUint64>;
using Uint64StreamFinder = StreamFinder<uint64_t, &StreamDecoder::ReadUint64>;

/// @brief Scans a serialized protobuf message for an `int64` field.
///
/// @param message The serialized message to search.
/// @param field_number Protobuf field number of the field.
/// @param occurrence Whether to find the first or last occurrence.
///
/// @returns @Result{the field}
/// * @NOT_FOUND: The field is not present.
/// * @DATA_LOSS: The serialized message is not a valid protobuf.
/// * @FAILED_PRECONDITION: The field exists, but is not the correct type.
inline Result<int64_t> FindInt64(ConstByteSpan message,
                                 uint32_t field_number,
                                 Occurrence occurrence) {
  return internal::Find<int64_t, &Decoder::ReadInt64>(
      message, field_number, occurrence);
}

template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
inline Result<int64_t> FindInt64(ConstByteSpan message,
                                 T field,
                                 Occurrence occurrence) {
  return FindInt64(message, static_cast<uint32_t>(field), occurrence);
}

/// @brief Scans a serialized protobuf message for an `int64` field.
///
/// @param message_stream The serialized message to search.
/// @param field_number Protobuf field number of the field.
/// @param occurrence Specifies which occurrence to find.
///
/// @returns @Result{the field}
/// * @NOT_FOUND: The field is not present.
/// * @DATA_LOSS: The serialized message is not a valid protobuf.
/// * @FAILED_PRECONDITION: The field exists, but is not the correct type.
inline Result<int64_t> FindInt64(stream::Reader& message_stream,
                                 uint32_t field_number,
                                 Occurrence occurrence) {
  return internal::Find<int64_t, &StreamDecoder::ReadInt64>(
      message_stream, field_number, occurrence);
}

template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
inline Result<int64_t> FindInt64(stream::Reader& message_stream,
                                 T field,
                                 Occurrence occurrence) {
  return FindInt64(message_stream, static_cast<uint32_t>(field), occurrence);
}

using Int64Finder = Finder<int64_t, &Decoder::ReadInt64>;
using Int64StreamFinder = StreamFinder<int64_t, &StreamDecoder::ReadInt64>;

/// @brief Scans a serialized protobuf message for an `sint64` field.
///
/// @param message The serialized message to search.
/// @param field_number Protobuf field number of the field.
/// @param occurrence Whether to find the first or last occurrence.
///
/// @returns @Result{the field}
/// * @NOT_FOUND: The field is not present.
/// * @DATA_LOSS: The serialized message is not a valid protobuf.
/// * @FAILED_PRECONDITION: The field exists, but is not the correct type.
inline Result<int64_t> FindSint64(ConstByteSpan message,
                                  uint32_t field_number,
                                  Occurrence occurrence) {
  return internal::Find<int64_t, &Decoder::ReadSint64>(
      message, field_number, occurrence);
}

template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
inline Result<int64_t> FindSint64(ConstByteSpan message,
                                  T field,
                                  Occurrence occurrence) {
  return FindSint64(message, static_cast<uint32_t>(field), occurrence);
}

/// @brief Scans a serialized protobuf message for an `sint64` field.
///
/// @param message_stream The serialized message to search.
/// @param field_number Protobuf field number of the field.
/// @param occurrence Specifies which occurrence to find.
///
/// @returns @Result{the field}
/// * @NOT_FOUND: The field is not present.
/// * @DATA_LOSS: The serialized message is not a valid protobuf.
/// * @FAILED_PRECONDITION: The field exists, but is not the correct type.
inline Result<int64_t> FindSint64(stream::Reader& message_stream,
                                  uint32_t field_number,
                                  Occurrence occurrence) {
  return internal::Find<int64_t, &StreamDecoder::ReadSint64>(
      message_stream, field_number, occurrence);
}

template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
inline Result<int64_t> FindSint64(stream::Reader& message_stream,
                                  T field,
                                  Occurrence occurrence) {
  return FindSint64(message_stream, static_cast<uint32_t>(field), occurrence);
}

using Sint64Finder = Finder<int64_t, &Decoder::ReadSint64>;
using Sint64StreamFinder = StreamFinder<int64_t, &StreamDecoder::ReadSint64>;

/// @brief Scans a serialized protobuf message for a `bool` field.
///
/// @param message The serialized message to search.
/// @param field_number Protobuf field number of the field.
/// @param occurrence Whether to find the first or last occurrence.
///
/// @returns @Result{the field}
/// * @NOT_FOUND: The field is not present.
/// * @DATA_LOSS: The serialized message is not a valid protobuf.
/// * @FAILED_PRECONDITION: The field exists, but is not the correct type.
inline Result<bool> FindBool(ConstByteSpan message,
                             uint32_t field_number,
                             Occurrence occurrence) {
  return internal::Find<bool, &Decoder::ReadBool>(
      message, field_number, occurrence);
}

template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
inline Result<bool> FindBool(ConstByteSpan message,
                             T field,
                             Occurrence occurrence) {
  return FindBool(message, static_cast<uint32_t>(field), occurrence);
}

/// @brief Scans a serialized protobuf message for a `bool` field.
///
/// @param message_stream The serialized message to search.
/// @param field_number Protobuf field number of the field.
/// @param occurrence Specifies which occurrence to find.
///
/// @returns @Result{the field}
/// * @NOT_FOUND: The field is not present.
/// * @DATA_LOSS: The serialized message is not a valid protobuf.
/// * @FAILED_PRECONDITION: The field exists, but is not the correct type.
inline Result<bool> FindBool(stream::Reader& message_stream,
                             uint32_t field_number,
                             Occurrence occurrence) {
  return internal::Find<bool, &StreamDecoder::ReadBool>(
      message_stream, field_number, occurrence);
}

template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
inline Result<bool> FindBool(stream::Reader& message_stream,
                             T field,
                             Occurrence occurrence) {
  return FindBool(message_stream, static_cast<uint32_t>(field), occurrence);
}

using BoolFinder = Finder<bool, &Decoder::ReadBool>;
using BoolStreamFinder = StreamFinder<bool, &StreamDecoder::ReadBool>;

/// @brief Scans a serialized protobuf message for a `fixed32` field.
///
/// @param message The serialized message to search.
/// @param field_number Protobuf field number of the field.
/// @param occurrence Whether to find the first or last occurrence.
///
/// @returns @Result{the field}
/// * @NOT_FOUND: The field is not present.
/// * @DATA_LOSS: The serialized message is not a valid protobuf.
/// * @FAILED_PRECONDITION: The field exists, but is not the correct type.
inline Result<uint32_t> FindFixed32(ConstByteSpan message,
                                    uint32_t field_number,
                                    Occurrence occurrence) {
  return internal::Find<uint32_t, &Decoder::ReadFixed32>(
      message, field_number, occurrence);
}

template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
inline Result<uint32_t> FindFixed32(ConstByteSpan message,
                                    T field,
                                    Occurrence occurrence) {
  return FindFixed32(message, static_cast<uint32_t>(field), occurrence);
}

/// @brief Scans a serialized protobuf message for a `fixed32` field.
///
/// @param message_stream The serialized message to search.
/// @param field_number Protobuf field number of the field.
/// @param occurrence Specifies which occurrence to find.
///
/// @returns @Result{the field}
/// * @NOT_FOUND: The field is not present.
/// * @DATA_LOSS: The serialized message is not a valid protobuf.
/// * @FAILED_PRECONDITION: The field exists, but is not the correct type.
inline Result<uint32_t> FindFixed32(stream::Reader& message_stream,
                                    uint32_t field_number,
                                    Occurrence occurrence) {
  return internal::Find<uint32_t, &StreamDecoder::ReadFixed32>(
      message_stream, field_number, occurrence);
}

template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
inline Result<uint32_t> FindFixed32(stream::Reader& message_stream,
                                    T field,
                                    Occurrence occurrence) {
  return FindFixed32(message_stream, static_cast<uint32_t>(field), occurrence);
}

using Fixed32Finder = Finder<uint32_t, &Decoder::ReadFixed32>;
using Fixed32StreamFinder = StreamFinder<uint32_t, &StreamDecoder::ReadFixed32>;

/// @brief Scans a serialized protobuf message for a `fixed64` field.
///
/// @param message The serialized message to search.
/// @param field_number Protobuf field number of the field.
/// @param occurrence Whether to find the first or last occurrence.
///
/// @returns @Result{the field}
/// * @NOT_FOUND: The field is not present.
/// * @DATA_LOSS: The serialized message is not a valid protobuf.
/// * @FAILED_PRECONDITION: The field exists, but is not the correct type.
inline Result<uint64_t> FindFixed64(ConstByteSpan message,
                                    uint32_t field_number,
                                    Occurrence occurrence) {
  return internal::Find<uint64_t, &Decoder::ReadFixed64>(
      message, field_number, occurrence);
}

template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
inline Result<uint64_t> FindFixed64(ConstByteSpan message,
                                    T field,
                                    Occurrence occurrence) {
  return FindFixed64(message, static_cast<uint32_t>(field), occurrence);
}

/// @brief Scans a serialized protobuf message for a `fixed64` field.
///
/// @param message_stream The serialized message to search.
/// @param field_number Protobuf field number of the field.
/// @param occurrence Specifies which occurrence to find.
///
/// @returns @Result{the field}
/// * @NOT_FOUND: The field is not present.
/// * @DATA_LOSS: The serialized message is not a valid protobuf.
/// * @FAILED_PRECONDITION: The field exists, but is not the correct type.
inline Result<uint64_t> FindFixed64(stream::Reader& message_stream,
                                    uint32_t field_number,
                                    Occurrence occurrence) {
  return internal::Find<uint64_t, &StreamDecoder::ReadFixed64>(
      message_stream, field_number, occurrence);
}

template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
inline Result<uint64_t> FindFixed64(stream::Reader& message_stream,
                                    T field,
                                    Occurrence occurrence) {
  return FindFixed64(message_stream, static_cast<uint32_t>(field), occurrence);
}

using Fixed64Finder = Finder<uint64_t, &Decoder::ReadFixed64>;
using Fixed64StreamFinder = StreamFinder<uint64_t, &StreamDecoder::ReadFixed64>;

/// @brief Scans a serialized protobuf message for an `sfixed32` field.
///
/// @param message The serialized message to search.
/// @param field_number Protobuf field number of the field.
/// @param occurrence Whether to find the first or last occurrence.
///
/// @returns @Result{the field}
/// * @NOT_FOUND: The field is not present.
/// * @DATA_LOSS: The serialized message is not a valid protobuf.
/// * @FAILED_PRECONDITION: The field exists, but is not the correct type.
inline Result<int32_t> FindSfixed32(ConstByteSpan message,
                                    uint32_t field_number,
                                    Occurrence occurrence) {
  return internal::Find<int32_t, &Decoder::ReadSfixed32>(
      message, field_number, occurrence);
}

template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
inline Result<int32_t> FindSfixed32(ConstByteSpan message,
                                    T field,
                                    Occurrence occurrence) {
  return FindSfixed32(message, static_cast<uint32_t>(field), occurrence);
}

/// @brief Scans a serialized protobuf message for an `sfixed32` field.
///
/// @param message_stream The serialized message to search.
/// @param field_number Protobuf field number of the field.
/// @param occurrence Specifies which occurrence to find.
///
/// @returns @Result{the field}
/// * @NOT_FOUND: The field is not present.
/// * @DATA_LOSS: The serialized message is not a valid protobuf.
/// * @FAILED_PRECONDITION: The field exists, but is not the correct type.
inline Result<int32_t> FindSfixed32(stream::Reader& message_stream,
                                    uint32_t field_number,
                                    Occurrence occurrence) {
  return internal::Find<int32_t, &StreamDecoder::ReadSfixed32>(
      message_stream, field_number, occurrence);
}

template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
inline Result<int32_t> FindSfixed32(stream::Reader& message_stream,
                                    T field,
                                    Occurrence occurrence) {
  return FindSfixed32(message_stream, static_cast<uint32_t>(field), occurrence);
}

using Sfixed32Finder = Finder<int32_t, &Decoder::ReadSfixed32>;
using Sfixed32StreamFinder =
    StreamFinder<int32_t, &StreamDecoder::ReadSfixed32>;

/// @brief Scans a serialized protobuf message for an `sfixed64` field.
///
/// @param message The serialized message to search.
/// @param field_number Protobuf field number of the field.
/// @param occurrence Whether to find the first or last occurrence.
///
/// @returns @Result{the field}
/// * @NOT_FOUND: The field is not present.
/// * @DATA_LOSS: The serialized message is not a valid protobuf.
/// * @FAILED_PRECONDITION: The field exists, but is not the correct type.
inline Result<int64_t> FindSfixed64(ConstByteSpan message,
                                    uint32_t field_number,
                                    Occurrence occurrence) {
  return internal::Find<int64_t, &Decoder::ReadSfixed64>(
      message, field_number, occurrence);
}

template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
inline Result<int64_t> FindSfixed64(ConstByteSpan message,
                                    T field,
                                    Occurrence occurrence) {
  return FindSfixed64(message, static_cast<uint32_t>(field), occurrence);
}

/// @brief Scans a serialized protobuf message for an `sfixed64` field.
///
/// @param message_stream The serialized message to search.
/// @param field_number Protobuf field number of the field.
/// @param occurrence Specifies which occurrence to find.
///
/// @returns @Result{the field}
/// * @NOT_FOUND: The field is not present.
/// * @DATA_LOSS: The serialized message is not a valid protobuf.
/// * @FAILED_PRECONDITION: The field exists, but is not the correct type.
inline Result<int64_t> FindSfixed64(stream::Reader& message_stream,
                                    uint32_t field_number,
                                    Occurrence occurrence) {
  return internal::Find<int64_t, &StreamDecoder::ReadSfixed64>(
      message_stream, field_number, occurrence);
}

template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
inline Result<int64_t> FindSfixed64(stream::Reader& message_stream,
                                    T field,
                                    Occurrence occurrence) {
  return FindSfixed64(message_stream, static_cast<uint32_t>(field), occurrence);
}

using Sfixed64Finder = Finder<int64_t, &Decoder::ReadSfixed64>;
using Sfixed64StreamFinder =
    StreamFinder<int64_t, &StreamDecoder::ReadSfixed64>;

/// @brief Scans a serialized protobuf message for a `float` field.
///
/// @param message The serialized message to search.
/// @param field_number Protobuf field number of the field.
/// @param occurrence Whether to find the first or last occurrence.
///
/// @returns @Result{the field}
/// * @NOT_FOUND: The field is not present.
/// * @DATA_LOSS: The serialized message is not a valid protobuf.
/// * @FAILED_PRECONDITION: The field exists, but is not the correct type.
inline Result<float> FindFloat(ConstByteSpan message,
                               uint32_t field_number,
                               Occurrence occurrence) {
  return internal::Find<float, &Decoder::ReadFloat>(
      message, field_number, occurrence);
}

template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
inline Result<float> FindFloat(ConstByteSpan message,
                               T field,
                               Occurrence occurrence) {
  return FindFloat(message, static_cast<uint32_t>(field), occurrence);
}

/// @brief Scans a serialized protobuf message for a `float` field.
///
/// @param message_stream The serialized message to search.
/// @param field_number Protobuf field number of the field.
/// @param occurrence Specifies which occurrence to find.
///
/// @returns @Result{the field}
/// * @NOT_FOUND: The field is not present.
/// * @DATA_LOSS: The serialized message is not a valid protobuf.
/// * @FAILED_PRECONDITION: The field exists, but is not the correct type.
inline Result<float> FindFloat(stream::Reader& message_stream,
                               uint32_t field_number,
                               Occurrence occurrence) {
  return internal::Find<float, &StreamDecoder::ReadFloat>(
      message_stream, field_number, occurrence);
}

template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
inline Result<float> FindFloat(stream::Reader& message_stream,
                               T field,
                               Occurrence occurrence) {
  return FindFloat(message_stream, static_cast<uint32_t>(field), occurrence);
}

using FloatFinder = Finder<float, &Decoder::ReadFloat>;
using FloatStreamFinder = StreamFinder<float, &StreamDecoder::ReadFloat>;

/// @brief Scans a serialized protobuf message for a `double` field.
///
/// @param message The serialized message to search.
/// @param field_number Protobuf field number of the field.
/// @param occurrence Whether to find the first or last occurrence.
///
/// @returns @Result{the field}
/// * @NOT_FOUND: The field is not present.
/// * @DATA_LOSS: The serialized message is not a valid protobuf.
/// * @FAILED_PRECONDITION: The field exists, but is not the correct type.
inline Result<double> FindDouble(ConstByteSpan message,
                                 uint32_t field_number,
                                 Occurrence occurrence) {
  return internal::Find<double, &Decoder::ReadDouble>(
      message, field_number, occurrence);
}

template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
inline Result<double> FindDouble(ConstByteSpan message,
                                 T field,
                                 Occurrence occurrence) {
  return FindDouble(message, static_cast<uint32_t>(field), occurrence);
}

/// @brief Scans a serialized protobuf message for a `double` field.
///
/// @param message_stream The serialized message to search.
/// @param field_number Protobuf field number of the field.
/// @param occurrence Specifies which occurrence to find.
///
/// @returns @Result{the field}
/// * @NOT_FOUND: The field is not present.
/// * @DATA_LOSS: The serialized message is not a valid protobuf.
/// * @FAILED_PRECONDITION: The field exists, but is not the correct type.
inline Result<double> FindDouble(stream::Reader& message_stream,
                                 uint32_t field_number,
                                 Occurrence occurrence) {
  return internal::Find<double, &StreamDecoder::ReadDouble>(
      message_stream, field_number, occurrence);
}

template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
inline Result<double> FindDouble(stream::Reader& message_stream,
                                 T field,
                                 Occurrence occurrence) {
  return FindDouble(message_stream, static_cast<uint32_t>(field), occurrence);
}

using DoubleFinder = Finder<double, &Decoder::ReadDouble>;
using DoubleStreamFinder = StreamFinder<double, &StreamDecoder::ReadDouble>;

/// @brief Scans a serialized protobuf message for a `string` field.
///
/// @param message The serialized message to search.
/// @param field_number Protobuf field number of the field.
/// @param occurrence Whether to find the first or last occurrence.
///
/// @note The returned string is NOT null-terminated.
///
/// @returns @Result{a subspan of the buffer containing the string field}
/// * @NOT_FOUND: The field is not present.
/// * @DATA_LOSS: The serialized message is not a valid protobuf.
/// * @FAILED_PRECONDITION: The field exists, but is not the correct type.
inline Result<std::string_view> FindString(ConstByteSpan message,
                                           uint32_t field_number,
                                           Occurrence occurrence) {
  return internal::Find<std::string_view, &Decoder::ReadString>(
      message, field_number, occurrence);
}

template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
inline Result<std::string_view> FindString(ConstByteSpan message,
                                           T field,
                                           Occurrence occurrence) {
  return FindString(message, static_cast<uint32_t>(field), occurrence);
}

inline StatusWithSize FindString(stream::Reader& message_stream,
                                 uint32_t field_number,
                                 span<char> out,
                                 Occurrence occurrence) {
  StreamDecoder decoder(message_stream);
  if (occurrence == Occurrence::kFirst) {
    Status status = internal::AdvanceToField(decoder, field_number);
    if (!status.ok()) {
      return StatusWithSize(status, 0);
    }
    StatusWithSize sws = decoder.ReadString(out);
    if (sws.status().IsResourceExhausted()) {
      decoder.SkipField().IgnoreError();
    }
    return sws.status().IsNotFound() ? StatusWithSize::FailedPrecondition()
                                     : sws;
  }

  Status status;
  StatusWithSize last_sws(Status::NotFound(), 0);
  while ((status = internal::AdvanceToField(decoder, field_number)).ok()) {
    last_sws = decoder.ReadString(out);
    if (last_sws.status().IsResourceExhausted()) {
      decoder.SkipField().IgnoreError();
    }
    if (last_sws.status().IsNotFound()) {
      return StatusWithSize::FailedPrecondition();
    }
    if (!last_sws.status().ok()) {
      return last_sws;
    }
  }
  if (status.IsNotFound()) {
    return last_sws;
  }
  return StatusWithSize(status, 0);
}

inline StatusWithSize FindString(stream::Reader& message_stream,
                                 uint32_t field_number,
                                 InlineString<>& out,
                                 Occurrence occurrence) {
  StatusWithSize sws;

  out.resize_and_overwrite([&](char* data, size_t size) {
    sws =
        FindString(message_stream, field_number, span(data, size), occurrence);
    return sws.size();
  });

  return sws;
}

template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
inline StatusWithSize FindString(stream::Reader& message_stream,
                                 T field,
                                 span<char> out,
                                 Occurrence occurrence) {
  return FindString(
      message_stream, static_cast<uint32_t>(field), out, occurrence);
}

template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
inline StatusWithSize FindString(stream::Reader& message_stream,
                                 T field,
                                 InlineString<>& out,
                                 Occurrence occurrence) {
  return FindString(
      message_stream, static_cast<uint32_t>(field), out, occurrence);
}

using StringFinder = Finder<std::string_view, &Decoder::ReadString>;

/// @brief Scans a serialized protobuf message for a `bytes` field.
///
/// @param message The serialized message to search.
/// @param field_number Protobuf field number of the field.
/// @param occurrence Whether to find the first or last occurrence.
///
/// @returns @Result{the subspan of the buffer containing the bytes field}
/// * @NOT_FOUND: The field is not present.
/// * @DATA_LOSS: The serialized message is not a valid protobuf.
/// * @FAILED_PRECONDITION: The field exists, but is not the correct type.
inline Result<ConstByteSpan> FindBytes(ConstByteSpan message,
                                       uint32_t field_number,
                                       Occurrence occurrence) {
  return internal::Find<ConstByteSpan, &Decoder::ReadBytes>(
      message, field_number, occurrence);
}

template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
inline Result<ConstByteSpan> FindBytes(ConstByteSpan message,
                                       T field,
                                       Occurrence occurrence) {
  return FindBytes(message, static_cast<uint32_t>(field), occurrence);
}

/// @brief Scans a serialized protobuf message for a `bytes` field, copying its
/// data into the provided buffer.
///
/// @param message_stream The serialized message to search.
/// @param field_number Protobuf field number of the field.
/// @param out The buffer to which to write the bytes.
/// @param occurrence Specifies which occurrence to find.
///
/// @returns
/// * @OK: Returns the size of the copied data.
/// * @NOT_FOUND: The field is not present.
/// * @DATA_LOSS: The serialized message is not a valid protobuf.
/// * @FAILED_PRECONDITION: The field exists, but is not the correct type.
inline StatusWithSize FindBytes(stream::Reader& message_stream,
                                uint32_t field_number,
                                ByteSpan out,
                                Occurrence occurrence) {
  StreamDecoder decoder(message_stream);
  if (occurrence == Occurrence::kFirst) {
    Status status = internal::AdvanceToField(decoder, field_number);
    if (!status.ok()) {
      return StatusWithSize(status, 0);
    }
    StatusWithSize sws = decoder.ReadBytes(out);
    if (sws.status().IsResourceExhausted()) {
      decoder.SkipField().IgnoreError();
    }
    return sws.status().IsNotFound() ? StatusWithSize::FailedPrecondition()
                                     : sws;
  }

  Status status;
  StatusWithSize last_sws(Status::NotFound(), 0);
  while ((status = internal::AdvanceToField(decoder, field_number)).ok()) {
    last_sws = decoder.ReadBytes(out);
    if (last_sws.status().IsResourceExhausted()) {
      decoder.SkipField().IgnoreError();
    }
    if (last_sws.status().IsNotFound()) {
      return StatusWithSize::FailedPrecondition();
    }
    if (!last_sws.status().ok()) {
      return last_sws;
    }
  }
  if (status.IsNotFound()) {
    return last_sws;
  }
  return StatusWithSize(status, 0);
}

template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
inline StatusWithSize FindBytes(stream::Reader& message_stream,
                                T field,
                                ByteSpan out,
                                Occurrence occurrence) {
  return FindBytes(
      message_stream, static_cast<uint32_t>(field), out, occurrence);
}

using BytesFinder = Finder<ConstByteSpan, &Decoder::ReadBytes>;

/// @brief Scans a serialized protobuf message for a submessage.
///
/// @param message The serialized message to search.
/// @param field_number Protobuf field number of the field.
/// @param occurrence Whether to find the first or last occurrence.
///
/// @returns @Result{the subspan of the buffer containing the submessage}
/// * @NOT_FOUND: The field is not present.
/// * @DATA_LOSS: The serialized message is not a valid protobuf.
/// * @FAILED_PRECONDITION: The field exists, but is not the correct type.
inline Result<ConstByteSpan> FindSubmessage(ConstByteSpan message,
                                            uint32_t field_number,
                                            Occurrence occurrence) {
  // On the wire, a submessage is identical to bytes. This function exists only
  // to clarify users' intent.
  return FindBytes(message, field_number, occurrence);
}

template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
inline Result<ConstByteSpan> FindSubmessage(ConstByteSpan message,
                                            T field,
                                            Occurrence occurrence) {
  return FindSubmessage(message, static_cast<uint32_t>(field), occurrence);
}

/// Returns a span containing the raw bytes of the value.
inline Result<ConstByteSpan> FindRaw(ConstByteSpan message,
                                     uint32_t field_number,
                                     Occurrence occurrence) {
  internal::RawFinder finder(message, field_number);
  return finder.Find(occurrence);
}

template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
inline Result<ConstByteSpan> FindRaw(ConstByteSpan message,
                                     T field,
                                     Occurrence occurrence) {
  return FindRaw(message, static_cast<uint32_t>(field), occurrence);
}

/// @endsubmodule

}  // namespace pw::protobuf
