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

#include "pw_tokenizer/internal/decode.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <string>

#include "pw_varint/varint.h"

namespace pw::tokenizer {
namespace {

// Functions for parsing a printf format specifier.
size_t SkipFlags(const char* str) {
  size_t i = 0;
  while (str[i] == '-' || str[i] == '+' || str[i] == '#' || str[i] == ' ' ||
         str[i] == '0') {
    i += 1;
  }
  return i;
}

size_t SkipAsteriskOrInteger(const char* str) {
  if (str[0] == '*') {
    return 1;
  }

  size_t i = (str[0] == '-' || str[0] == '+') ? 1 : 0;

  while (std::isdigit(str[i])) {
    i += 1;
  }
  return i;
}

std::array<char, 2> ReadLengthModifier(const char* str) {
  // Check for ll or hh.
  if (str[0] == str[1] && (str[0] == 'l' || str[0] == 'h')) {
    return {str[0], str[1]};
  }
  if (std::strchr("hljztL", str[0]) != nullptr) {
    return {str[0]};
  }
  return {};
}

// Returns the error message that is used in place of a decoded arg when an
// error occurs.
std::string ErrorMessage(ArgStatus status,
                         std::string_view spec,
                         std::string_view value) {
  const char* message;
  if (status.HasError(ArgStatus::kSkipped)) {
    message = "SKIPPED";
  } else if (status.HasError(ArgStatus::kMissing)) {
    message = "MISSING";
  } else if (status.HasError(ArgStatus::kDecodeError)) {
    message = "ERROR";
  } else {
    message = "INTERNAL ERROR";
  }

  std::string result(PW_TOKENIZER_ARG_DECODING_ERROR_PREFIX);
  result.append(spec);
  result.push_back(' ');
  result.append(message);

  if (!value.empty()) {
    result.push_back(' ');
    result.push_back('(');
    result.append(value);
    result.push_back(')');
  }

  result.append(PW_TOKENIZER_ARG_DECODING_ERROR_SUFFIX);
  return result;
}

DecodedArg MakeErrorArg(ArgStatus status,
                        std::string_view spec,
                        size_t total_arguments_size,
                        size_t bytes_consumed) {
  return DecodedArg(status,
                    spec,
                    status.HasError(ArgStatus::kDecodeError)
                        ? total_arguments_size
                        : bytes_consumed);
}
}  // namespace

DecodedArg::DecodedArg(ArgStatus error,
                       std::string_view spec,
                       size_t raw_size_bytes,
                       std::string_view value)
    : value_(ErrorMessage(error, spec, value)),
      spec_(spec),
      raw_data_size_bytes_(raw_size_bytes),
      status_(error) {}

StringSegment StringSegment::ParseFormatSpec(const char* format) {
  if (format[0] != '%' || format[1] == '\0') {
    return StringSegment();
  }

  // Parse the format specifier.
  size_t i = 1;

  // Skip the flags.
  i += SkipFlags(&format[i]);

  // Skip the field width.
  const size_t width_asterisk_pos = format[i] == '*' ? i : kNoAsterisk;
  i += SkipAsteriskOrInteger(&format[i]);

  // Skip the precision.
  size_t precision_asterisk_pos = kNoAsterisk;
  if (format[i] == '.') {
    i += 1;
    if (format[i] == '*') {
      precision_asterisk_pos = i;
    }
    i += SkipAsteriskOrInteger(&format[i]);
  }

  // Read the length modifier.
  const std::array<char, 2> length = ReadLengthModifier(&format[i]);
  i += (length[0] == '\0' ? 0 : 1) + (length[1] == '\0' ? 0 : 1);

  // Read the conversion specifier.
  const char spec = format[i];

  Type type;
  if (spec == 's') {
    type = kString;
  } else if (spec == 'c' || spec == 'd' || spec == 'i') {
    type = kSignedInt;
  } else if (std::strchr("oxXup", spec) != nullptr) {
    // The source size matters for unsigned integers because they need to be
    // masked off to their correct length, since zig-zag decode sign extends.
    // TODO(hepler): 64-bit targets likely have 64-bit l, j, z, and t. Also, p
    // needs to be 64-bit on these targets.
    type = length[0] == 'j' || length[1] == 'l' ? kUnsigned64 : kUnsigned32;
  } else if (std::strchr("fFeEaAgG", spec) != nullptr) {
    type = kFloatingPoint;
  } else if (spec == '%' && i == 1) {
    type = kPercent;
  } else {
    return StringSegment();
  }

  return StringSegment(std::string_view(format, i + 1),
                       type,
                       VarargSize(length, spec),
                       width_asterisk_pos,
                       precision_asterisk_pos);
}

StringSegment::ArgSize StringSegment::VarargSize(std::array<char, 2> length,
                                                 char spec) {
  // Use pointer size for %p or any other type (for which this doesn't matter).
  if (std::strchr("cdioxXu", spec) == nullptr) {
    return VarargSize<void*>();
  }
  if (length[0] == 'l') {
    return length[1] == 'l' ? VarargSize<long long>() : VarargSize<long>();
  }
  if (length[0] == 'j') {
    return VarargSize<intmax_t>();
  }
  if (length[0] == 'z') {
    return VarargSize<size_t>();
  }
  if (length[0] == 't') {
    return VarargSize<ptrdiff_t>();
  }
  return VarargSize<int>();
}

DecodedArg StringSegment::DecodeString(const span<const uint8_t>& arguments,
                                       const char* format,
                                       size_t raw_size_bytes) const {
  if (arguments.empty()) {
    return DecodedArg(ArgStatus::kMissing, text_);
  }

  ArgStatus status =
      (arguments[0] & 0x80u) == 0u ? ArgStatus::kOk : ArgStatus::kTruncated;

  const uint_fast8_t size = arguments[0] & 0x7Fu;

  if (arguments.size() - 1 < size) {
    status.Update(ArgStatus::kDecodeError);
    span<const uint8_t> arg_val = arguments.subspan(1);
    return DecodedArg(
        status,
        text_,
        arguments.size(),
        {reinterpret_cast<const char*>(arg_val.data()), arg_val.size()});
  }

  std::string value(reinterpret_cast<const char*>(arguments.data() + 1), size);

  if (status.HasError(ArgStatus::kTruncated)) {
    value.append("[...]");
  }

  return DecodedArg::FromValue(
      text_.c_str(), format, value.c_str(), raw_size_bytes + 1 + size, status);
}

DecodedArg StringSegment::DecodeInteger(const span<const uint8_t>& arguments,
                                        const char* format,
                                        size_t raw_size_bytes) const {
  if (arguments.empty()) {
    return DecodedArg(ArgStatus::kMissing, text_);
  }

  int64_t value;
  const size_t bytes = varint::Decode(as_bytes(arguments), &value);

  if (bytes == 0u) {
    return DecodedArg(ArgStatus::kDecodeError,
                      text_,
                      std::min(varint::kMaxVarint64SizeBytes,
                               static_cast<size_t>(arguments.size())));
  }

  // Unsigned ints need to be masked to their bit width due to sign extension.
  if (type_ == kUnsigned32) {
    value &= 0xFFFFFFFFu;
  }

  if (local_size_ == k32Bit) {
    return DecodedArg::FromValue(text_.c_str(),
                                 format,
                                 static_cast<uint32_t>(value),
                                 raw_size_bytes + bytes);
  }
  return DecodedArg::FromValue(
      text_.c_str(), format, value, raw_size_bytes + bytes);
}

DecodedArg StringSegment::DecodeFloatingPoint(
    const span<const uint8_t>& arguments,
    const char* format,
    size_t raw_size_bytes) const {
  static_assert(sizeof(float) == 4u);
  if (arguments.size() < sizeof(float)) {
    return DecodedArg(ArgStatus::kMissing, text_);
  }

  float value;
  std::memcpy(&value, arguments.data(), sizeof(value));
  return DecodedArg::FromValue(
      text_.c_str(), format, value, raw_size_bytes + sizeof(value));
}

DecodedArg StringSegment::DecodeStringOrNumber(
    const span<const uint8_t>& arguments,
    const char* format,
    size_t raw_size_bytes) const {
  if (type_ == kString) {
    return DecodeString(arguments, format, raw_size_bytes);
  }
  if (type_ == kSignedInt || type_ == kUnsigned32 || type_ == kUnsigned64) {
    return DecodeInteger(arguments, format, raw_size_bytes);
  }
  if (type_ == kFloatingPoint) {
    return DecodeFloatingPoint(arguments, format, raw_size_bytes);
  }
  return DecodedArg(ArgStatus::kDecodeError, text_);
}

DecodedArg StringSegment::Decode(const span<const uint8_t>& arguments) const {
  if (type_ == kLiteral) {
    return DecodedArg(text_);
  }
  if (type_ == kPercent) {
    return DecodedArg("%");
  }

  if (width_asterisk_pos_ == kNoAsterisk &&
      precision_asterisk_pos_ == kNoAsterisk) {
    return DecodeStringOrNumber(arguments, text_.c_str(), 0);
  }

  return DecodeArgWithAsterisks(arguments);
}

DecodedArg StringSegment::DecodeArgWithAsterisks(
    const span<const uint8_t>& arguments) const {
  size_t bytes_consumed = 0;
  ArgStatus status = ArgStatus::kOk;
  span<const uint8_t> remaining_arguments = arguments;

  auto decode_asterisk =
      [&remaining_arguments, &status, &bytes_consumed](int64_t& val) -> bool {
    if (remaining_arguments.empty()) {
      status.Update(ArgStatus::kMissing);
      return false;
    }
    size_t bytes = varint::Decode(as_bytes(remaining_arguments), &val);
    if (bytes == 0u) {
      status.Update(ArgStatus::kDecodeError);
      return false;
    }
    bytes_consumed += bytes;
    remaining_arguments = remaining_arguments.subspan(bytes);
    return true;
  };

  int64_t width_val = 0;
  if (width_asterisk_pos_ != kNoAsterisk && !decode_asterisk(width_val)) {
    return MakeErrorArg(status, text_, arguments.size(), bytes_consumed);
  }
  int64_t precision_val = 0;
  if (precision_asterisk_pos_ != kNoAsterisk &&
      !decode_asterisk(precision_val)) {
    return MakeErrorArg(status, text_, arguments.size(), bytes_consumed);
  }

  std::string resolved = text_;
  if (precision_asterisk_pos_ != kNoAsterisk) {
    // Ignore negative precision, as required by the C99 standard.
    if (precision_val < 0) {
      resolved.erase(precision_asterisk_pos_ - 1, 2);
    } else {
      resolved.replace(
          precision_asterisk_pos_, 1, std::to_string(precision_val));
    }
  }
  if (width_asterisk_pos_ != kNoAsterisk) {
    resolved.replace(width_asterisk_pos_, 1, std::to_string(width_val));
  }

  return DecodeStringOrNumber(
      remaining_arguments, resolved.c_str(), bytes_consumed);
}

DecodedArg StringSegment::Skip() const {
  switch (type_) {
    case kLiteral:
      return DecodedArg(text_);
    case kPercent:
      return DecodedArg("%");
    case kString:
    case kSignedInt:
    case kUnsigned32:
    case kUnsigned64:
    case kFloatingPoint:
    default:
      return DecodedArg(ArgStatus::kSkipped, text_);
  }
}

std::string DecodedFormatString::value() const {
  std::string output;

  for (const DecodedArg& arg : segments_) {
    output.append(arg.ok() ? arg.value() : arg.spec());
  }

  return output;
}

std::string DecodedFormatString::value_with_errors() const {
  std::string output;

  for (const DecodedArg& arg : segments_) {
    output.append(arg.value());
  }

  return output;
}

size_t DecodedFormatString::argument_count() const {
  return static_cast<size_t>(
      std::count_if(segments_.begin(), segments_.end(), [](const auto& arg) {
        return !arg.spec().empty();
      }));
}

size_t DecodedFormatString::decoding_errors() const {
  return static_cast<size_t>(
      std::count_if(segments_.begin(), segments_.end(), [](const auto& arg) {
        return !arg.ok();
      }));
}

FormatString::FormatString(const char* format) {
  const char* text_start = format;

  while (format[0] != '\0') {
    if (StringSegment spec = StringSegment::ParseFormatSpec(format);
        !spec.empty()) {
      // Add the text segment seen so far (if any).
      if (text_start < format) {
        segments_.emplace_back(std::string_view(
            text_start, static_cast<size_t>(format - text_start)));
      }

      // Move along the index and text segment start.
      format += spec.text().size();
      text_start = format;

      // Add the format specifier that was just found.
      segments_.push_back(std::move(spec));
    } else {
      format += 1;
    }
  }

  if (text_start < format) {
    segments_.emplace_back(
        std::string_view(text_start, static_cast<size_t>(format - text_start)));
  }
}

DecodedFormatString FormatString::Format(span<const uint8_t> arguments) const {
  std::vector<DecodedArg> results;
  bool skip = false;

  for (const auto& segment : segments_) {
    if (skip) {
      results.push_back(segment.Skip());
    } else {
      results.push_back(segment.Decode(arguments));
      arguments = arguments.subspan(results.back().raw_size_bytes());

      // If an error occurred, skip decoding the remaining arguments.
      if (!results.back().ok()) {
        skip = true;
      }
    }
  }

  return DecodedFormatString(std::move(results), arguments.size());
}

std::string FormatString::text() const {
  std::string full_string;
  for (const StringSegment& seg : segments_) {
    full_string.append(seg.text());
  }
  return full_string;
}

}  // namespace pw::tokenizer
