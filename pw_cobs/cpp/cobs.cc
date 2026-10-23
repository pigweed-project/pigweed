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

#include "pw_cobs/cobs.h"

#include <algorithm>
#include <cstring>

namespace pw::cobs {
namespace {

constexpr std::byte kFrameDelimiter{0x00};
constexpr uint8_t kMaxRunCode = 0xFF;

size_t FindFirstZero(ConstByteSpan span) {
  const void* zero =
      !span.empty() ? std::memchr(span.data(), 0, span.size()) : nullptr;
  if (zero == nullptr) {
    return span.size();
  }
  return static_cast<size_t>(static_cast<const std::byte*>(zero) - span.data());
}

}  // namespace

ConstByteSpan Streamer::Write(ConstByteSpan data) {
  if (buffer_.size() <= write_pos_) {
    return data;
  }
  while (!data.empty()) {
    // If the active block reached 254 non-zero bytes on the previous Write()
    // (or loop iteration), close it with 0xFF before consuming more payload.
    if (code_ == kMaxRunCode) {
      if (write_pos_ + 1 >= buffer_.size()) {
        break;
      }
      buffer_[code_pos_] = std::byte{kMaxRunCode};
      code_pos_ = write_pos_++;
      code_ = 1;
    }

    // Reserve at least 1 trailing byte for the closing 0x00 delimiter.
    const size_t avail_out = (buffer_.size() - 1) - write_pos_;
    if (avail_out == 0) {
      break;
    }

    const size_t max_run = std::min(
        {data.size(), avail_out, static_cast<size_t>(kMaxRunCode - code_)});
    const size_t non_zero = FindFirstZero(data.first(max_run));
    const bool hit_zero =
        (non_zero < max_run) ||
        ((non_zero < data.size()) && (code_ + non_zero < kMaxRunCode) &&
         (data[non_zero] == kFrameDelimiter));
    const bool block_completes = hit_zero || (code_ + non_zero == kMaxRunCode);

    // Ensure a 0x00 byte never leaves zero bytes for the next block's code
    // byte, and never start an incomplete tail block once at least one block
    // has already completed in `buffer_` (`code_pos_ > lead_offset_`).
    if ((hit_zero && write_pos_ + non_zero + 1 >= buffer_.size()) ||
        (code_pos_ > lead_offset_ && !block_completes)) {
      break;
    }

    if (non_zero > 0) {
      if (buffer_.data() + write_pos_ != data.data()) {
        std::memmove(buffer_.data() + write_pos_, data.data(), non_zero);
      }
      write_pos_ += non_zero;
      code_ = static_cast<uint8_t>(code_ + non_zero);
      data = data.subspan(non_zero);
    }

    if (hit_zero) {
      buffer_[code_pos_] = static_cast<std::byte>(code_);
      code_pos_ = write_pos_++;
      code_ = 1;
      data = data.subspan(1);
    }
  }
  return data;
}

ByteSpan Streamer::Flush() {
  if (code_ == kMaxRunCode) {
    buffer_[code_pos_] = std::byte{kMaxRunCode};
    code_pos_ = write_pos_;
    code_ = 1;
  }
  if (code_pos_ <= lead_offset_) {
    return {};
  }
  const ByteSpan flushed = buffer_.first(code_pos_);
  lead_offset_ = 0;
  code_pos_ = 0;
  write_pos_ = 1;
  flushed_any_ = true;
  return flushed;
}

ByteSpan Streamer::Finish() {
  if (buffer_.size() <= write_pos_) {
    Reset();
    return {};
  }
  buffer_[code_pos_] = static_cast<std::byte>(code_);
  buffer_[write_pos_++] = kFrameDelimiter;
  const ByteSpan final_chunk = buffer_.first(write_pos_);
  Reset();
  return final_chunk;
}

Result<ByteSpan> Encode(ConstByteSpan input, ByteSpan output) {
  if (output.size() < 2) {
    return Status::ResourceExhausted();
  }
  Streamer streamer(output);
  size_t total = 0;
  while (!input.empty()) {
    input = streamer.Write(input);
    if (!input.empty()) {
      const ByteSpan chunk = streamer.Flush();
      if (chunk.empty()) {
        return Status::ResourceExhausted();
      }
      total += chunk.size();
      streamer.buffer_ = output.subspan(total);
    }
  }
  const ByteSpan tail = streamer.Finish();
  return output.first(total + tail.size());
}

Result<ByteSpan> Decode(ConstByteSpan encoded, ByteSpan output) {
  if (encoded.size() < 2 || encoded.front() == kFrameDelimiter ||
      encoded.back() != kFrameDelimiter) {
    return Status::DataLoss();
  }
  Decoder decoder(output);
  ConstByteSpan remaining;
  Result<ByteSpan> result = decoder.ProcessChunk(encoded, remaining);
  if (!remaining.empty()) {
    return Status::DataLoss();
  }
  return result;
}

Result<ByteSpan> EncodeInPlace(ByteSpan buffer, size_t payload_size) {
  const size_t max_encoded = MaxEncodedSize(payload_size);
  if (buffer.size() < max_encoded) {
    return Status::ResourceExhausted();
  }
  buffer = buffer.first(max_encoded);
  const size_t offset = buffer.size() - 1 - payload_size;
  if (payload_size > 0 && offset != 1) {
    std::memmove(buffer.data() + offset, buffer.data() + 1, payload_size);
  }
  return Encode(buffer.subspan(offset, payload_size), buffer);
}

void Decoder::Clear() {
  write_pos_ = 0;
  remaining_in_run_ = 0;
  code_ = 0;
  overflowed_ = false;
}

Result<ByteSpan> Decoder::Process(std::byte b) {
  ConstByteSpan remaining;
  return ProcessChunk(ConstByteSpan(&b, 1), remaining);
}

Result<ByteSpan> Decoder::ProcessChunk(ConstByteSpan input,
                                       ConstByteSpan& remaining) {
  while (!input.empty()) {
    if (remaining_in_run_ > 0) {
      const size_t step = std::min(input.size(), remaining_in_run_);
      const size_t zero_pos = FindFirstZero(input.first(step));
      if (zero_pos < step) {
        remaining = input.subspan(zero_pos + 1);
        Clear();
        return Status::DataLoss();
      }
      if (!overflowed_ && decode_buffer_.size() - write_pos_ >= step) {
        std::memcpy(decode_buffer_.data() + write_pos_, input.data(), step);
        write_pos_ += step;
      } else {
        overflowed_ = true;
      }
      remaining_in_run_ -= step;
      input = input.subspan(step);
      continue;
    }

    const uint8_t code = std::to_integer<uint8_t>(input[0]);
    input = input.subspan(1);

    if (code == 0) {
      if (code_ == 0) {
        continue;
      }
      remaining = input;
      const size_t total_len = write_pos_;
      const bool had_overflow = overflowed_;
      Clear();
      return had_overflow ? Result<ByteSpan>(Status::ResourceExhausted())
                          : Result<ByteSpan>(decode_buffer_.first(total_len));
    }

    if (code_ != 0 && code_ != 0xFF) {
      if (!overflowed_ && write_pos_ < decode_buffer_.size()) {
        decode_buffer_[write_pos_++] = kFrameDelimiter;
      } else {
        overflowed_ = true;
      }
    }
    code_ = code;
    remaining_in_run_ = code - 1;
  }

  remaining = {};
  return Status::Unavailable();
}

}  // namespace pw::cobs
