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

#include <array>
#include <cstdint>
#include <string_view>

#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_string/string_builder.h"

namespace pw {

/// Provides basic helpers for reading and writing UTF-8-encoded strings.
namespace utf {

/// @submodule{pw_string,utf8}

/// Checks if the code point is in a valid range.
///
/// Excludes the surrogate code points (`[0xD800, 0xDFFF]`) and
/// codepoints larger than `0x10FFFF` (the highest codepoint allowed).
/// Non-characters and unassigned codepoints are allowed.
constexpr inline bool IsValidCodepoint(uint32_t code_point) {
  return code_point < 0xD800u ||
         (code_point >= 0xE000u && code_point <= 0x10FFFFu);
}

/// Checks if the code point is a valid character.
///
/// Excludes non-characters (`U+FDD0..U+FDEF`, and all codepoints ending in
/// `0xFFFE` or `0xFFFF`) from the set of valid code points.
constexpr inline bool IsValidCharacter(uint32_t code_point) {
  return code_point < 0xD800u ||
         (code_point >= 0xE000u && code_point < 0xFDD0u) ||
         (code_point > 0xFDEFu && code_point <= 0x10FFFFu &&
          (code_point & 0xFFFEu) != 0xFFFEu);
}

/// @class CodePointAndSize
///
/// Provides a combined view of a valid codepoint and the number of bytes its
/// encoding requires. The maximum valid codepoint is `0x10FFFFU` which requires
/// 20 bits to represent. This combined view uses the available upper bits to
/// encode the number of bytes required to represent the codepoint when UTF
/// encoded.
class CodePointAndSize final {
 public:
  /// Creates a combined view of a @p code_point and its encoded @p size.
  explicit constexpr CodePointAndSize(uint32_t code_point, size_t size)
      : code_point_((static_cast<uint32_t>(size) << kSizeShift) | code_point) {}

  constexpr CodePointAndSize(const CodePointAndSize&) = default;
  constexpr CodePointAndSize& operator=(const CodePointAndSize&) = default;
  constexpr CodePointAndSize(CodePointAndSize&&) = default;
  constexpr CodePointAndSize& operator=(CodePointAndSize&&) = default;

  /// Returns the code point this represents.
  constexpr uint32_t code_point() const { return code_point_ & kCodePointMask; }

  /// Returns the number of bytes required to encode this codepoint.
  constexpr size_t size() const {
    return (code_point_ & kSizeMask) >> kSizeShift;
  }

 private:
  static constexpr size_t kSizeBits = 4;
  static constexpr uint32_t kCodePointMask = ~0U >> kSizeBits;
  static constexpr uint32_t kSizeMask = ~kCodePointMask;
  static constexpr size_t kSizeShift = sizeof(uint32_t) * 8 - kSizeBits;
  uint32_t code_point_;
};

/// @}

}  // namespace utf

namespace utf8 {

/// @submodule{pw_string,utf8}

/// @brief Reads the first code point from a UTF-8 encoded `str`.
///
/// This is a very basic decoder without much thought for performance and very
/// basic validation that the correct number of bytes are available and that
/// each byte of a multibyte sequence has a continuation character. See
/// `pw::utf8::EncodeCharacter()` for encoding details.
///
/// @returns @Result{the decoded code point and the number of bytes read}
/// * @OK: The decoded code point and the number of bytes read.
/// * @INVALID_ARGUMENT: The string was empty or malformed.
/// * @OUT_OF_RANGE: The decoded code point was not in the valid range.
constexpr pw::Result<utf::CodePointAndSize> ReadCodePoint(
    std::string_view str) {
  if (str.empty()) {
    return pw::Status::InvalidArgument();
  }

  const uint8_t leading_byte = static_cast<uint8_t>(str.front());
  size_t byte_count = 0;
  uint32_t code_point = 0xFFFFFFFFu;

  if (leading_byte <= 0x7F) {
    byte_count = 1;
    // b0xxx xxxx
    code_point = leading_byte;
  } else if (leading_byte <= 0xDF) {
    byte_count = 2;
    if (str.size() < byte_count) {
      return pw::Status::InvalidArgument();
    }
    // b110x xxxx 10xx xxxx
    if ((str[1] & 0xC0) != 0x80) {
      // Invalid continuation
      return pw::Status::InvalidArgument();
    }
    code_point = (static_cast<uint32_t>(str[0] & 0x1F) << 6) +
                 static_cast<uint32_t>(str[1] & 0x3F);
  } else if (leading_byte <= 0xEF) {
    byte_count = 3;
    if (str.size() < byte_count) {
      return pw::Status::InvalidArgument();
    }
    if ((str[1] & 0xC0) != 0x80 || (str[2] & 0xC0) != 0x80) {
      // Invalid continuation
      return pw::Status::InvalidArgument();
    }
    // b1110 xxxx 10xx xxxx 10xx xxxx
    code_point = (static_cast<uint32_t>(str[0] & 0x0F) << 12) +
                 (static_cast<uint32_t>(str[1] & 0x3F) << 6) +
                 static_cast<uint32_t>(str[2] & 0x3F);
  } else if (leading_byte <= 0xF7) {
    byte_count = 4;
    if (str.size() < byte_count) {
      return pw::Status::InvalidArgument();
    }
    if ((str[1] & 0xC0) != 0x80 || (str[2] & 0xC0) != 0x80 ||
        (str[3] & 0xC0) != 0x80) {
      // Invalid continuation
      return pw::Status::InvalidArgument();
    }
    // b1111 0xxx 10xx xxxx 10xx xxxx 10xx xxxx
    code_point = (static_cast<uint32_t>(str[0] & 0x07) << 18) +
                 (static_cast<uint32_t>(str[1] & 0x3F) << 12) +
                 (static_cast<uint32_t>(str[2] & 0x3F) << 6) +
                 static_cast<uint32_t>(str[3] & 0x3F);
  } else {
    return pw::Status::InvalidArgument();
  }

  // Validate the decoded value.
  if (utf::IsValidCodepoint(code_point)) {
    return utf::CodePointAndSize(code_point, byte_count);
  }

  return pw::Status::OutOfRange();
}

/// Determines if `str` is a valid UTF-8 string.
constexpr bool IsStringValid(std::string_view str) {
  while (!str.empty()) {
    auto rslt = utf8::ReadCodePoint(str);
    if (!rslt.ok() || !utf::IsValidCharacter(rslt->code_point())) {
      return false;
    }
    str = str.substr(rslt->size());
  }
  return true;
}

/// Encapsulates the result of encoding a single code point as UTF-8.
class EncodedCodePoint {
 public:
  constexpr EncodedCodePoint(uint32_t size, std::array<char, 4> data)
      : size_(size), data_(std::move(data)) {}
  constexpr EncodedCodePoint(EncodedCodePoint&& encoded) = default;
  constexpr std::string_view as_view() const { return {data_.data(), size_}; }

 private:
  uint32_t size_;
  std::array<char, 4> data_;
};

/// @brief Encodes a single code point as UTF-8.
///
/// UTF-8 encodes as 1-4 bytes from a range of `[0, 0x10FFFF]`.
///
/// 1-byte encoding has a top bit of zero:
/// @code
/// [0, 0x7F] 1-bytes: b0xxx xxxx
/// @endcode
/// N-bytes sequences are denoted by annotating the top N+1 bits of the leading
/// byte and then using a 2-bit continuation marker on the following bytes.
/// @code
/// [0x00080, 0x0007FF] 2-bytes: b110x xxxx 10xx xxxx
/// [0x00800, 0x00FFFF] 3-bytes: b1110 xxxx 10xx xxxx 10xx xxxx
/// [0x10000, 0x10FFFF] 4-bytes: b1111 0xxx 10xx xxxx 10xx xxxx 10xx xxxx
/// @endcode
///
/// @returns @Result{the codepoint encoded as UTF-8}
/// * @OUT_OF_RANGE: The code point was not in the valid range for UTF-8
///   encoding.
constexpr Result<EncodedCodePoint> EncodeCodePoint(uint32_t code_point) {
  if (code_point <= 0x7F) {
    return EncodedCodePoint{1, {static_cast<char>(code_point)}};
  }
  if (code_point <= 0x7FF) {
    return EncodedCodePoint{2,
                            {static_cast<char>(0xC0 | (code_point >> 6)),
                             static_cast<char>(0x80 | (code_point & 0x3F))}};
  }
  if (code_point <= 0xFFFF) {
    return EncodedCodePoint{
        3,
        {static_cast<char>(0xE0 | (code_point >> 12)),
         static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)),
         static_cast<char>(0x80 | (code_point & 0x3F))}};
  }
  if (code_point <= 0x10FFFF) {
    return EncodedCodePoint{
        4,
        {static_cast<char>(0xF0 | (code_point >> 18)),
         static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)),
         static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)),
         static_cast<char>(0x80 | (code_point & 0x3F))}};
  }

  return pw::Status::OutOfRange();
}

/// Helper that writes a code point to the provided `pw::StringBuilder`.
Status WriteCodePoint(uint32_t code_point, pw::StringBuilder& output);

/// @}

}  // namespace utf8

}  // namespace pw
