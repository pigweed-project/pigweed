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

#include <cstddef>

#include "pw_bytes/span.h"
#include "pw_containers/algorithm.h"
#include "pw_containers/inline_var_len_entry_queue.h"
#include "pw_status/status.h"
#include "pw_trace_tokenized/config.h"

namespace pw {

/// @module{pw_trace_tokenzed}

namespace trace {
namespace internal {

// Forward declaration.
class TraceBufferImpl;

}  // namespace internal

/// Adapter type that allows using an `InlineVarLenEntryQueue` with code that
/// expects a `PrefixedEntryRingBufferMulti`. This type only partially
/// implements the latter's interface. In particular, it implements those
/// methods used by callers of `pw::trace::GetBuffer`.
class TraceBuffer {
 public:
  InlineVarLenEntryQueue<>& queue() { return queue_; }

  /// Read the oldest stored data chunk of data from the ring buffer to
  /// the provided destination span. The number of bytes read is written
  /// to bytes_read
  ///
  /// @returns
  /// * @OK: Data successfully read from the ring buffer.
  /// * @OUT_OF_RANGE: No entries in ring buffer to pop.
  /// * @RESOURCE_EXHAUSTED - Destination data span was smaller number of bytes
  ///   than the data size of the data chunk being read. Available destination
  ///   bytes were filled, remaining bytes of the data chunk were ignored.
  constexpr Status PeekFront(ByteSpan data, size_t* bytes_read_out) const;

  /// Pop and discard the oldest stored data chunk of data from the ring
  /// buffer.
  ///
  /// @returns
  /// * @OK: Data successfully read from the ring buffer.
  /// * @OUT_OF_RANGE: No entries in ring buffer to pop.
  constexpr Status PopFront();

  /// Get the number of variable-length entries currently in the ring buffer.
  constexpr size_t EntryCount() const { return queue_.size(); }

  /// Determines if the ring buffer has corrupted entries.
  ///
  /// This method is kept strictly for compatibility reasons. As long as access
  /// to this object is properly synchronized, it should not be possible to
  /// corrupt the queue through its public API.
  constexpr Status CheckForCorruption() { return OkStatus(); }

  /// Removes all data from the ring buffer.
  constexpr void Clear() { queue_.clear(); }

  /// Get the size in bytes of all the current entries in the ring buffer.
  constexpr size_t TotalUsedBytes() const {
    return queue_.encoded_size_bytes();
  }

 private:
  friend class ::pw::trace::internal::TraceBufferImpl;

  constexpr explicit TraceBuffer(InlineVarLenEntryQueue<>& queue)
      : queue_(queue) {}

  InlineVarLenEntryQueue<>& queue_;
};

/// Resets the trace buffer. All data currently stored in the buffer is lost.
void ClearBuffer();

/// Gets the ring buffer which contains the data.
TraceBuffer* GetBuffer();

/// Makes all entries contiguous (i.e. "dering") and then provides a raw view
/// of the data. This allows for bulk access to the trace events buffer. Since
/// this also derings the underlying ring_buffer, ensure that tracing is
/// disabled when calling this function.
ConstByteSpan DeringAndViewRawBuffer();

////////////////////////////////////////////////////////////////////////////////
// Constexpr method implementations

constexpr Status TraceBuffer::PeekFront(ByteSpan data,
                                        size_t* bytes_read_out) const {
  if (queue_.empty()) {
    return Status::OutOfRange();
  }
  auto entry = queue_.front();
  size_t len = std::min(data.size(), entry.size());
  pw::copy(
      entry.begin(),
      entry.begin() +
          static_cast<
              InlineVarLenEntryQueue<>::value_type::iterator::difference_type>(
              len),
      data.begin());
  *bytes_read_out = len;
  return data.size() < entry.size() ? Status::ResourceExhausted() : OkStatus();
}

constexpr Status TraceBuffer::PopFront() {
  if (queue_.empty()) {
    return Status::OutOfRange();
  }
  queue_.pop();
  return OkStatus();
}

}  // namespace trace

/// @}

}  // namespace pw
