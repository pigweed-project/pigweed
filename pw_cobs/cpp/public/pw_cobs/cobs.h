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

/// @file pw_cobs/cobs.h
/// Consistent Overhead Byte Stuffing (COBS) encoding and decoding primitives.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "pw_assert/assert.h"
#include "pw_bytes/span.h"
#include "pw_containers/ptr_iterator.h"
#include "pw_containers/storage.h"
#include "pw_result/result.h"
#include "pw_status/status.h"

namespace pw::cobs {

/// @module{pw_cobs}

/// Maximum number of contiguous non-zero bytes in a single COBS block.
inline constexpr size_t kMaxRunLength = 254;

/// Returns the worst-case encoded frame size in bytes for a payload of
/// @p unencoded_size bytes, including the trailing `0x00` delimiter.
///
/// Use this to size output buffers passed to `Encode()` or `EncodeInPlace()`.
constexpr size_t MaxEncodedSize(size_t unencoded_size) {
  return (unencoded_size == 0
              ? 1
              : unencoded_size +
                    ((unencoded_size + kMaxRunLength - 1) / kMaxRunLength)) +
         1;
}

/// Returns the maximum possible decoded payload size in bytes for a
/// `0x00`-terminated COBS frame of @p encoded_size bytes.
///
/// Use this to size output buffers passed to `Decode()` or `Decoder`.
constexpr size_t MaxDecodedSize(size_t encoded_size) {
  return encoded_size >= 2 ? encoded_size - 2 : 0;
}

/// Encodes @p input into @p output as a `0x00`-terminated COBS frame.
///
/// @pre @p input and @p output must not overlap (use `EncodeInPlace()` or
///   `EncodeBuffer` for in-place encoding).
///
/// @returns @Result{the encoded frame subspan of `output` (including trailing
/// `0x00`)}
/// * @RESOURCE_EXHAUSTED: `output` is too small to hold the encoded frame.
///   Sizing `output` to `MaxEncodedSize(input.size())` guarantees success.
Result<ByteSpan> Encode(ConstByteSpan input, ByteSpan output);

/// Decodes a single `0x00`-terminated COBS frame @p encoded into @p output.
///
/// @pre @p encoded and @p output must not overlap.
///
/// @returns @Result{the decoded payload subspan of `output`}
/// * @DATA_LOSS: `encoded` is not a valid `0x00`-terminated COBS frame.
/// * @RESOURCE_EXHAUSTED: `output` is too small to hold the decoded payload.
///   Sizing `output` to `MaxDecodedSize(encoded.size())` guarantees success.
Result<ByteSpan> Decode(ConstByteSpan encoded, ByteSpan output);

/// Encodes a payload in-place inside @p buffer, appending a trailing `0x00`
/// frame delimiter.
///
/// Prefer `EncodeBuffer<kMaxPayloadSize>` when allocating a fixed-capacity
/// buffer on the stack or inside a class. Use `EncodeInPlace` directly when
/// operating on an externally provided `ByteSpan`.
///
/// @pre `buffer.size() >= MaxEncodedSize(payload_size)`.
/// @pre The unencoded payload must be written at
///   `buffer.subspan(1, payload_size)` before calling `EncodeInPlace`
///   (`buffer[0]` must be left reserved in front of the payload).
///
/// @returns @Result{the encoded frame subspan at the start of `buffer`
/// (including trailing `0x00`)}
/// * @RESOURCE_EXHAUSTED: `buffer.size() < MaxEncodedSize(payload_size)`.
Result<ByteSpan> EncodeInPlace(ByteSpan buffer, size_t payload_size);

/// Fixed-capacity container that builds an unencoded payload and COBS-encodes
/// it in-place without a second buffer.
///
/// Example:
/// @code
///   pw::cobs::EncodeBuffer<kMaxSize> buffer;
///   buffer.append(header);
///   buffer.append(payload);
///   pw::ConstByteSpan encoded = buffer.Encode();
/// @endcode
template <size_t kMaxPayloadSize>
class EncodeBuffer {
 public:
  using value_type = std::byte;
  using size_type = size_t;
  using difference_type = ptrdiff_t;
  using reference = std::byte&;
  using const_reference = const std::byte&;
  using pointer = std::byte*;
  using const_pointer = const std::byte*;
  using iterator = containers::PtrIterator<EncodeBuffer>;
  using const_iterator = containers::ConstPtrIterator<EncodeBuffer>;

  constexpr EncodeBuffer() = default;

  explicit EncodeBuffer(ConstByteSpan initial) { append(initial); }

  /// Pointer to the unencoded payload region (`[data(), data() + size())`).
  ///
  /// @warning Invalidated by `Encode()`, which overwrites the internal buffer
  ///   in-place. Call `clear()` or `resize()` before writing a new payload.
  constexpr std::byte* data() { return buffer_.data() + 1; }
  constexpr const std::byte* data() const { return buffer_.data() + 1; }

  /// Current unencoded payload length in bytes.
  constexpr size_t size() const { return size_; }

  /// Maximum unencoded payload capacity in bytes.
  static constexpr size_t max_size() { return kMaxPayloadSize; }
  static constexpr size_t capacity() { return kMaxPayloadSize; }

  constexpr bool empty() const { return size_ == 0; }

  constexpr iterator begin() { return iterator(data()); }
  constexpr const_iterator begin() const { return const_iterator(data()); }
  constexpr const_iterator cbegin() const { return const_iterator(data()); }
  constexpr iterator end() { return iterator(data() + size_); }
  constexpr const_iterator end() const {
    return const_iterator(data() + size_);
  }
  constexpr const_iterator cend() const {
    return const_iterator(data() + size_);
  }

  constexpr std::byte& operator[](size_t index) { return data()[index]; }
  constexpr const std::byte& operator[](size_t index) const {
    return data()[index];
  }

  /// Resets the unencoded payload length to 0 so a new payload can be built.
  constexpr void clear() { size_ = 0; }

  /// Sets the unencoded payload length to @p new_size bytes.
  ///
  /// @pre `new_size <= max_size()`.
  void resize(size_t new_size) {
    PW_ASSERT(new_size <= kMaxPayloadSize);
    size_ = new_size;
  }

  /// Appends a single byte to the unencoded payload.
  ///
  /// @pre `size() < max_size()`.
  void push_back(std::byte b) {
    PW_ASSERT(size_ < kMaxPayloadSize);
    data()[size_++] = b;
  }

  /// Appends @p bytes to the unencoded payload.
  ///
  /// @pre `size() + bytes.size() <= max_size()`.
  void append(ConstByteSpan bytes) {
    PW_ASSERT(bytes.size() <= kMaxPayloadSize - size_);
    if (!bytes.empty()) {
      std::memcpy(data() + size_, bytes.data(), bytes.size());
      size_ += bytes.size();
    }
  }

  /// Encodes the current `size()` payload bytes in-place and returns the
  /// resulting `0x00`-terminated COBS frame.
  ///
  /// @post The returned span is valid until the next mutation or `Encode()`
  ///   call on this `EncodeBuffer`. Because `Encode()` overwrites the buffer
  ///   in-place, call `clear()` or `resize()` before building a new payload.
  ConstByteSpan Encode() { return *EncodeInPlace(buffer_, size_); }

  /// Resizes the payload to @p payload_size bytes (for payloads written
  /// directly via `data()`) and encodes in-place.
  ///
  /// @pre `payload_size <= max_size()`.
  /// @post Same as `Encode()`.
  ConstByteSpan Encode(size_t payload_size) {
    resize(payload_size);
    return Encode();
  }

 private:
  std::array<std::byte, MaxEncodedSize(kMaxPayloadSize)> buffer_{};
  size_t size_ = 0;
};

/// Incrementally COBS-encodes a frame of arbitrary length through a small
/// output buffer (`StreamerBuffer<>` or a caller-provided `ByteSpan`).
///
/// Feed payload chunks into `Write()`. Whenever `Write()` returns a non-empty
/// unconsumed remainder, transmit `Flush()` before calling `Write()` again.
/// Once all payload chunks have been written, transmit `Finish()` to complete
/// the frame:
///
/// @code
///   pw::cobs::StreamerBuffer<> streamer;
///   for (pw::ConstByteSpan seg : segments) {
///     while (!seg.empty()) {
///       seg = streamer.Write(seg);
///       if (!seg.empty()) {
///         WriteToUart(streamer.Flush());
///       }
///     }
///   }
///   WriteToUart(streamer.Finish());
/// @endcode
class Streamer {
 public:
  /// Minimum buffer size (257 bytes) required to stream arbitrarily large
  /// frames across multiple `Flush()` calls.
  static constexpr size_t kMinBufferSize = MaxEncodedSize(kMaxRunLength) + 1;

  /// 2-byte sequence (`0xFF, 0x00`) that aborts an in-progress frame on the
  /// wire so the receiver discards it with `DATA_LOSS`.
  static constexpr std::array<std::byte, 2> kAbortSequence{std::byte{0xFF},
                                                           std::byte{0x00}};

  /// Constructs a `Streamer` that writes encoded chunks into @p buffer.
  ///
  /// @pre `buffer.size() >= kMinBufferSize` to stream arbitrary payloads (or
  ///   use `StreamerBuffer<>`, which provides the buffer automatically).
  /// @param emit_leading_delimiter If `true`, prepends a leading `0x00`
  ///   delimiter byte to the first emitted chunk to flush any prior line noise
  ///   on the receiver.
  explicit constexpr Streamer(ByteSpan buffer,
                              bool emit_leading_delimiter = false)
      : buffer_(buffer) {
    Reset(emit_leading_delimiter);
  }

  /// Discards any buffered state and prepares to encode a new frame.
  ///
  /// @param emit_leading_delimiter If `true`, prepends a leading `0x00`
  ///   delimiter byte to the first chunk of the new frame.
  constexpr void Reset(bool emit_leading_delimiter = false) {
    if (emit_leading_delimiter && !buffer_.empty()) {
      buffer_[0] = std::byte{0x00};
      lead_offset_ = 1;
    } else {
      lead_offset_ = 0;
    }
    code_pos_ = lead_offset_;
    write_pos_ = lead_offset_ + 1;
    code_ = 1;
    flushed_any_ = false;
  }

  /// Encodes as much of @p data as fits into the output buffer and returns the
  /// unconsumed remainder of @p data.
  ///
  /// @post If the returned span is empty, all of @p data was consumed and you
  ///   may call `Write()` with the next payload piece or `Finish()` to end the
  ///   frame.
  /// @post If the returned span is non-empty, the output buffer is ready to
  ///   drain: call `Flush()`, transmit that chunk, and pass the returned
  ///   remainder back to `Write()`.
  ConstByteSpan Write(ConstByteSpan data);

  /// Drains completed output from the buffer and returns the wire chunk to
  /// transmit.
  ///
  /// Returns an empty span if no complete block is ready to flush yet.
  ///
  /// @post The returned `ByteSpan` points into `buffer` and remains valid
  ///   until the next `Write()`, `Flush()`, `Finish()`, or `Reset()` call.
  ByteSpan Flush();

  /// Finalizes the frame (appending the trailing `0x00` delimiter), resets the
  /// streamer for the next frame (`emit_leading_delimiter = false`), and
  /// returns the final wire chunk to transmit.
  ///
  /// @post The returned `ByteSpan` points into `buffer` and remains valid
  ///   until the next `Write()` or `Reset()` call.
  ByteSpan Finish();

  /// Returns the 2-byte abort sequence (`0xFF, 0x00`) to transmit if a prior
  /// `Flush()` has already sent bytes of the current frame on the wire, or an
  /// empty span if nothing has been flushed yet.
  ///
  /// Call `Reset()` after aborting before starting a new frame.
  constexpr ConstByteSpan AbortSequence() const {
    return flushed_any_ ? ConstByteSpan(kAbortSequence) : ConstByteSpan();
  }

 private:
  friend Result<ByteSpan> Encode(ConstByteSpan input, ByteSpan output);

  ByteSpan buffer_;
  size_t lead_offset_ = 0;
  size_t code_pos_ = 0;
  size_t write_pos_ = 1;
  uint8_t code_ = 1;
  bool flushed_any_ = false;
};

/// A `Streamer` with an inline output buffer of @p kSizeBytes bytes (defaulting
/// to `Streamer::kMinBufferSize` = 257 bytes).
template <size_t kSizeBytes = Streamer::kMinBufferSize>
class StreamerBuffer
    : private containers::StorageBaseFor<std::byte, kSizeBytes>,
      public Streamer {
 public:
  static_assert(kSizeBytes >= Streamer::kMinBufferSize,
                "StreamerBuffer must be at least Streamer::kMinBufferSize");

  explicit constexpr StreamerBuffer(bool emit_leading_delimiter = false)
      : containers::StorageBaseFor<std::byte, kSizeBytes>{},
        Streamer(ByteSpan(this->storage().data(), this->storage().size()),
                 emit_leading_delimiter) {}

  StreamerBuffer(const StreamerBuffer&) = delete;
  StreamerBuffer& operator=(const StreamerBuffer&) = delete;
  StreamerBuffer(StreamerBuffer&&) = delete;
  StreamerBuffer& operator=(StreamerBuffer&&) = delete;

  static constexpr size_t max_size() { return kSizeBytes; }
};

/// Incremental streaming decoder for `0x00`-delimited COBS frames.
///
/// Feed incoming wire bytes either one byte at a time (`Process(std::byte)`) or
/// in chunks (`Process(ConstByteSpan, callback)`). Back-to-back `0x00`
/// delimiters are ignored as idle line bytes.
class Decoder {
 public:
  /// Constructs a `Decoder` that writes decoded payloads into @p decode_buffer.
  ///
  /// Size @p decode_buffer to the maximum expected decoded payload length (or
  /// use `DecoderBuffer<kMaxPayloadSize>`).
  explicit constexpr Decoder(ByteSpan decode_buffer)
      : decode_buffer_(decode_buffer) {}

  /// Discards any partially received frame and resets the decoder to idle.
  void Clear();

  /// Returns `true` if a frame is currently being assembled (at least one
  /// non-zero byte has been received since the last `0x00` delimiter or
  /// `Clear()`).
  constexpr bool in_progress() const { return code_ != 0; }

  /// Returns the number of guaranteed non-zero data bytes remaining in the
  /// active COBS block. When `in_progress()` is true, at least
  /// `remaining_in_run() + 1` wire bytes must arrive before the current frame
  /// can terminate at a `0x00` delimiter.
  constexpr size_t remaining_in_run() const { return remaining_in_run_; }

  /// Processes up to the first `0x00` frame delimiter in @p input, setting
  /// @p bytes_consumed to the number of bytes consumed from @p input (including
  /// the terminating `0x00` delimiter when a frame completes).
  ///
  /// Unlike `Process(ConstByteSpan, Callback)`, which consumes all of @p input
  /// and invokes a callback for every frame, `ProcessChunk` stops at the first
  /// frame boundary so pull/poll-based callers (such as `pw_async2` receive
  /// tasks draining a ring or DMA buffer) can return one frame per poll.
  Result<ByteSpan> ProcessChunk(ConstByteSpan input, size_t& bytes_consumed);

  /// Processes a single incoming wire byte @p b.
  ///
  /// The decoder automatically resets at every `0x00` delimiter (whether the
  /// frame succeeded or failed), so the next byte begins a new frame without
  /// calling `Clear()`.
  ///
  /// @returns
  /// * @UNAVAILABLE: Frame assembly is still in progress (or an idle `0x00`
  ///   delimiter was ignored).
  /// * @OK: A valid frame completed on a `0x00` delimiter. The returned
  ///   `ByteSpan` points into `decode_buffer` and remains valid until the next
  ///   `Process()` or `Clear()` call.
  /// * @DATA_LOSS: A malformed or truncated frame ended at `0x00`.
  /// * @RESOURCE_EXHAUSTED: The decoded payload exceeded `decode_buffer.size()`
  ///   (reported when the frame's closing `0x00` delimiter arrives).
  Result<ByteSpan> Process(std::byte b);

  /// Processes a chunk of incoming wire bytes and invokes @p callback with a
  /// `Result<ByteSpan>` (`Ok`, `DataLoss`, or `ResourceExhausted`) for each
  /// frame that terminates within @p data.
  ///
  /// @post Each `ByteSpan` passed to @p callback points into `decode_buffer`
  ///   and is only valid for the duration of that @p callback invocation.
  template <typename Callback>
  void Process(ConstByteSpan data, Callback&& callback) {
    while (!data.empty()) {
      size_t bytes_consumed = 0;
      Result<ByteSpan> result = ProcessChunk(data, bytes_consumed);
      data = data.subspan(bytes_consumed);
      if (!result.status().IsUnavailable()) {
        callback(result);
      }
    }
  }

 private:
  friend Result<ByteSpan> Decode(ConstByteSpan encoded, ByteSpan output);

  ByteSpan decode_buffer_;
  size_t write_pos_ = 0;
  size_t remaining_in_run_ = 0;
  uint8_t code_ = 0;
  bool overflowed_ = false;
};

/// A `Decoder` with an inline decode buffer of @p kSizeBytes bytes.
template <size_t kSizeBytes>
class DecoderBuffer : private containers::StorageBaseFor<std::byte, kSizeBytes>,
                      public Decoder {
 public:
  constexpr DecoderBuffer()
      : containers::StorageBaseFor<std::byte, kSizeBytes>{},
        Decoder(ByteSpan(this->storage().data(), this->storage().size())) {}

  DecoderBuffer(const DecoderBuffer&) = delete;
  DecoderBuffer& operator=(const DecoderBuffer&) = delete;
  DecoderBuffer(DecoderBuffer&&) = delete;
  DecoderBuffer& operator=(DecoderBuffer&&) = delete;

  /// Returns the capacity of the decode buffer in bytes.
  static constexpr size_t max_size() { return kSizeBytes; }
};

}  // namespace pw::cobs
