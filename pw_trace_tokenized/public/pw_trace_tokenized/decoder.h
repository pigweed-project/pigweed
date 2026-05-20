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

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "pw_result/result.h"
#include "pw_stream/stream.h"
#include "pw_tokenizer/detokenize.h"
#include "pw_trace_tokenized/trace_tokenized.h"

namespace pw::trace {

/// @module{pw_trace_tokenized}

/// A decoded trace event.
///
/// See also `pw_trace_tokenized_TraceEvent`.
struct DecodedEvent {
  EventType type = EventType::PW_TRACE_EVENT_TYPE_INVALID;
  // TODO: https://pwbug.dev/448489618 - The 'flag' field in the token is
  // ostensibly a decimal integer string, but could actually be any arbitrary C
  // expression that evaluates to an integer. Rather than try to parse it and
  // risk failing, simply return it as a string for now.
  std::string flags_str;
  std::string module;
  std::string group;
  std::string label;
  std::string data_fmt;
  uint64_t timestamp_usec{};
  std::optional<uint64_t> trace_id;
  std::vector<std::byte> data;
};

/// TokenizedDecoder can decode pw_trace_tokenized-encoded trace events.
class TokenizedDecoder {
 public:
  /// Creates a new event decoder.
  ///
  /// @param[in] detokenizer The Detokenizer responsible for converting
  ///            embedded tokens back to strings. This must reference a token
  ///            database with valid tokens for the originating firmware.
  /// @param[in] ticks_per_sec The rate at which the pw_trace clock ticks for
  ///            the target device/firmware. This is typically retrieved using
  ///            the `TraceService::GetClockParameters()` RPC method.
  TokenizedDecoder(tokenizer::Detokenizer& detokenizer, uint64_t ticks_per_sec)
      : detokenizer_(detokenizer), ticks_per_sec_(ticks_per_sec) {}

  /// Sets the current time offset; the base for future decoded event
  /// timestamps.
  void SetTimeOffset(uint64_t time_offset) { last_timestamp_us_ = time_offset; }

  /// Reads from reader a size-prefixed DecodedEvent as returned by
  /// pw_trace_tokenized:transfer_handler.
  ///
  /// @param[in] reader The stream from which size-prefixed Events are read and
  ///            decoded.
  /// @returns @Result{a decoded event}
  /// * @OUT_OF_RANGE: The stream hit EOF before reading an event.
  /// * @DATA_LOSS: The event could not be decoded.
  Result<DecodedEvent> ReadSizePrefixed(stream::Reader& reader);

  /// Decodes a DecodedEvent from a span of data.
  ///
  /// @param[in] data The byte span which a DecodedEvent is decoded.
  /// @note All of the data is assumed to be part of a single DecodedEvent.
  ///       Don't use this method if the data may include multiple encoded
  ///       DecodedEvents.
  /// @returns @Result{a decoded event}
  /// * @OUT_OF_RANGE: The data was truncated.
  /// * @DATA_LOSS: The event could not be decoded.
  Result<DecodedEvent> Decode(ConstByteSpan data);

 private:
  tokenizer::Detokenizer& detokenizer_;
  const uint64_t ticks_per_sec_;
  uint64_t last_timestamp_us_ = 0;

  uint64_t usec_per_tick() const { return 1'000'000 / ticks_per_sec_; }
};

/// @}

}  // namespace pw::trace
