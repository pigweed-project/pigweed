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
#include "pw_trace_tokenized/trace_buffer.h"

#include "pw_span/span.h"
#include "pw_trace_tokenized/trace_callback.h"

namespace pw::trace {
namespace internal {

class TraceBufferImpl {
 public:
  TraceBufferImpl(Callbacks& callbacks)
      : callbacks_(callbacks), trace_buffer_(queue_) {
    callbacks_
        .RegisterSink(
            TraceSinkStartBlock, TraceSinkAddBytes, TraceSinkEndBlock, this)
        .IgnoreError();  // TODO: b/242598609 - Handle Status properly
  }

  static void TraceSinkStartBlock(void* user_data, size_t size) {
    auto* buffer = reinterpret_cast<TraceBufferImpl*>(user_data);
    if (size > PW_TRACE_BUFFER_MAX_BLOCK_SIZE_BYTES) {
      buffer->block_size_ = 0;  // Skip this block
      return;
    }
    buffer->block_size_ = static_cast<uint16_t>(size);
    buffer->block_idx_ = 0;
  }

  static void TraceSinkAddBytes(void* user_data,
                                const void* bytes,
                                size_t size) {
    auto* buffer = reinterpret_cast<TraceBufferImpl*>(user_data);
    if (buffer->block_size_ == 0 ||
        buffer->block_idx_ + size > buffer->block_size_) {
      return;  // Block is too large, skipping.
    }
    memcpy(&buffer->current_block_[buffer->block_idx_], bytes, size);
    buffer->block_idx_ += static_cast<uint16_t>(size);
  }

  static void TraceSinkEndBlock(void* user_data) {
    auto* buffer = reinterpret_cast<TraceBufferImpl*>(user_data);
    if (buffer->block_idx_ != buffer->block_size_) {
      return;  // Block is too large, skipping.
    }
    buffer->queue_.push_overwrite(
        ConstByteSpan(buffer->current_block_, buffer->block_size_));
  }

  constexpr TraceBuffer& GetBuffer() { return trace_buffer_; }

 private:
  friend ConstByteSpan pw::trace::DeringAndViewRawBuffer();

  Callbacks& callbacks_;
  uint16_t block_size_ = 0;
  uint16_t block_idx_ = 0;
  std::byte current_block_[PW_TRACE_BUFFER_MAX_BLOCK_SIZE_BYTES];
  InlineVarLenEntryQueue<PW_TRACE_BUFFER_SIZE_BYTES> queue_;
  TraceBuffer trace_buffer_;
};

#if PW_TRACE_BUFFER_SIZE_BYTES > 0
TraceBufferImpl trace_buffer_instance(GetCallbacks());
#endif  // PW_TRACE_BUFFER_SIZE_BYTES > 0

}  // namespace internal

void ClearBuffer() { GetBuffer()->Clear(); }

TraceBuffer* GetBuffer() {
  return &internal::trace_buffer_instance.GetBuffer();
}

ConstByteSpan DeringAndViewRawBuffer() { return GetBuffer()->queue().dering(); }

}  // namespace pw::trace
