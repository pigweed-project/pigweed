// Copyright 2025 The Pigweed Authors
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

#include "pw_trace_tokenized/decoder.h"

#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>

#include "lib/stdcompat/utility.h"
#include "pw_bytes/endian.h"
#include "pw_bytes/span.h"
#include "pw_log/log.h"
#include "pw_status/status.h"
#include "pw_status/try.h"
#include "pw_stream/memory_stream.h"
#include "pw_varint/stream.h"

namespace pw::trace {
namespace {

using cpp23::to_underlying;

constexpr std::string_view kDomain = "trace";
constexpr const char* kTokenLabelDataFmt = "@pw_trace_tokenized_token_label";

// Token string: "event_type|flag|module|group|label|<optional DATA_FMT>"
enum class TokenIdx {
  kEventType = 0,
  kFlag = 1,
  kModule = 2,
  kGroup = 3,
  kLabel = 4,
  kDataFmt = 5,  // optional
};

template <typename T>
pw::Result<T> ReadInt(stream::Reader& reader) {
  static_assert(std::is_trivially_copyable_v<T>);
  std::array<std::byte, sizeof(T)> buffer;
  PW_TRY(reader.ReadExact(buffer));
  return bytes::ReadInOrder<T>(endian::little, buffer);
}

EventType ParseEventType(std::string_view type_str) {
  if (type_str == "PW_TRACE_EVENT_TYPE_INSTANT")
    return EventType::PW_TRACE_EVENT_TYPE_INSTANT;
  if (type_str == "PW_TRACE_EVENT_TYPE_INSTANT_GROUP")
    return EventType::PW_TRACE_EVENT_TYPE_INSTANT_GROUP;
  if (type_str == "PW_TRACE_EVENT_TYPE_ASYNC_START")
    return EventType::PW_TRACE_EVENT_TYPE_ASYNC_START;
  if (type_str == "PW_TRACE_EVENT_TYPE_ASYNC_STEP")
    return EventType::PW_TRACE_EVENT_TYPE_ASYNC_STEP;
  if (type_str == "PW_TRACE_EVENT_TYPE_ASYNC_END")
    return EventType::PW_TRACE_EVENT_TYPE_ASYNC_END;
  if (type_str == "PW_TRACE_EVENT_TYPE_DURATION_START")
    return EventType::PW_TRACE_EVENT_TYPE_DURATION_START;
  if (type_str == "PW_TRACE_EVENT_TYPE_DURATION_END")
    return EventType::PW_TRACE_EVENT_TYPE_DURATION_END;
  if (type_str == "PW_TRACE_EVENT_TYPE_DURATION_GROUP_START")
    return EventType::PW_TRACE_EVENT_TYPE_DURATION_GROUP_START;
  if (type_str == "PW_TRACE_EVENT_TYPE_DURATION_GROUP_END")
    return EventType::PW_TRACE_EVENT_TYPE_DURATION_GROUP_END;
  return EventType::PW_TRACE_EVENT_TYPE_INVALID;
}

bool HasTraceId(EventType event_type) {
  switch (event_type) {
    case EventType::PW_TRACE_EVENT_TYPE_ASYNC_START:
    case EventType::PW_TRACE_EVENT_TYPE_ASYNC_STEP:
    case EventType::PW_TRACE_EVENT_TYPE_ASYNC_END:
      return true;
    case EventType::PW_TRACE_EVENT_TYPE_INVALID:
    case EventType::PW_TRACE_EVENT_TYPE_INSTANT:
    case EventType::PW_TRACE_EVENT_TYPE_INSTANT_GROUP:
    case EventType::PW_TRACE_EVENT_TYPE_DURATION_START:
    case EventType::PW_TRACE_EVENT_TYPE_DURATION_END:
    case EventType::PW_TRACE_EVENT_TYPE_DURATION_GROUP_START:
    case EventType::PW_TRACE_EVENT_TYPE_DURATION_GROUP_END:
      return false;
  }
  return false;
}

std::vector<std::string_view> Split(std::string_view input, char delimiter) {
  std::vector<std::string_view> result;
  while (!input.empty()) {
    size_t found_pos = input.find(delimiter);
    if (found_pos == input.npos) {
      break;
    }
    result.emplace_back(input.substr(0, found_pos));
    input = input.substr(found_pos + 1);
  }
  result.emplace_back(input);
  return result;
}

Result<std::string_view> GetUniqueString(
    const tokenizer::DetokenizedString& detok_result, uint32_t token) {
  if (detok_result.matches().empty()) {
    PW_LOG_WARN("Failed to detokenize token: 0x%08x (no matches)",
                static_cast<unsigned int>(token));
    return Status::DataLoss();
  }
  if (detok_result.matches().size() > 1) {
    PW_LOG_WARN("Token collision for token 0x%08x: %zu matches",
                static_cast<unsigned int>(token),
                detok_result.matches().size());
    return Status::DataLoss();
  }
  return detok_result.BestString();
}

}  // namespace

Result<DecodedEvent> TokenizedDecoder::ReadSizePrefixed(
    stream::Reader& reader) {
  // Trace entry as returned via pw_trace_tokenized:transfer_handler.
  PW_TRY_ASSIGN(uint8_t entry_size, ReadInt<uint8_t>(reader));

  std::array<std::byte, std::numeric_limits<uint8_t>::max()> buffer;
  const ByteSpan entry_buf = span(buffer).first(entry_size);

  PW_TRY(reader.Read(entry_buf));
  return Decode(entry_buf);
}

Result<DecodedEvent> TokenizedDecoder::Decode(ConstByteSpan data) {
  stream::MemoryReader reader(data);

  // Read token
  PW_TRY_ASSIGN(uint32_t token, ReadInt<uint32_t>(reader));

  // Detokenize
  tokenizer::DetokenizedString detok_result =
      detokenizer_.Detokenize(ObjectAsBytes(token), kDomain);
  PW_TRY_ASSIGN(std::string_view token_string,
                GetUniqueString(detok_result, token));

  // Split token string:
  // "event_type|flag|module|group|label|<optional DATA_FMT>"
  std::vector<std::string_view> token_string_values = Split(token_string, '|');
  if (token_string_values.size() < 5) {
    PW_LOG_WARN("Too few token values: %zu", token_string_values.size());
    return Status::DataLoss();
  }
  DecodedEvent event{};
  event.type =
      ParseEventType(token_string_values[to_underlying(TokenIdx::kEventType)]);
  event.module = token_string_values[to_underlying(TokenIdx::kModule)];
  event.group = token_string_values[to_underlying(TokenIdx::kGroup)];
  event.label = token_string_values[to_underlying(TokenIdx::kLabel)];

  // TODO: https://pwbug.dev/448489618 - The 'flag' field in the token is
  // ostensibly a decimal integer string, but could actually be any arbitrary C
  // expression that evaluates to an integer. Rather than try to parse it and
  // risk failing, simply return it as a string for now.
  event.flags_str = token_string_values[to_underlying(TokenIdx::kFlag)];

  const bool has_data =
      (token_string_values.size() > to_underlying(TokenIdx::kDataFmt));
  if (has_data) {
    event.data_fmt = token_string_values[to_underlying(TokenIdx::kDataFmt)];
  }

  // Read time
  uint64_t time_delta;
  PW_TRY(varint::Read(reader, &time_delta));
  last_timestamp_us_ +=
      (time_delta / ticks_per_sec_) * 1000000 +
      ((time_delta % ticks_per_sec_) * 1000000) / ticks_per_sec_;
  event.timestamp_usec = last_timestamp_us_;

  // Trace ID
  if (HasTraceId(event.type)) {
    uint64_t trace_id_val;
    PW_TRY(varint::Read(reader, &trace_id_val));
    event.trace_id = trace_id_val;
  }

  // Data
  if (has_data) {
    if (event.data_fmt == kTokenLabelDataFmt) {
      if (reader.ConservativeReadLimit() != 4) {
        PW_LOG_WARN("Mismatched data size for %s: expected 4, got %zu",
                    kTokenLabelDataFmt,
                    reader.ConservativeReadLimit());
        return Status::DataLoss();
      }
      PW_TRY_ASSIGN(uint32_t label_token, ReadInt<uint32_t>(reader));
      tokenizer::DetokenizedString detok_label =
          detokenizer_.Detokenize(ObjectAsBytes(label_token), kDomain);
      PW_TRY_ASSIGN(std::string_view label_string,
                    GetUniqueString(detok_label, label_token));
      event.label = label_string;
      event.data_fmt.clear();
    } else {
      size_t n = reader.ConservativeReadLimit();
      event.data.resize(n);
      PW_TRY(reader.Read(event.data));
    }
  }

  return event;
}

}  // namespace pw::trace
