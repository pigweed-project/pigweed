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

#include "pw_grpc/connection.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

#include "pw_allocator/best_fit.h"
#include "pw_allocator/forwarding_allocator.h"
#include "pw_allocator/synchronized_allocator.h"
#include "pw_async/dispatcher.h"
#include "pw_async_basic/dispatcher.h"
#include "pw_bytes/array.h"
#include "pw_bytes/byte_builder.h"
#include "pw_bytes/endian.h"
#include "pw_bytes/span.h"
#include "pw_containers/inline_deque.h"
#include "pw_containers/vector.h"
#include "pw_function/function.h"
#include "pw_grpc/grpc_channel_output.h"
#include "pw_grpc/send_queue.h"
#include "pw_rpc/internal/packet.h"
#include "pw_status/status.h"
#include "pw_stream/stream.h"
#include "pw_string/string.h"
#include "pw_sync/lock_annotations.h"
#include "pw_sync/mutex.h"
#include "pw_sync/thread_notification.h"
#include "pw_thread/test_thread_context.h"
#include "pw_thread/thread.h"
#include "pw_thread/thread_core.h"
#include "pw_unit_test/framework.h"

namespace pw::grpc {
namespace {

constexpr size_t kFrameHeaderSize = 9;
constexpr size_t kLengthPrefixedMessageHdrSize = 5;
constexpr uint32_t kDefaultInitialWindowSize = 65535;

constexpr uint8_t kFrameTypeData = 0x00;
constexpr uint8_t kFrameTypeHeaders = 0x01;
constexpr uint8_t kFrameTypePriority = 0x02;
constexpr uint8_t kFrameTypeRstStream = 0x03;
constexpr uint8_t kFrameTypeSettings = 0x04;
constexpr uint8_t kFrameTypePing = 0x06;
constexpr uint8_t kFrameTypeGoAway = 0x07;
constexpr uint8_t kFrameTypeWindowUpdate = 0x08;

constexpr uint8_t kFlagAck = 0x01;
constexpr uint8_t kFlagEndStream = 0x01;
constexpr uint8_t kFlagEndHeaders = 0x04;

constexpr uint16_t kSettingsInitialWindowSize = 0x0004;
constexpr uint32_t kMaxFlowControlWindow = 0x7fffffffU;
constexpr uint32_t kMax24BitFrameLength = 0xffffffU;
constexpr uint32_t kHttp2ErrorProtocolError = 0x01;
constexpr uint32_t kHttp2ErrorInternalError = 0x02;
constexpr uint32_t kHttp2ErrorFlowControlError = 0x03;
constexpr uint32_t kHttp2ErrorStreamClosed = 0x05;
constexpr uint32_t kHttp2ErrorFrameSizeError = 0x06;
constexpr uint32_t kHttp2ErrorCancel = 0x08;
constexpr uint32_t kHttp2ErrorEnhanceYourCalm = 0x0b;

constexpr size_t kReaderBufferSize = 512;
constexpr size_t kFrameScratchSize = 128;
constexpr size_t kMaxRecordedFrames = 32;
constexpr size_t kMaxStreamFrames = 8;
constexpr size_t kMaxRecordedMessageSize = 64;
constexpr size_t kMaxTrackedStreams = 8;
constexpr size_t kSendAllocatorPoolSize = 32 * 1024;
constexpr size_t kAssemblyAllocatorPoolSize = 256;
constexpr size_t kReadAllocatorPoolSize = 4 * 1024;

constexpr std::string_view kClientPrefaceLiteral(
    "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n");

void AppendFrameHeader(ByteBuilder& out,
                       uint32_t payload_length,
                       uint8_t type,
                       uint8_t flags,
                       StreamId stream_id) {
  out.PutUint8(static_cast<uint8_t>((payload_length >> 16) & 0xff));
  out.PutUint8(static_cast<uint8_t>((payload_length >> 8) & 0xff));
  out.PutUint8(static_cast<uint8_t>(payload_length & 0xff));
  out.PutUint8(type);
  out.PutUint8(flags);
  out.PutUint32(stream_id & 0x7fffffffU, endian::big);
}

void AppendLengthPrefixedChunk(ByteBuilder& out,
                               uint32_t declared_message_length,
                               ConstByteSpan chunk) {
  constexpr uint8_t kUncompressedFlag = 0;
  out.PutUint8(kUncompressedFlag);
  out.PutUint32(declared_message_length, endian::big);
  out.append(chunk);
}

// Blocking in-memory reader that parks the reader thread on a
// ThreadNotification when empty and notifies the test thread once all pushed
// bytes have been consumed and processed up to the next frame boundary.
class BlockingInMemoryReader : public stream::NonSeekableReader {
 public:
  void Push(ConstByteSpan data) PW_LOCKS_EXCLUDED(mutex_) {
    size_t offset = 0;
    while (offset < data.size()) {
      bool pushed = false;
      bool wait_for_space = false;
      {
        std::lock_guard<sync::Mutex> lock(mutex_);
        if (reader_exited_) {
          return;
        }
        const size_t space = buffer_.capacity() - buffer_.size();
        const size_t to_copy = std::min(space, data.size() - offset);
        for (size_t i = 0; i < to_copy; ++i) {
          buffer_.push_back(data[offset + i]);
        }
        offset += to_copy;
        pushed = to_copy > 0;
        if (pushed) {
          static_cast<void>(idle_notification_.try_acquire());
        }
        if (offset < data.size() && buffer_.full()) {
          static_cast<void>(space_available_.try_acquire());
          wait_for_space = true;
        }
      }
      if (pushed) {
        readable_.release();
      }
      if (wait_for_space) {
        space_available_.acquire();
      }
    }
  }

  void PushClientPreface() PW_LOCKS_EXCLUDED(mutex_) {
    ByteBuffer<kFrameScratchSize> bytes;
    bytes.append(as_bytes(span(kClientPrefaceLiteral)));
    AppendFrameHeader(bytes,
                      /*payload_length=*/0,
                      kFrameTypeSettings,
                      /*flags=*/0,
                      /*stream_id=*/0);
    ASSERT_EQ(bytes.status(), OkStatus());
    Push(bytes);
  }

  void PushClientPrefaceWithWindow(uint32_t initial_window)
      PW_LOCKS_EXCLUDED(mutex_) {
    ByteBuffer<kFrameScratchSize> bytes;
    bytes.append(as_bytes(span(kClientPrefaceLiteral)));
    AppendFrameHeader(bytes,
                      /*payload_length=*/6,
                      kFrameTypeSettings,
                      /*flags=*/0,
                      /*stream_id=*/0);
    bytes.PutUint16(kSettingsInitialWindowSize, endian::big);
    bytes.PutUint32(initial_window, endian::big);
    ASSERT_EQ(bytes.status(), OkStatus());
    Push(bytes);
  }

  void PushSettingsInitialWindow(uint32_t initial_window)
      PW_LOCKS_EXCLUDED(mutex_) {
    ByteBuffer<kFrameScratchSize> bytes;
    AppendFrameHeader(bytes,
                      /*payload_length=*/6,
                      kFrameTypeSettings,
                      /*flags=*/0,
                      /*stream_id=*/0);
    bytes.PutUint16(kSettingsInitialWindowSize, endian::big);
    bytes.PutUint32(initial_window, endian::big);
    ASSERT_EQ(bytes.status(), OkStatus());
    Push(bytes);
  }

  void PushRequestHeaders(StreamId stream_id, std::string_view path = "/")
      PW_LOCKS_EXCLUDED(mutex_) {
    ByteBuffer<kFrameScratchSize> bytes;
    if (path == "/") {
      // RFC 7541 static table index 4 (:path = "/").
      AppendFrameHeader(bytes,
                        /*payload_length=*/1,
                        kFrameTypeHeaders,
                        kFlagEndHeaders,
                        stream_id);
      bytes.PutUint8(0x84);
    } else {
      // Literal header field without indexing for static index 4 (:path).
      const uint32_t hpack_len = 2U + static_cast<uint32_t>(path.size());
      AppendFrameHeader(
          bytes, hpack_len, kFrameTypeHeaders, kFlagEndHeaders, stream_id);
      bytes.PutUint8(0x04);
      bytes.PutUint8(static_cast<uint8_t>(path.size()));
      bytes.append(as_bytes(span(path)));
    }
    ASSERT_EQ(bytes.status(), OkStatus());
    Push(bytes);
  }

  void PushRequestData(StreamId stream_id,
                       ConstByteSpan message,
                       bool end_stream) PW_LOCKS_EXCLUDED(mutex_) {
    PushLengthPrefixAndChunk(
        stream_id, static_cast<uint32_t>(message.size()), message, end_stream);
  }

  void PushRawDataFrame(StreamId stream_id,
                        ConstByteSpan raw_payload,
                        bool end_stream) PW_LOCKS_EXCLUDED(mutex_) {
    ByteBuffer<kFrameHeaderSize> header;
    AppendFrameHeader(header,
                      static_cast<uint32_t>(raw_payload.size()),
                      kFrameTypeData,
                      end_stream ? kFlagEndStream : 0,
                      stream_id);
    ASSERT_EQ(header.status(), OkStatus());
    Push(header);
    Push(raw_payload);
  }

  void PushFrameHeaderWithPartialPayload(uint32_t declared_payload_length,
                                         uint8_t type,
                                         uint8_t flags,
                                         StreamId stream_id,
                                         ConstByteSpan partial_payload)
      PW_LOCKS_EXCLUDED(mutex_) {
    ByteBuffer<kFrameHeaderSize> header;
    AppendFrameHeader(header, declared_payload_length, type, flags, stream_id);
    ASSERT_EQ(header.status(), OkStatus());
    Push(header);
    Push(partial_payload);
  }

  void PushDataFrameHeader(StreamId stream_id,
                           uint32_t payload_length,
                           bool end_stream) PW_LOCKS_EXCLUDED(mutex_) {
    PushFrameHeaderWithPartialPayload(payload_length,
                                      kFrameTypeData,
                                      end_stream ? kFlagEndStream : 0,
                                      stream_id,
                                      ConstByteSpan());
  }

  void PushLengthPrefixAndChunk(StreamId stream_id,
                                uint32_t declared_message_length,
                                ConstByteSpan initial_chunk,
                                bool end_stream) PW_LOCKS_EXCLUDED(mutex_) {
    ByteBuffer<kFrameHeaderSize + kLengthPrefixedMessageHdrSize> prefix;
    const uint32_t framed_len = static_cast<uint32_t>(
        kLengthPrefixedMessageHdrSize + initial_chunk.size());
    AppendFrameHeader(prefix,
                      framed_len,
                      kFrameTypeData,
                      end_stream ? kFlagEndStream : 0,
                      stream_id);
    AppendLengthPrefixedChunk(prefix, declared_message_length, ConstByteSpan());
    ASSERT_EQ(prefix.status(), OkStatus());
    Push(prefix);
    Push(initial_chunk);
  }

  void PushPing(ConstByteSpan opaque_eight_bytes) PW_LOCKS_EXCLUDED(mutex_) {
    ASSERT_EQ(opaque_eight_bytes.size(), 8U);
    ByteBuffer<kFrameScratchSize> bytes;
    AppendFrameHeader(bytes,
                      /*payload_length=*/8,
                      kFrameTypePing,
                      /*flags=*/0,
                      /*stream_id=*/0);
    bytes.append(opaque_eight_bytes);
    ASSERT_EQ(bytes.status(), OkStatus());
    Push(bytes);
  }

  void PushWindowUpdate(StreamId stream_id, uint32_t increment)
      PW_LOCKS_EXCLUDED(mutex_) {
    ByteBuffer<kFrameScratchSize> bytes;
    AppendFrameHeader(bytes,
                      /*payload_length=*/4,
                      kFrameTypeWindowUpdate,
                      /*flags=*/0,
                      stream_id);
    bytes.PutUint32(increment & 0x7fffffffU, endian::big);
    ASSERT_EQ(bytes.status(), OkStatus());
    Push(bytes);
  }

  void PushRstStream(StreamId stream_id, uint32_t error_code)
      PW_LOCKS_EXCLUDED(mutex_) {
    ByteBuffer<kFrameScratchSize> bytes;
    AppendFrameHeader(bytes,
                      /*payload_length=*/4,
                      kFrameTypeRstStream,
                      /*flags=*/0,
                      stream_id);
    bytes.PutUint32(error_code, endian::big);
    ASSERT_EQ(bytes.status(), OkStatus());
    Push(bytes);
  }

  // Blocks until the reader thread has consumed all pushed bytes, completed the
  // in-progress ProcessConnectionPreface()/ProcessFrame() call, and parked
  // waiting for the next frame (or exited).
  void WaitUntilIdle() PW_LOCKS_EXCLUDED(mutex_) {
    while (true) {
      {
        std::lock_guard<sync::Mutex> lock(mutex_);
        if (reader_exited_ || (buffer_.empty() && reader_waiting_)) {
          return;
        }
      }
      idle_notification_.acquire();
    }
  }

  void Close() PW_LOCKS_EXCLUDED(mutex_) {
    {
      std::lock_guard<sync::Mutex> lock(mutex_);
      closed_ = true;
    }
    readable_.release();
  }

  void MarkReaderExited() PW_LOCKS_EXCLUDED(mutex_) {
    {
      std::lock_guard<sync::Mutex> lock(mutex_);
      reader_exited_ = true;
    }
    space_available_.release();
    idle_notification_.release();
  }

 private:
  StatusWithSize DoRead(ByteSpan dest) override PW_LOCKS_EXCLUDED(mutex_) {
    while (true) {
      bool freed_from_full = false;
      size_t copied = 0;
      {
        std::lock_guard<sync::Mutex> lock(mutex_);
        if (!buffer_.empty()) {
          const bool was_full = buffer_.full();
          copied = std::min<size_t>(dest.size(), buffer_.size());
          for (size_t i = 0; i < copied; ++i) {
            dest[i] = buffer_.front();
            buffer_.pop_front();
          }
          freed_from_full = was_full && copied > 0;
        } else if (closed_) {
          return StatusWithSize::OutOfRange();
        } else {
          reader_waiting_ = true;
        }
      }
      if (copied > 0) {
        if (freed_from_full) {
          space_available_.release();
        }
        return StatusWithSize(copied);
      }
      idle_notification_.release();
      readable_.acquire();
      {
        std::lock_guard<sync::Mutex> lock(mutex_);
        reader_waiting_ = false;
      }
    }
  }

  sync::Mutex mutex_;
  sync::ThreadNotification readable_;
  sync::ThreadNotification space_available_;
  sync::ThreadNotification idle_notification_;
  InlineDeque<std::byte, kReaderBufferSize> buffer_ PW_GUARDED_BY(mutex_);
  bool closed_ PW_GUARDED_BY(mutex_) = false;
  bool reader_waiting_ PW_GUARDED_BY(mutex_) = false;
  bool reader_exited_ PW_GUARDED_BY(mutex_) = false;
};

struct RecordedFrame {
  uint8_t type = 0;
  uint8_t flags = 0;
  StreamId stream_id = 0;
  UniquePtr<std::byte[]> raw_frame;

  ConstByteSpan payload() const {
    if (raw_frame == nullptr || raw_frame.size() < kFrameHeaderSize) {
      return ConstByteSpan();
    }
    return ConstByteSpan(raw_frame.get() + kFrameHeaderSize,
                         raw_frame.size() - kFrameHeaderSize);
  }
};

class RecordingSendQueue : public SendQueue {
 public:
  bool QueueSend(UniquePtr<std::byte[]>&& buffer) override
      PW_LOCKS_EXCLUDED(mutex_) {
    std::lock_guard<sync::Mutex> lock(mutex_);
    if (reject_sends_ || buffer == nullptr ||
        buffer.size() < kFrameHeaderSize || frames_.full()) {
      return false;
    }
    const uint32_t length = (static_cast<uint32_t>(buffer[0]) << 16) |
                            (static_cast<uint32_t>(buffer[1]) << 8) |
                            static_cast<uint32_t>(buffer[2]);
    if (kFrameHeaderSize + length != buffer.size()) {
      return false;
    }

    const uint8_t type = static_cast<uint8_t>(buffer[3]);
    const uint8_t flags = static_cast<uint8_t>(buffer[4]);
    const StreamId stream_id =
        bytes::ReadInOrder<uint32_t>(endian::big, buffer.get() + 5) &
        0x7fffffffU;
    frames_.push_back(RecordedFrame{
        .type = type,
        .flags = flags,
        .stream_id = stream_id,
        .raw_frame = std::move(buffer),
    });
    return true;
  }

  void set_on_error(ErrorHandler&& /*error_handler*/) override {}
  void set_on_space_available(SpaceAvailableCallback&& callback) override
      PW_LOCKS_EXCLUDED(mutex_) {
    std::lock_guard<sync::Mutex> lock(mutex_);
    on_space_available_ = std::move(callback);
  }
  void Run() override {}
  void RequestStop() override {}

  void SetRejectSends(bool reject) PW_LOCKS_EXCLUDED(mutex_) {
    std::lock_guard<sync::Mutex> lock(mutex_);
    reject_sends_ = reject;
    if (!reject && on_space_available_ != nullptr) {
      on_space_available_();
    }
  }

  void TakeStreamFrames(StreamId stream_id, Vector<RecordedFrame>& out)
      PW_LOCKS_EXCLUDED(mutex_) {
    std::lock_guard<sync::Mutex> lock(mutex_);
    out.clear();
    const size_t count = frames_.size();
    for (size_t i = 0; i < count; ++i) {
      RecordedFrame frame = std::move(frames_.front());
      frames_.pop_front();
      if (frame.stream_id == stream_id) {
        ASSERT_FALSE(out.full());
        out.push_back(std::move(frame));
      } else {
        frames_.push_back(std::move(frame));
      }
    }
  }

  void ClearStreamFrames(StreamId stream_id) PW_LOCKS_EXCLUDED(mutex_) {
    std::lock_guard<sync::Mutex> lock(mutex_);
    const size_t count = frames_.size();
    for (size_t i = 0; i < count; ++i) {
      RecordedFrame frame = std::move(frames_.front());
      frames_.pop_front();
      if (frame.stream_id != stream_id) {
        frames_.push_back(std::move(frame));
      }
    }
  }

 private:
  mutable sync::Mutex mutex_;
  InlineDeque<RecordedFrame, kMaxRecordedFrames> frames_ PW_GUARDED_BY(mutex_);
  SpaceAvailableCallback on_space_available_ PW_GUARDED_BY(mutex_);
  bool reject_sends_ PW_GUARDED_BY(mutex_) = false;
};

class MessageGate {
 public:
  void EnterAndWait() {
    entered_.release();
    allow_return_.acquire();
  }

  void WaitUntilEntered() { entered_.acquire(); }
  void AllowReturn() { allow_return_.release(); }

 private:
  sync::ThreadNotification entered_;
  sync::ThreadNotification allow_return_;
};

class RecordingCallbacks : public Connection::RequestCallbacks {
 public:
  void OnNewConnection() override PW_LOCKS_EXCLUDED(mutex_) {
    std::lock_guard<sync::Mutex> lock(mutex_);
    ++new_connection_count_;
  }

  Status OnNew(StreamId id,
               InlineString<kMaxMethodNameSize> full_method_name) override
      PW_LOCKS_EXCLUDED(mutex_) {
    std::lock_guard<sync::Mutex> lock(mutex_);
    last_stream_id_ = id;
    last_method_ = full_method_name;
    return OkStatus();
  }

  Status OnMessage(StreamId id, ByteSpan message) override
      PW_LOCKS_EXCLUDED(mutex_) {
    MessageGate* gate = nullptr;
    Function<Status(StreamId, ByteSpan)> callback;
    Status status = OkStatus();
    {
      std::lock_guard<sync::Mutex> lock(mutex_);
      ++message_count_;
      last_message_stream_id_ = id;
      last_message_.clear();
      last_message_.append(message);
      status = last_message_.status();
      gate = message_gate_;
      message_gate_ = nullptr;
      callback = std::move(on_message_callback_);
    }
    if (gate != nullptr) {
      gate->EnterAndWait();
    }
    if (callback != nullptr) {
      return callback(id, message);
    }
    return status;
  }

  void OnHalfClose(StreamId id) override PW_LOCKS_EXCLUDED(mutex_) {
    std::lock_guard<sync::Mutex> lock(mutex_);
    if (!half_closed_streams_.full()) {
      half_closed_streams_.push_back(id);
    }
  }

  void OnCancel(StreamId id) override PW_LOCKS_EXCLUDED(mutex_) {
    Connection* reentry_connection = nullptr;
    {
      std::lock_guard<sync::Mutex> lock(mutex_);
      if (!canceled_streams_.full()) {
        canceled_streams_.push_back(id);
      }
      reentry_connection = on_cancel_reentry_connection_;
    }
    if (reentry_connection != nullptr) {
      const Status status =
          reentry_connection->SendResponseComplete(id, OkStatus());
      on_cancel_reentry_code_.store(status.code(), std::memory_order_release);
    }
  }

  void EnableOnCancelReentry(Connection& connection) PW_LOCKS_EXCLUDED(mutex_) {
    std::lock_guard<sync::Mutex> lock(mutex_);
    on_cancel_reentry_connection_ = &connection;
  }

  void SetNextOnMessageGate(MessageGate& gate) PW_LOCKS_EXCLUDED(mutex_) {
    std::lock_guard<sync::Mutex> lock(mutex_);
    message_gate_ = &gate;
  }

  void SetNextOnMessageCallback(Function<Status(StreamId, ByteSpan)>&& callback)
      PW_LOCKS_EXCLUDED(mutex_) {
    std::lock_guard<sync::Mutex> lock(mutex_);
    on_message_callback_ = std::move(callback);
  }

  Status on_cancel_reentry_status() const {
    return Status(static_cast<pw_Status>(
        on_cancel_reentry_code_.load(std::memory_order_acquire)));
  }

  size_t new_connection_count() const PW_LOCKS_EXCLUDED(mutex_) {
    std::lock_guard<sync::Mutex> lock(mutex_);
    return new_connection_count_;
  }

  size_t message_count() const PW_LOCKS_EXCLUDED(mutex_) {
    std::lock_guard<sync::Mutex> lock(mutex_);
    return message_count_;
  }

  StreamId last_stream_id() const PW_LOCKS_EXCLUDED(mutex_) {
    std::lock_guard<sync::Mutex> lock(mutex_);
    return last_stream_id_;
  }

  InlineString<kMaxMethodNameSize> last_method() const
      PW_LOCKS_EXCLUDED(mutex_) {
    std::lock_guard<sync::Mutex> lock(mutex_);
    return last_method_;
  }

  StreamId last_message_stream_id() const PW_LOCKS_EXCLUDED(mutex_) {
    std::lock_guard<sync::Mutex> lock(mutex_);
    return last_message_stream_id_;
  }

  void CopyLastMessage(ByteBuilder& out) const PW_LOCKS_EXCLUDED(mutex_) {
    std::lock_guard<sync::Mutex> lock(mutex_);
    out.clear();
    out.append(last_message_);
  }

  bool WasHalfClosed(StreamId id) const PW_LOCKS_EXCLUDED(mutex_) {
    std::lock_guard<sync::Mutex> lock(mutex_);
    return std::find(half_closed_streams_.begin(),
                     half_closed_streams_.end(),
                     id) != half_closed_streams_.end();
  }

  bool WasCanceled(StreamId id) const PW_LOCKS_EXCLUDED(mutex_) {
    std::lock_guard<sync::Mutex> lock(mutex_);
    return std::find(canceled_streams_.begin(), canceled_streams_.end(), id) !=
           canceled_streams_.end();
  }

  size_t canceled_stream_count() const PW_LOCKS_EXCLUDED(mutex_) {
    std::lock_guard<sync::Mutex> lock(mutex_);
    return canceled_streams_.size();
  }

 private:
  mutable sync::Mutex mutex_;
  size_t new_connection_count_ PW_GUARDED_BY(mutex_) = 0;
  size_t message_count_ PW_GUARDED_BY(mutex_) = 0;
  StreamId last_stream_id_ PW_GUARDED_BY(mutex_) = 0;
  InlineString<kMaxMethodNameSize> last_method_ PW_GUARDED_BY(mutex_);
  StreamId last_message_stream_id_ PW_GUARDED_BY(mutex_) = 0;
  ByteBuffer<kMaxRecordedMessageSize> last_message_ PW_GUARDED_BY(mutex_);
  Vector<StreamId, kMaxTrackedStreams> half_closed_streams_
      PW_GUARDED_BY(mutex_);
  Vector<StreamId, kMaxTrackedStreams> canceled_streams_ PW_GUARDED_BY(mutex_);
  Connection* on_cancel_reentry_connection_ PW_GUARDED_BY(mutex_) = nullptr;
  MessageGate* message_gate_ PW_GUARDED_BY(mutex_) = nullptr;
  Function<Status(StreamId, ByteSpan)> on_message_callback_
      PW_GUARDED_BY(mutex_);
  std::atomic<int> on_cancel_reentry_code_{PW_STATUS_UNKNOWN};
};

// Drives Connection::ProcessConnectionPreface() and ProcessFrame() on a
// dedicated reader thread until the stream closes or errors.
class ReaderRunner final : public thread::ThreadCore {
 public:
  ReaderRunner(Connection& connection, BlockingInMemoryReader& reader)
      : connection_(connection),
        reader_(reader),
        thread_(context_.options(), *this) {}

  ~ReaderRunner() override { StopAndJoin(); }

  ReaderRunner(const ReaderRunner&) = delete;
  ReaderRunner& operator=(const ReaderRunner&) = delete;
  ReaderRunner(ReaderRunner&&) = delete;
  ReaderRunner& operator=(ReaderRunner&&) = delete;

  void StopAndJoin() {
    reader_.Close();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  Status preface_status() const {
    return Status(
        static_cast<pw_Status>(preface_code_.load(std::memory_order_acquire)));
  }

  Status frame_status() const {
    return Status(
        static_cast<pw_Status>(frame_code_.load(std::memory_order_acquire)));
  }

 private:
  void Run() override {
    const Status preface = connection_.ProcessConnectionPreface();
    preface_code_.store(preface.code(), std::memory_order_release);
    if (preface.ok()) {
      Status status = OkStatus();
      while ((status = connection_.ProcessFrame()).ok()) {
      }
      frame_code_.store(status.code(), std::memory_order_release);
    }
    reader_.MarkReaderExited();
  }

  Connection& connection_;
  BlockingInMemoryReader& reader_;
  std::atomic<int> preface_code_{PW_STATUS_UNKNOWN};
  std::atomic<int> frame_code_{PW_STATUS_UNKNOWN};
  thread::test::TestThreadContext context_;
  thread::Thread thread_;
};

// Calls SendResponseMessage() (and optionally SendResponseComplete()) on a
// dedicated writer thread so tests can exercise send-window blocking and
// wakeups across reader and writer threads using ThreadNotifications.
class ResponseWriter final : public thread::ThreadCore {
 public:
  ResponseWriter(Connection& connection,
                 StreamId stream_id,
                 ConstByteSpan message,
                 bool complete_after_message = false)
      : connection_(connection),
        stream_id_(stream_id),
        message_(message),
        complete_after_message_(complete_after_message),
        thread_(context_.options(), *this) {}

  ~ResponseWriter() override { static_cast<void>(Join()); }

  ResponseWriter(const ResponseWriter&) = delete;
  ResponseWriter& operator=(const ResponseWriter&) = delete;
  ResponseWriter(ResponseWriter&&) = delete;
  ResponseWriter& operator=(ResponseWriter&&) = delete;

  void WaitUntilStarted() { started_.acquire(); }

  Status Join() {
    if (thread_.joinable()) {
      thread_.join();
    }
    return message_status_;
  }

  Status complete_status() const { return complete_status_; }

 private:
  void Run() override {
    started_.release();
    message_status_ = connection_.SendResponseMessage(stream_id_, message_);
    if (message_status_.ok() && complete_after_message_) {
      complete_status_ =
          connection_.SendResponseComplete(stream_id_, OkStatus());
    }
  }

  Connection& connection_;
  StreamId stream_id_;
  ConstByteSpan message_;
  bool complete_after_message_;
  sync::ThreadNotification started_;
  Status message_status_ = Status::Unknown();
  Status complete_status_ = Status::Unknown();
  thread::test::TestThreadContext context_;
  thread::Thread thread_;
};

// Answers GrpcChannelOutput lookups as a server streaming call and records
// when SendPacket() starts and finishes using the connection.
class RecordingRpcHandler : public GrpcChannelOutput::PwRpcHandlerInterface {
 public:
  void OnClose(StreamId) override {
    closed_.store(true, std::memory_order_release);
  }

  Result<rpc::MethodType> LookupMethodType(uint32_t) override {
    entered_.release();
    return rpc::MethodType::kServerStreaming;
  }

  // Blocks until SendPacket() is running inside GrpcChannelOutput.
  void WaitUntilEntered() { entered_.acquire(); }

  // True once SendPacket() has finished with the stream.
  bool closed() const { return closed_.load(std::memory_order_acquire); }

 private:
  sync::ThreadNotification entered_;
  std::atomic<bool> closed_{false};
};

// Calls GrpcChannelOutput::SendPacket() with one RESPONSE packet on a
// dedicated thread.
class PacketWriter final : public thread::ThreadCore {
 public:
  PacketWriter(GrpcChannelOutput& output,
               StreamId stream_id,
               ConstByteSpan payload)
      : output_(output),
        stream_id_(stream_id),
        payload_(payload),
        thread_(context_.options(), *this) {}

  ~PacketWriter() override { static_cast<void>(Join()); }

  PacketWriter(const PacketWriter&) = delete;
  PacketWriter& operator=(const PacketWriter&) = delete;
  PacketWriter(PacketWriter&&) = delete;
  PacketWriter& operator=(PacketWriter&&) = delete;

  Status Join() {
    if (thread_.joinable()) {
      thread_.join();
    }
    return status_;
  }

 private:
  void Run() override {
    status_ = output_.SendPacket(
        rpc::internal::Packet(rpc::internal::pwpb::PacketType::kResponse,
                              /*channel_id=*/1,
                              /*service_id=*/1,
                              /*method_id=*/1,
                              stream_id_,
                              payload_));
  }

  GrpcChannelOutput& output_;
  StreamId stream_id_;
  ConstByteSpan payload_;
  Status status_ = Status::Unknown();
  thread::test::TestThreadContext context_;
  thread::Thread thread_;
};

class RecordingAssemblyAllocator final : public allocator::ForwardingAllocator {
 public:
  explicit RecordingAssemblyAllocator(Allocator& inner)
      : ForwardingAllocator(inner) {}

  size_t allocate_calls() const {
    return allocate_calls_.load(std::memory_order_acquire);
  }

  size_t last_requested_size() const {
    return last_requested_size_.load(std::memory_order_acquire);
  }

 private:
  void* DoAllocate(Layout layout) override {
    allocate_calls_.fetch_add(1, std::memory_order_acq_rel);
    last_requested_size_.store(layout.size(), std::memory_order_release);
    return ForwardingAllocator::DoAllocate(layout);
  }

  std::atomic<size_t> allocate_calls_{0};
  std::atomic<size_t> last_requested_size_{0};
};

// Runs BasicDispatcher::Run() on a dedicated background thread.
class DispatcherRunner final : public thread::ThreadCore {
 public:
  explicit DispatcherRunner(async::BasicDispatcher& dispatcher)
      : dispatcher_(dispatcher), thread_(context_.options(), *this) {}

  ~DispatcherRunner() override { StopAndJoin(); }

  DispatcherRunner(const DispatcherRunner&) = delete;
  DispatcherRunner& operator=(const DispatcherRunner&) = delete;
  DispatcherRunner(DispatcherRunner&&) = delete;
  DispatcherRunner& operator=(DispatcherRunner&&) = delete;

  void StopAndJoin() {
    if (thread_.joinable()) {
      dispatcher_.RequestStop();
      thread_.join();
    }
  }

 private:
  void Run() override { dispatcher_.Run(); }

  async::BasicDispatcher& dispatcher_;
  thread::test::TestThreadContext context_;
  thread::Thread thread_;
};

struct ConnectionHarness {
  explicit ConnectionHarness(bool enable_assembly_allocator = false,
                             async::Dispatcher* read_dispatcher = nullptr,
                             bool use_owned_read_dispatcher = false)
      : assembly_allocator(raw_assembly_allocator),
        sync_assembly_allocator(assembly_allocator),
        active_read_dispatcher(use_owned_read_dispatcher
                                   ? &owned_read_dispatcher
                                   : read_dispatcher),
        connection(
            reader,
            send_queue,
            callbacks,
            /*message_assembly_allocator=*/
            enable_assembly_allocator ? &sync_assembly_allocator : nullptr,
            send_allocator,
            /*close_connection_callback=*/
            [this]() {
              close_connection_calls.fetch_add(1, std::memory_order_relaxed);
              const Status status =
                  connection.SendResponseComplete(/*stream_id=*/1, OkStatus());
              close_callback_reentry_code.store(status.code(),
                                                std::memory_order_release);
            },
            /*read_allocator=*/
            active_read_dispatcher != nullptr ? &read_allocator : nullptr,
            active_read_dispatcher) {}

  std::array<std::byte, kSendAllocatorPoolSize> send_pool{};
  std::array<std::byte, kAssemblyAllocatorPoolSize> assembly_pool{};
  std::array<std::byte, kReadAllocatorPoolSize> read_pool{};
  std::array<std::byte, internal::kMaxFramePayloadSize> drain_scratch{};
  allocator::BestFitAllocator<> raw_send_allocator{send_pool};
  allocator::SynchronizedAllocator<sync::Mutex> send_allocator{
      raw_send_allocator};
  allocator::BestFitAllocator<> raw_read_allocator{read_pool};
  allocator::SynchronizedAllocator<sync::Mutex> read_allocator{
      raw_read_allocator};
  allocator::BestFitAllocator<> raw_assembly_allocator{assembly_pool};
  RecordingAssemblyAllocator assembly_allocator;
  allocator::SynchronizedAllocator<sync::Mutex> sync_assembly_allocator;
  BlockingInMemoryReader reader;
  RecordingSendQueue send_queue;
  RecordingCallbacks callbacks;
  async::BasicDispatcher owned_read_dispatcher;
  async::Dispatcher* active_read_dispatcher = nullptr;
  std::atomic<size_t> close_connection_calls{0};
  std::atomic<int> close_callback_reentry_code{PW_STATUS_UNKNOWN};
  Connection connection;
  std::optional<ReaderRunner> reader_runner;

  Status close_callback_reentry_status() const {
    return Status(static_cast<pw_Status>(
        close_callback_reentry_code.load(std::memory_order_acquire)));
  }
};

void ExpectUnaryResponseFrames(RecordingSendQueue& send_queue,
                               StreamId expected_stream_id,
                               ConstByteSpan expected_payload) {
  Vector<RecordedFrame, kMaxStreamFrames> sent;
  send_queue.TakeStreamFrames(expected_stream_id, sent);
  ASSERT_EQ(sent.size(), 3U);
  EXPECT_EQ(sent[0].type, kFrameTypeHeaders);
  EXPECT_EQ(sent[0].flags & kFlagEndStream, 0);

  EXPECT_EQ(sent[1].type, kFrameTypeData);
  EXPECT_EQ(sent[1].flags & kFlagEndStream, 0);
  const ConstByteSpan data_payload = sent[1].payload();
  ASSERT_EQ(data_payload.size(),
            kLengthPrefixedMessageHdrSize + expected_payload.size());
  EXPECT_EQ(data_payload[0], std::byte{0});
  EXPECT_EQ(bytes::ReadInOrder<uint32_t>(endian::big, data_payload.data() + 1),
            static_cast<uint32_t>(expected_payload.size()));
  EXPECT_TRUE(
      std::equal(expected_payload.begin(),
                 expected_payload.end(),
                 data_payload.begin() +
                     static_cast<ptrdiff_t>(kLengthPrefixedMessageHdrSize)));

  EXPECT_EQ(sent[2].type, kFrameTypeHeaders);
  EXPECT_NE(sent[2].flags & kFlagEndStream, 0);
}

void ExpectSingleRstStreamFrame(const Vector<RecordedFrame>& frames,
                                StreamId expected_stream_id,
                                uint32_t expected_error_code) {
  ASSERT_EQ(frames.size(), 1U);
  EXPECT_EQ(frames[0].type, kFrameTypeRstStream);
  EXPECT_EQ(frames[0].flags, 0U);
  EXPECT_EQ(frames[0].stream_id, expected_stream_id);
  const ConstByteSpan payload = frames[0].payload();
  ASSERT_EQ(payload.size(), 4U);
  EXPECT_EQ(bytes::ReadInOrder<uint32_t>(endian::big, payload.data()),
            expected_error_code);
}

void ExpectSingleGoAwayFrame(const Vector<RecordedFrame>& frames,
                             StreamId expected_last_stream_id,
                             uint32_t expected_error_code) {
  ASSERT_EQ(frames.size(), 1U);
  EXPECT_EQ(frames[0].type, kFrameTypeGoAway);
  EXPECT_EQ(frames[0].flags, 0U);
  EXPECT_EQ(frames[0].stream_id, 0U);
  const ConstByteSpan payload = frames[0].payload();
  ASSERT_EQ(payload.size(), 8U);
  EXPECT_EQ(bytes::ReadInOrder<uint32_t>(endian::big, payload.data()),
            expected_last_stream_id);
  EXPECT_EQ(bytes::ReadInOrder<uint32_t>(endian::big, payload.data() + 4),
            expected_error_code);
}

// Static storage keeps Connection (>16 KiB frame scratch), allocator pools,
// and reader thread stack out of both the test stack and pw_unit_test's 16 KiB
// fixture pool without heap allocation.
std::optional<ConnectionHarness> g_harness;

class ConnectionTest : public ::testing::Test {
 protected:
  void SetUp() override { ResetHarness(/*enable_assembly_allocator=*/false); }

  void TearDown() override {
    if (g_harness.has_value()) {
      StopReader();
      g_harness.reset();
    }
  }

  void ResetHarness(bool enable_assembly_allocator,
                    async::Dispatcher* read_dispatcher = nullptr) {
    if (g_harness.has_value()) {
      StopReader();
      g_harness.reset();
    }
    g_harness.emplace(enable_assembly_allocator, read_dispatcher);
    std::fill(g_harness->drain_scratch.begin(),
              g_harness->drain_scratch.end(),
              std::byte{0x5a});
    g_harness->reader_runner.emplace(g_harness->connection, g_harness->reader);
  }

  void ResetHarnessWithReadDispatcher(bool enable_assembly_allocator = false) {
    if (g_harness.has_value()) {
      StopReader();
      g_harness.reset();
    }
    g_harness.emplace(enable_assembly_allocator,
                      /*read_dispatcher=*/nullptr,
                      /*use_owned_read_dispatcher=*/true);
    std::fill(g_harness->drain_scratch.begin(),
              g_harness->drain_scratch.end(),
              std::byte{0x5a});
    g_harness->reader_runner.emplace(g_harness->connection, g_harness->reader);
  }

  ConnectionHarness& harness() { return *g_harness; }

  void StopReader() {
    if (harness().reader_runner.has_value()) {
      harness().reader_runner->StopAndJoin();
      harness().reader_runner.reset();
    }
  }

  void StartConnection() {
    harness().reader.PushClientPreface();
    harness().reader.WaitUntilIdle();
    ASSERT_EQ(harness().reader_runner->preface_status(), OkStatus());
  }

  void StartConnectionWithWindow(uint32_t initial_window) {
    harness().reader.PushClientPrefaceWithWindow(initial_window);
    harness().reader.WaitUntilIdle();
    ASSERT_EQ(harness().reader_runner->preface_status(), OkStatus());
  }

  void OpenStream(StreamId stream_id, std::string_view path = "/") {
    harness().reader.PushRequestHeaders(stream_id, path);
    harness().reader.WaitUntilIdle();
  }

  void DrainConnectionSendWindow(size_t bytes_to_drain,
                                 StreamId drain_stream_id) {
    ASSERT_GE(bytes_to_drain, kLengthPrefixedMessageHdrSize);
    OpenStream(drain_stream_id);

    while (bytes_to_drain > 0) {
      size_t chunk_framed = std::min(
          bytes_to_drain, static_cast<size_t>(internal::kMaxFramePayloadSize));
      if (bytes_to_drain > chunk_framed &&
          bytes_to_drain - chunk_framed < kLengthPrefixedMessageHdrSize) {
        chunk_framed = bytes_to_drain - kLengthPrefixedMessageHdrSize;
      }
      const ConstByteSpan chunk(harness().drain_scratch.data(),
                                chunk_framed - kLengthPrefixedMessageHdrSize);
      ASSERT_EQ(
          harness().connection.SendResponseMessage(drain_stream_id, chunk),
          OkStatus());
      harness().send_queue.ClearStreamFrames(drain_stream_id);
      bytes_to_drain -= chunk_framed;
    }
    ASSERT_EQ(
        harness().connection.SendResponseComplete(drain_stream_id, OkStatus()),
        OkStatus());
    harness().send_queue.ClearStreamFrames(drain_stream_id);
  }
};

TEST_F(ConnectionTest, RequestDispatchPingAndUnaryResponseFraming) {
  StartConnection();
  EXPECT_EQ(harness().callbacks.new_connection_count(), 1U);
  harness().send_queue.ClearStreamFrames(/*stream_id=*/0);

  // Unknown stream IDs return NOT_FOUND.
  constexpr StreamId kUnknownStream = 99;
  EXPECT_EQ(
      harness().connection.SendResponseMessage(kUnknownStream, ConstByteSpan()),
      Status::NotFound());
  EXPECT_EQ(
      harness().connection.SendResponseComplete(kUnknownStream, OkStatus()),
      Status::NotFound());

  // Inbound PING produces an immediate PING ACK echoing the 8-byte payload.
  constexpr auto kPingPayload =
      bytes::Array<0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08>();
  harness().reader.PushPing(kPingPayload);
  harness().reader.WaitUntilIdle();

  Vector<RecordedFrame, kMaxStreamFrames> control_frames;
  harness().send_queue.TakeStreamFrames(/*stream_id=*/0, control_frames);
  ASSERT_EQ(control_frames.size(), 1U);
  EXPECT_EQ(control_frames[0].type, kFrameTypePing);
  EXPECT_EQ(control_frames[0].flags, kFlagAck);
  ASSERT_EQ(control_frames[0].payload().size(), kPingPayload.size());
  EXPECT_TRUE(std::equal(kPingPayload.begin(),
                         kPingPayload.end(),
                         control_frames[0].payload().begin()));

  // Inbound HEADERS + DATA(END_STREAM) dispatches OnNew, OnMessage, and
  // OnHalfClose.
  constexpr StreamId kStreamId = 1;
  constexpr std::string_view kMethodPath = "/test.Service/Unary";
  constexpr auto kRequestPayload = bytes::Array<0xca, 0xfe>();
  harness().reader.PushRequestHeaders(kStreamId, kMethodPath);
  harness().reader.PushRequestData(
      kStreamId, kRequestPayload, /*end_stream=*/true);
  harness().reader.WaitUntilIdle();

  EXPECT_EQ(harness().callbacks.last_stream_id(), kStreamId);
  EXPECT_EQ(std::string_view(harness().callbacks.last_method()), kMethodPath);
  EXPECT_EQ(harness().callbacks.last_message_stream_id(), kStreamId);
  ByteBuffer<kMaxRecordedMessageSize> recorded_message;
  harness().callbacks.CopyLastMessage(recorded_message);
  ASSERT_EQ(recorded_message.size(), kRequestPayload.size());
  EXPECT_TRUE(std::equal(kRequestPayload.begin(),
                         kRequestPayload.end(),
                         recorded_message.begin()));
  EXPECT_TRUE(harness().callbacks.WasHalfClosed(kStreamId));

  // Unary response emits response HEADERS, length-prefixed DATA, and END_STREAM
  // trailers.
  constexpr auto kResponsePayload = bytes::Array<0xde, 0xad, 0xbe, 0xef>();
  ASSERT_EQ(
      harness().connection.SendResponseMessage(kStreamId, kResponsePayload),
      OkStatus());
  ASSERT_EQ(harness().connection.SendResponseComplete(kStreamId, OkStatus()),
            OkStatus());
  EXPECT_EQ(harness().connection.SendResponseComplete(kStreamId, OkStatus()),
            Status::NotFound());

  Vector<RecordedFrame, kMaxStreamFrames> sent;
  harness().send_queue.TakeStreamFrames(kStreamId, sent);
  ASSERT_EQ(sent.size(), 3U);
  EXPECT_EQ(sent[0].type, kFrameTypeHeaders);
  EXPECT_EQ(sent[0].flags & kFlagEndStream, 0);

  EXPECT_EQ(sent[1].type, kFrameTypeData);
  EXPECT_EQ(sent[1].flags & kFlagEndStream, 0);
  const ConstByteSpan data_payload = sent[1].payload();
  ASSERT_EQ(data_payload.size(),
            kLengthPrefixedMessageHdrSize + kResponsePayload.size());
  EXPECT_EQ(data_payload[0], std::byte{0});
  EXPECT_EQ(bytes::ReadInOrder<uint32_t>(endian::big, data_payload.data() + 1),
            static_cast<uint32_t>(kResponsePayload.size()));
  EXPECT_TRUE(
      std::equal(kResponsePayload.begin(),
                 kResponsePayload.end(),
                 data_payload.begin() +
                     static_cast<ptrdiff_t>(kLengthPrefixedMessageHdrSize)));

  EXPECT_EQ(sent[2].type, kFrameTypeHeaders);
  EXPECT_NE(sent[2].flags & kFlagEndStream, 0);
}

TEST_F(ConnectionTest, ZeroSendWindowWaitsForWindowUpdate) {
  StartConnectionWithWindow(/*initial_window=*/0);

  constexpr auto kPayload = bytes::Array<0x10, 0x20, 0x30, 0x40, 0x50, 0x60>();
  constexpr uint32_t kFramedBytes =
      static_cast<uint32_t>(kLengthPrefixedMessageHdrSize + kPayload.size());

  // 1. Zero stream send window blocks writer until stream WINDOW_UPDATE.
  constexpr StreamId kStream1 = 1;
  OpenStream(kStream1);

  ResponseWriter stream_writer(harness().connection,
                               kStream1,
                               kPayload,
                               /*complete_after_message=*/true);
  stream_writer.WaitUntilStarted();

  harness().reader.PushWindowUpdate(kStream1, /*increment=*/kFramedBytes);
  ASSERT_EQ(stream_writer.Join(), OkStatus());
  ASSERT_EQ(stream_writer.complete_status(), OkStatus());

  Vector<RecordedFrame, kMaxStreamFrames> stream1_frames;
  harness().send_queue.TakeStreamFrames(kStream1, stream1_frames);
  EXPECT_EQ(stream1_frames.size(), 3U);

  // 2. kStream1 already consumed kFramedBytes of the connection send window.
  // Drain the remaining (kDefaultInitialWindowSize - kFramedBytes) bytes so
  // connection_send_window reaches zero while stream send windows are large.
  harness().reader.PushSettingsInitialWindow(kDefaultInitialWindowSize);
  harness().reader.WaitUntilIdle();
  DrainConnectionSendWindow(kDefaultInitialWindowSize - kFramedBytes,
                            /*drain_stream_id=*/3);

  constexpr StreamId kConnBlockedStream = 5;
  OpenStream(kConnBlockedStream);

  ResponseWriter conn_writer(harness().connection,
                             kConnBlockedStream,
                             kPayload,
                             /*complete_after_message=*/true);
  conn_writer.WaitUntilStarted();

  harness().reader.PushWindowUpdate(/*stream_id=*/0,
                                    /*increment=*/kFramedBytes);
  ASSERT_EQ(conn_writer.Join(), OkStatus());
  ASSERT_EQ(conn_writer.complete_status(), OkStatus());

  Vector<RecordedFrame, kMaxStreamFrames> conn_frames;
  harness().send_queue.TakeStreamFrames(kConnBlockedStream, conn_frames);
  EXPECT_EQ(conn_frames.size(), 3U);
}

TEST_F(ConnectionTest, NegativeWindowFromSettingsShrinkWaitsUntilRestored) {
  constexpr uint32_t kInitialWindow = 32;
  StartConnectionWithWindow(kInitialWindow);

  constexpr StreamId kStreamId = 1;
  OpenStream(kStreamId);

  // Send 11-byte payload (16 bytes framed), leaving send_window == 16.
  constexpr auto kFirstPayload =
      bytes::Array<1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11>();
  constexpr uint32_t kFirstFramedBytes = static_cast<uint32_t>(
      kLengthPrefixedMessageHdrSize + kFirstPayload.size());
  ASSERT_EQ(harness().connection.SendResponseMessage(kStreamId, kFirstPayload),
            OkStatus());
  harness().send_queue.ClearStreamFrames(kStreamId);

  // Shrink SETTINGS_INITIAL_WINDOW_SIZE from 32 to 0 -> send_window == -16.
  harness().reader.PushSettingsInitialWindow(/*initial_window=*/0);
  harness().reader.WaitUntilIdle();

  constexpr auto kSecondPayload = bytes::Array<0xa1, 0xb2, 0xc3, 0xd4>();
  constexpr uint32_t kSecondFramedBytes = static_cast<uint32_t>(
      kLengthPrefixedMessageHdrSize + kSecondPayload.size());

  ResponseWriter writer(harness().connection,
                        kStreamId,
                        kSecondPayload,
                        /*complete_after_message=*/true);
  writer.WaitUntilStarted();

  // Clearing only the negative deficit (send_window -> 0) keeps the writer
  // waiting for positive credit.
  harness().reader.PushWindowUpdate(kStreamId, /*increment=*/kFirstFramedBytes);
  harness().reader.WaitUntilIdle();

  harness().reader.PushWindowUpdate(kStreamId,
                                    /*increment=*/kSecondFramedBytes);
  ASSERT_EQ(writer.Join(), OkStatus());
  ASSERT_EQ(writer.complete_status(), OkStatus());

  Vector<RecordedFrame, kMaxStreamFrames> sent;
  harness().send_queue.TakeStreamFrames(kStreamId, sent);
  EXPECT_EQ(sent.size(), 2U);
}

TEST_F(ConnectionTest, ZeroConnectionWindowUpdateSendsGoAwayWithoutDeadlock) {
  StartConnection();
  harness().callbacks.EnableOnCancelReentry(harness().connection);
  harness().send_queue.ClearStreamFrames(/*stream_id=*/0);

  constexpr StreamId kStreamId = 1;
  OpenStream(kStreamId);

  harness().reader.PushWindowUpdate(/*stream_id=*/0, /*increment=*/0);
  harness().reader.WaitUntilIdle();
  EXPECT_EQ(harness().reader_runner->frame_status(), Status::Internal());

  EXPECT_EQ(harness().callbacks.canceled_stream_count(), 1U);
  EXPECT_TRUE(harness().callbacks.WasCanceled(kStreamId));
  EXPECT_EQ(harness().callbacks.on_cancel_reentry_status(),
            Status::Unavailable());
  EXPECT_EQ(harness().close_connection_calls.load(), 1U);
  EXPECT_EQ(harness().close_callback_reentry_status(), Status::Unavailable());

  Vector<RecordedFrame, kMaxStreamFrames> control_frames;
  harness().send_queue.TakeStreamFrames(/*stream_id=*/0, control_frames);
  ExpectSingleGoAwayFrame(control_frames, kStreamId, kHttp2ErrorProtocolError);
}

TEST_F(ConnectionTest,
       ConnectionWindowUpdateOverflowSendsGoAwayWithoutDeadlock) {
  StartConnection();
  harness().callbacks.EnableOnCancelReentry(harness().connection);
  harness().send_queue.ClearStreamFrames(/*stream_id=*/0);

  constexpr StreamId kStreamId = 1;
  OpenStream(kStreamId);

  constexpr uint32_t kMinOverflowIncrement =
      kMaxFlowControlWindow - kDefaultInitialWindowSize + 1;
  harness().reader.PushWindowUpdate(/*stream_id=*/0, kMinOverflowIncrement);
  harness().reader.WaitUntilIdle();
  EXPECT_EQ(harness().reader_runner->frame_status(), Status::Internal());

  EXPECT_EQ(harness().callbacks.canceled_stream_count(), 1U);
  EXPECT_TRUE(harness().callbacks.WasCanceled(kStreamId));
  EXPECT_EQ(harness().callbacks.on_cancel_reentry_status(),
            Status::Unavailable());
  EXPECT_EQ(harness().close_connection_calls.load(), 1U);
  EXPECT_EQ(harness().close_callback_reentry_status(), Status::Unavailable());

  Vector<RecordedFrame, kMaxStreamFrames> control_frames;
  harness().send_queue.TakeStreamFrames(/*stream_id=*/0, control_frames);
  ExpectSingleGoAwayFrame(
      control_frames, kStreamId, kHttp2ErrorFlowControlError);
}

TEST_F(ConnectionTest,
       SettingsInitialWindowOverflowSendsGoAwayWithoutDeadlock) {
  StartConnectionWithWindow(/*initial_window=*/0);
  harness().callbacks.EnableOnCancelReentry(harness().connection);
  harness().send_queue.ClearStreamFrames(/*stream_id=*/0);

  constexpr auto kPayload = bytes::Array<0x01, 0x02, 0x03, 0x04>();

  // Open slot 0 at send_window == 0 and start a writer waiting for window.
  constexpr StreamId kBlockedStream = 1;
  OpenStream(kBlockedStream);

  ResponseWriter blocked_writer(harness().connection, kBlockedStream, kPayload);
  blocked_writer.WaitUntilStarted();

  // Open slot 1 and raise its send_window above initial_send_window_ so
  // SETTINGS_INITIAL_WINDOW_SIZE overflows slot 1 while fitting slot 0.
  constexpr StreamId kRaisedStream = 3;
  constexpr uint32_t kRaisedStreamIncrement = 16;
  OpenStream(kRaisedStream);
  harness().reader.PushWindowUpdate(kRaisedStream, kRaisedStreamIncrement);
  harness().reader.WaitUntilIdle();

  constexpr uint32_t kOverflowInitialWindow =
      kMaxFlowControlWindow - kRaisedStreamIncrement + 1;
  harness().reader.PushSettingsInitialWindow(kOverflowInitialWindow);
  harness().reader.WaitUntilIdle();
  EXPECT_EQ(harness().reader_runner->frame_status(), Status::Internal());

  const Status blocked_status = blocked_writer.Join();
  EXPECT_TRUE(blocked_status.IsUnavailable() || blocked_status.IsNotFound());
  Vector<RecordedFrame, kMaxStreamFrames> blocked_frames;
  harness().send_queue.TakeStreamFrames(kBlockedStream, blocked_frames);
  EXPECT_TRUE(blocked_frames.empty());
  EXPECT_EQ(harness().callbacks.canceled_stream_count(), 2U);
  EXPECT_TRUE(harness().callbacks.WasCanceled(kBlockedStream));
  EXPECT_TRUE(harness().callbacks.WasCanceled(kRaisedStream));
  EXPECT_EQ(harness().callbacks.on_cancel_reentry_status(),
            Status::Unavailable());
  EXPECT_EQ(harness().close_connection_calls.load(), 1U);
  EXPECT_EQ(harness().close_callback_reentry_status(), Status::Unavailable());

  Vector<RecordedFrame, kMaxStreamFrames> control_frames;
  harness().send_queue.TakeStreamFrames(/*stream_id=*/0, control_frames);
  ExpectSingleGoAwayFrame(
      control_frames, kRaisedStream, kHttp2ErrorFlowControlError);
}

TEST_F(ConnectionTest, WaitingSendUnblocksOnResetOrDisconnect) {
  StartConnectionWithWindow(/*initial_window=*/0);

  constexpr auto kPayload = bytes::Array<0x11, 0x22>();

  // 1. RST_STREAM cancels the stream and unblocks a writer waiting in
  // SendResponseMessage() with Status::Unavailable() (or Status::NotFound() if
  // the reset is processed before SendResponseMessage() acquires the state lock
  // for the first time).
  constexpr StreamId kRstStream = 1;
  OpenStream(kRstStream);

  ResponseWriter rst_writer(harness().connection, kRstStream, kPayload);
  rst_writer.WaitUntilStarted();

  harness().reader.PushRstStream(kRstStream, kHttp2ErrorCancel);
  harness().reader.WaitUntilIdle();
  const Status rst_status = rst_writer.Join();
  EXPECT_TRUE(rst_status.IsUnavailable() || rst_status.IsNotFound());
  EXPECT_TRUE(harness().callbacks.WasCanceled(kRstStream));

  // 2. Reader EOF closes the connection and similarly unblocks a waiting writer
  // with Status::Unavailable() (or Status::NotFound() if EOF closes the stream
  // before SendResponseMessage() acquires the state lock).
  constexpr StreamId kCloseStream = 3;
  OpenStream(kCloseStream);

  ResponseWriter close_writer(harness().connection, kCloseStream, kPayload);
  close_writer.WaitUntilStarted();

  StopReader();
  const Status close_status = close_writer.Join();
  EXPECT_TRUE(close_status.IsUnavailable() || close_status.IsNotFound());
}

TEST_F(ConnectionTest, EmptyResponseWaitsUntilFiveBytePrefixWindowAvailable) {
  constexpr uint32_t kShortfall = 1;
  constexpr uint32_t kWindowBelowPrefix =
      static_cast<uint32_t>(kLengthPrefixedMessageHdrSize) - kShortfall;
  StartConnectionWithWindow(/*initial_window=*/0);

  constexpr StreamId kStreamId = 1;
  OpenStream(kStreamId);

  ResponseWriter writer(harness().connection,
                        kStreamId,
                        ConstByteSpan(),
                        /*complete_after_message=*/true);
  writer.WaitUntilStarted();

  // Raise the stream window to 4 bytes (still below the 5-byte gRPC message
  // prefix) before granting the final byte, exercising the post-wakeup loop
  // re-check.
  harness().reader.PushWindowUpdate(kStreamId,
                                    /*increment=*/kWindowBelowPrefix);
  harness().reader.WaitUntilIdle();

  harness().reader.PushWindowUpdate(kStreamId, /*increment=*/kShortfall);
  ASSERT_EQ(writer.Join(), OkStatus());
  ASSERT_EQ(writer.complete_status(), OkStatus());

  Vector<RecordedFrame, kMaxStreamFrames> sent;
  harness().send_queue.TakeStreamFrames(kStreamId, sent);
  ASSERT_EQ(sent.size(), 3U);
  EXPECT_EQ(sent[0].type, kFrameTypeHeaders);
  EXPECT_EQ(sent[1].type, kFrameTypeData);
  ASSERT_EQ(sent[1].payload().size(), kLengthPrefixedMessageHdrSize);
  EXPECT_EQ(sent[1].payload()[0], std::byte{0});
  EXPECT_EQ(
      bytes::ReadInOrder<uint32_t>(endian::big, sent[1].payload().data() + 1),
      0U);
  EXPECT_EQ(sent[2].type, kFrameTypeHeaders);
  EXPECT_NE(sent[2].flags & kFlagEndStream, 0);
}

TEST_F(ConnectionTest,
       NonEmptyResponseWaitsWhenStreamOrConnectionWindowExcludesPrefix) {
  constexpr auto kPayload = bytes::Array<0x01, 0x02, 0x03, 0x04, 0x05, 0x06>();
  constexpr uint32_t kFramedBytes =
      static_cast<uint32_t>(kLengthPrefixedMessageHdrSize + kPayload.size());
  constexpr uint32_t kShortfall = 3;
  constexpr uint32_t kWindowMissingPrefixBytes = kFramedBytes - kShortfall;
  static_assert(kWindowMissingPrefixBytes >= kPayload.size());
  static_assert(kWindowMissingPrefixBytes < kFramedBytes);

  StartConnectionWithWindow(/*initial_window=*/0);

  // 1. Stream window holds kPayload.size() bytes without room for the 5-byte
  // gRPC message prefix.
  constexpr StreamId kStream1 = 1;
  OpenStream(kStream1);

  ResponseWriter stream_writer(harness().connection,
                               kStream1,
                               kPayload,
                               /*complete_after_message=*/true);
  stream_writer.WaitUntilStarted();

  harness().reader.PushWindowUpdate(kStream1,
                                    /*increment=*/kWindowMissingPrefixBytes);
  harness().reader.WaitUntilIdle();

  harness().reader.PushWindowUpdate(kStream1, /*increment=*/kShortfall);
  ASSERT_EQ(stream_writer.Join(), OkStatus());
  ASSERT_EQ(stream_writer.complete_status(), OkStatus());

  Vector<RecordedFrame, kMaxStreamFrames> stream1_frames;
  harness().send_queue.TakeStreamFrames(kStream1, stream1_frames);
  EXPECT_EQ(stream1_frames.size(), 3U);

  // 2. Connection window holds kPayload.size() bytes without room for the
  // 5-byte gRPC message prefix while stream send window is large.
  harness().reader.PushSettingsInitialWindow(kDefaultInitialWindowSize);
  harness().reader.WaitUntilIdle();
  constexpr size_t kRemainingConnWindowAfterStream1 =
      kDefaultInitialWindowSize - kFramedBytes;
  DrainConnectionSendWindow(kRemainingConnWindowAfterStream1,
                            /*drain_stream_id=*/3);

  constexpr StreamId kConnBlockedStream = 5;
  OpenStream(kConnBlockedStream);

  ResponseWriter conn_writer(harness().connection,
                             kConnBlockedStream,
                             kPayload,
                             /*complete_after_message=*/true);
  conn_writer.WaitUntilStarted();

  harness().reader.PushWindowUpdate(/*stream_id=*/0,
                                    /*increment=*/kWindowMissingPrefixBytes);
  harness().reader.WaitUntilIdle();

  harness().reader.PushWindowUpdate(/*stream_id=*/0, /*increment=*/kShortfall);
  ASSERT_EQ(conn_writer.Join(), OkStatus());
  ASSERT_EQ(conn_writer.complete_status(), OkStatus());

  Vector<RecordedFrame, kMaxStreamFrames> conn_frames;
  harness().send_queue.TakeStreamFrames(kConnBlockedStream, conn_frames);
  EXPECT_EQ(conn_frames.size(), 3U);
}

TEST_F(ConnectionTest, ZeroSendWindowWakesAllWritersOnStream) {
  StartConnectionWithWindow(/*initial_window=*/0);

  constexpr auto kPayload = bytes::Array<0x31, 0x32, 0x33>();
  constexpr uint32_t kFramedBytes =
      static_cast<uint32_t>(kLengthPrefixedMessageHdrSize + kPayload.size());

  // Two writers block on the same stream. One WINDOW_UPDATE with room for both
  // messages must wake both of them.
  constexpr StreamId kStreamId = 1;
  OpenStream(kStreamId);

  ResponseWriter writer_a(harness().connection, kStreamId, kPayload);
  ResponseWriter writer_b(harness().connection, kStreamId, kPayload);
  writer_a.WaitUntilStarted();
  writer_b.WaitUntilStarted();

  harness().reader.PushWindowUpdate(kStreamId, /*increment=*/2 * kFramedBytes);
  EXPECT_EQ(writer_a.Join(), OkStatus());
  EXPECT_EQ(writer_b.Join(), OkStatus());
  ASSERT_EQ(harness().connection.SendResponseComplete(kStreamId, OkStatus()),
            OkStatus());

  // Response HEADERS, two DATA frames, and END_STREAM trailers.
  Vector<RecordedFrame, kMaxStreamFrames> sent;
  harness().send_queue.TakeStreamFrames(kStreamId, sent);
  EXPECT_EQ(sent.size(), 4U);
}

TEST_F(ConnectionTest, ResetStreamSlotReuseKeepsWriterWakeups) {
  StartConnectionWithWindow(/*initial_window=*/0);

  constexpr auto kPayload = bytes::Array<0x41, 0x42>();
  constexpr uint32_t kFramedBytes =
      static_cast<uint32_t>(kLengthPrefixedMessageHdrSize + kPayload.size());

  // A writer blocks on stream 1. RST_STREAM wakes it, and stream 3 reuses the
  // freed stream slot before the old writer runs. A writer on stream 3 must
  // not take the old writer's wakeup, and must wait for its own window.
  constexpr StreamId kResetStream = 1;
  constexpr StreamId kNewStream = 3;
  OpenStream(kResetStream);

  ResponseWriter old_writer(harness().connection, kResetStream, kPayload);
  old_writer.WaitUntilStarted();

  harness().reader.PushRstStream(kResetStream, kHttp2ErrorCancel);
  harness().reader.PushRequestHeaders(kNewStream);
  harness().reader.WaitUntilIdle();

  ResponseWriter new_writer(harness().connection, kNewStream, kPayload);
  new_writer.WaitUntilStarted();

  const Status old_status = old_writer.Join();
  EXPECT_TRUE(old_status.IsUnavailable() || old_status.IsNotFound());
  EXPECT_TRUE(harness().callbacks.WasCanceled(kResetStream));
  Vector<RecordedFrame, kMaxStreamFrames> early_frames;
  harness().send_queue.TakeStreamFrames(kNewStream, early_frames);
  EXPECT_TRUE(early_frames.empty());

  harness().reader.PushWindowUpdate(kNewStream, /*increment=*/kFramedBytes);
  EXPECT_EQ(new_writer.Join(), OkStatus());

  Vector<RecordedFrame, kMaxStreamFrames> sent;
  harness().send_queue.TakeStreamFrames(kNewStream, sent);
  EXPECT_EQ(sent.size(), 2U);
}

TEST_F(ConnectionTest, ClearConnectionRejectsSendsAndStopsReader) {
  StartConnection();
  harness().send_queue.ClearStreamFrames(/*stream_id=*/0);

  constexpr auto kPayload = bytes::Array<0x71, 0x72>();
  constexpr StreamId kStreamId = 1;
  OpenStream(kStreamId);

  RecordingRpcHandler handler;
  GrpcChannelOutput output;
  output.set_callbacks(handler);
  output.set_connection(harness().connection);
  output.clear_connection();

  EXPECT_EQ(harness().connection.SendResponseMessage(kStreamId, kPayload),
            Status::Unavailable());
  EXPECT_EQ(harness().connection.SendResponseComplete(kStreamId, OkStatus()),
            Status::Unavailable());

  // The transport is still open, but the reader must stop at the next frame
  // instead of processing it. A PING would otherwise get a PING ACK.
  constexpr auto kPingPayload =
      bytes::Array<0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08>();
  harness().reader.PushPing(kPingPayload);
  StopReader();
  Vector<RecordedFrame, kMaxStreamFrames> control_frames;
  harness().send_queue.TakeStreamFrames(/*stream_id=*/0, control_frames);
  EXPECT_TRUE(control_frames.empty());
  Vector<RecordedFrame, kMaxStreamFrames> stream_frames;
  harness().send_queue.TakeStreamFrames(kStreamId, stream_frames);
  EXPECT_TRUE(stream_frames.empty());
}

TEST_F(ConnectionTest, ClearConnectionWaitsForBlockedSendPacket) {
  StartConnectionWithWindow(/*initial_window=*/0);

  constexpr auto kPayload = bytes::Array<0x51, 0x52>();
  constexpr StreamId kStreamId = 1;
  OpenStream(kStreamId);

  RecordingRpcHandler handler;
  GrpcChannelOutput output;
  output.set_callbacks(handler);
  output.set_connection(harness().connection);

  // SendPacket() blocks on the zero send window. clear_connection() must wake
  // it and wait until it is done with the connection, so the caller can
  // destroy the connection right after.
  PacketWriter writer(output, kStreamId, kPayload);
  handler.WaitUntilEntered();

  output.clear_connection();
  EXPECT_TRUE(handler.closed());

  // Reader EOF closes the connection, which wakes the writer if
  // clear_connection() did not.
  StopReader();
  EXPECT_EQ(writer.Join(), Status::Unavailable());

  EXPECT_EQ(output.SendPacket(rpc::internal::Packet(
                rpc::internal::pwpb::PacketType::kServerStream,
                /*channel_id=*/1,
                /*service_id=*/1,
                /*method_id=*/1,
                kStreamId)),
            Status::FailedPrecondition());
}

TEST_F(ConnectionTest, DestroyConnectionAfterClearConnection) {
  StartConnectionWithWindow(/*initial_window=*/0);

  constexpr auto kPayload = bytes::Array<0x61, 0x62>();
  constexpr StreamId kStreamId = 1;
  OpenStream(kStreamId);

  RecordingRpcHandler handler;
  GrpcChannelOutput output;
  output.set_callbacks(handler);
  output.set_connection(harness().connection);

  PacketWriter writer(output, kStreamId, kPayload);
  handler.WaitUntilEntered();

  // Server teardown order: reader EOF closes the connection and wakes the
  // writer, then the server clears and destroys the connection.
  StopReader();
  output.clear_connection();
  EXPECT_TRUE(handler.closed());
  g_harness.reset();

  EXPECT_EQ(writer.Join(), Status::Unavailable());
}

TEST_F(ConnectionTest, OversizedMessagePrefixResetsStreamWithoutAllocating) {
  ResetHarness(/*enable_assembly_allocator=*/true);
  StartConnection();

  constexpr auto kInitialChunk = bytes::Array<0xaa>();

  // 1. UINT32_MAX message_length resets without calling the allocator.
  constexpr StreamId kMaxUint32Stream = 1;
  constexpr uint32_t kMaxUint32Length = 0xffffffffU;
  OpenStream(kMaxUint32Stream);
  harness().reader.PushLengthPrefixAndChunk(kMaxUint32Stream,
                                            kMaxUint32Length,
                                            kInitialChunk,
                                            /*end_stream=*/false);
  harness().reader.WaitUntilIdle();

  EXPECT_EQ(harness().assembly_allocator.allocate_calls(), 0U);
  EXPECT_TRUE(harness().callbacks.WasCanceled(kMaxUint32Stream));
  EXPECT_EQ(harness().callbacks.last_message_stream_id(), 0U);

  Vector<RecordedFrame, kMaxStreamFrames> max_uint32_rst;
  harness().send_queue.TakeStreamFrames(kMaxUint32Stream, max_uint32_rst);
  ExpectSingleRstStreamFrame(
      max_uint32_rst, kMaxUint32Stream, kHttp2ErrorEnhanceYourCalm);

  // 2. (kMaxGrpcMessageAssemblySize + 1) also resets without allocating.
  constexpr StreamId kOverLimitStream = 3;
  OpenStream(kOverLimitStream);
  harness().reader.PushLengthPrefixAndChunk(
      kOverLimitStream,
      internal::kMaxGrpcMessageAssemblySize + 1U,
      kInitialChunk,
      /*end_stream=*/false);
  harness().reader.WaitUntilIdle();

  EXPECT_EQ(harness().assembly_allocator.allocate_calls(), 0U);
  EXPECT_TRUE(harness().callbacks.WasCanceled(kOverLimitStream));

  Vector<RecordedFrame, kMaxStreamFrames> over_limit_rst;
  harness().send_queue.TakeStreamFrames(kOverLimitStream, over_limit_rst);
  ExpectSingleRstStreamFrame(
      over_limit_rst, kOverLimitStream, kHttp2ErrorEnhanceYourCalm);

  // 3. Subsequent streams on the same connection continue operating normally.
  constexpr StreamId kHealthyStream = 5;
  constexpr auto kValidPayload = bytes::Array<0x12, 0x34>();
  OpenStream(kHealthyStream);
  harness().reader.PushRequestData(
      kHealthyStream, kValidPayload, /*end_stream=*/true);
  harness().reader.WaitUntilIdle();
  EXPECT_EQ(harness().callbacks.last_message_stream_id(), kHealthyStream);
  EXPECT_TRUE(harness().callbacks.WasHalfClosed(kHealthyStream));
}

TEST_F(ConnectionTest,
       FragmentedMessageWithinMaxLimitUsesFailableAssemblyAllocator) {
  ResetHarness(/*enable_assembly_allocator=*/true);
  StartConnection();

  // 1. Fragmented message that fits in the bounded pool reassembles cleanly.
  constexpr StreamId kValidStream = 1;
  constexpr auto kPart1 = bytes::Array<1, 2, 3, 4, 5>();
  constexpr auto kPart2 = bytes::Array<6, 7, 8, 9, 10, 11, 12>();
  constexpr uint32_t kValidLength =
      static_cast<uint32_t>(kPart1.size() + kPart2.size());
  OpenStream(kValidStream);
  harness().reader.PushLengthPrefixAndChunk(
      kValidStream, kValidLength, kPart1, /*end_stream=*/false);
  harness().reader.PushRawDataFrame(kValidStream, kPart2, /*end_stream=*/true);
  harness().reader.WaitUntilIdle();

  EXPECT_EQ(harness().assembly_allocator.allocate_calls(), 1U);
  EXPECT_EQ(harness().assembly_allocator.last_requested_size(), kValidLength);
  EXPECT_EQ(harness().callbacks.last_message_stream_id(), kValidStream);
  ByteBuffer<kMaxRecordedMessageSize> assembled;
  harness().callbacks.CopyLastMessage(assembled);
  ASSERT_EQ(assembled.size(), kValidLength);
  EXPECT_TRUE(std::equal(kPart1.begin(), kPart1.end(), assembled.begin()));
  EXPECT_TRUE(std::equal(
      kPart2.begin(), kPart2.end(), assembled.begin() + kPart1.size()));
  EXPECT_TRUE(harness().callbacks.WasHalfClosed(kValidStream));
  EXPECT_EQ(harness().raw_assembly_allocator.GetAllocated(), 0U);

  // 2. Exact kMaxGrpcMessageAssemblySize passes the static cap, fails cleanly
  // in the smaller failable pool, and resets with INTERNAL_ERROR.
  constexpr StreamId kAtMaxLimitStream = 3;
  OpenStream(kAtMaxLimitStream);
  harness().reader.PushLengthPrefixAndChunk(
      kAtMaxLimitStream,
      internal::kMaxGrpcMessageAssemblySize,
      kPart1,
      /*end_stream=*/false);
  harness().reader.WaitUntilIdle();

  EXPECT_EQ(harness().assembly_allocator.allocate_calls(), 2U);
  EXPECT_EQ(harness().assembly_allocator.last_requested_size(),
            internal::kMaxGrpcMessageAssemblySize);
  EXPECT_TRUE(harness().callbacks.WasCanceled(kAtMaxLimitStream));
  EXPECT_EQ(harness().raw_assembly_allocator.GetAllocated(), 0U);

  Vector<RecordedFrame, kMaxStreamFrames> rst_frames;
  harness().send_queue.TakeStreamFrames(kAtMaxLimitStream, rst_frames);
  ExpectSingleRstStreamFrame(
      rst_frames, kAtMaxLimitStream, kHttp2ErrorInternalError);
}

TEST_F(ConnectionTest, AsyncDataDispatchesQueuedFramesInFifoOrder) {
  async::BasicDispatcher read_dispatcher;
  ResetHarness(/*enable_assembly_allocator=*/false, &read_dispatcher);
  StartConnection();

  constexpr StreamId kStreamId = 1;
  constexpr auto kPayload = bytes::Array<0x11, 0x22, 0x33, 0x44>();
  OpenStream(kStreamId);
  harness().reader.PushRequestData(kStreamId, kPayload, /*end_stream=*/false);
  harness().reader.PushRawDataFrame(
      kStreamId, ConstByteSpan(), /*end_stream=*/true);
  harness().reader.WaitUntilIdle();

  EXPECT_EQ(harness().callbacks.message_count(), 0U);
  EXPECT_FALSE(harness().callbacks.WasHalfClosed(kStreamId));

  read_dispatcher.RunUntilIdle();
  EXPECT_EQ(harness().callbacks.message_count(), 1U);
  EXPECT_EQ(harness().callbacks.last_message_stream_id(), kStreamId);
  EXPECT_TRUE(harness().callbacks.WasHalfClosed(kStreamId));
  EXPECT_EQ(harness().raw_read_allocator.GetAllocated(), 0U);

  StopReader();
  g_harness.reset();
  read_dispatcher.RunUntilIdle();
}

TEST_F(ConnectionTest, AsyncDataTeardownCancelsQueuedTasksAndFreesBuffers) {
  async::BasicDispatcher read_dispatcher;
  ResetHarness(/*enable_assembly_allocator=*/false, &read_dispatcher);
  StartConnection();

  constexpr StreamId kStreamId = 1;
  constexpr auto kPayload = bytes::Array<0x11, 0x22, 0x33, 0x44>();
  OpenStream(kStreamId);
  harness().reader.PushRequestData(kStreamId, kPayload, /*end_stream=*/false);
  harness().reader.PushRequestData(kStreamId, kPayload, /*end_stream=*/true);
  harness().reader.WaitUntilIdle();

  EXPECT_GT(harness().raw_read_allocator.GetAllocated(), 0U);
  EXPECT_EQ(harness().callbacks.message_count(), 0U);

  // Reader EOF and Connection destruction must cancel queued read_dispatcher
  // work and release all read_allocator blocks before read_dispatcher runs.
  StopReader();
  EXPECT_EQ(harness().raw_read_allocator.GetAllocated(), 0U);
  g_harness.reset();

  read_dispatcher.RunUntilIdle();
}

TEST_F(ConnectionTest, AsyncDataTeardownWaitsForInFlightOnMessageCallback) {
  async::BasicDispatcher read_dispatcher;
  DispatcherRunner dispatcher_runner(read_dispatcher);
  ResetHarness(/*enable_assembly_allocator=*/false, &read_dispatcher);
  StartConnectionWithWindow(/*initial_window=*/0);

  MessageGate gate;
  harness().callbacks.SetNextOnMessageGate(gate);

  constexpr StreamId kStreamId = 1;
  constexpr auto kPayload1 = bytes::Array<0xaa, 0xbb>();
  constexpr auto kPayload2 = bytes::Array<0xcc, 0xdd>();
  constexpr auto kResponsePayload = bytes::Array<0x61, 0x62>();
  OpenStream(kStreamId);
  harness().reader.PushRequestData(kStreamId, kPayload1, /*end_stream=*/false);
  gate.WaitUntilEntered();

  // Queue a second DATA(END_STREAM) frame while OnMessage() is in flight on
  // read_dispatcher.
  harness().reader.PushRequestData(kStreamId, kPayload2, /*end_stream=*/true);
  harness().reader.WaitUntilIdle();

  // Park a writer on the zero send window so CloseStreams() at the start of
  // Reader::Drain() signals deterministically when ReaderRunner enters Drain()
  // while OnMessage() is still blocked on gate.
  RecordingRpcHandler rpc_handler;
  GrpcChannelOutput output;
  output.set_callbacks(rpc_handler);
  output.set_connection(harness().connection);
  PacketWriter writer(output, kStreamId, kResponsePayload);
  rpc_handler.WaitUntilEntered();

  harness().reader.Close();
  EXPECT_EQ(writer.Join(), Status::Unavailable());

  gate.AllowReturn();
  StopReader();
  EXPECT_EQ(harness().callbacks.message_count(), 1U);
  EXPECT_FALSE(harness().callbacks.WasHalfClosed(kStreamId));
  EXPECT_EQ(harness().raw_read_allocator.GetAllocated(), 0U);

  output.clear_connection();
  g_harness.reset();
  dispatcher_runner.StopAndJoin();
}

TEST_F(ConnectionTest, StreamIdOrderingAndIdleRstStreamEchoLastStreamId) {
  // 1. Out-of-order HEADERS stream ID sends GOAWAY with the highest opened
  // stream ID.
  StartConnection();
  harness().send_queue.ClearStreamFrames(/*stream_id=*/0);

  constexpr StreamId kFirstStream = 1;
  constexpr StreamId kSecondStream = 5;
  constexpr StreamId kRegressionStream = 3;
  OpenStream(kFirstStream);
  OpenStream(kSecondStream);

  harness().reader.PushRequestHeaders(kRegressionStream);
  harness().reader.WaitUntilIdle();
  EXPECT_EQ(harness().reader_runner->frame_status(), Status::Internal());

  {
    Vector<RecordedFrame, kMaxStreamFrames> goaway_frames;
    harness().send_queue.TakeStreamFrames(/*stream_id=*/0, goaway_frames);
    ExpectSingleGoAwayFrame(
        goaway_frames, kSecondStream, kHttp2ErrorProtocolError);
  }

  // 2. RST_STREAM on an idle stream (> last_stream_id) sends GOAWAY with
  // last_stream_id.
  ResetHarness(/*enable_assembly_allocator=*/false);
  StartConnection();
  harness().send_queue.ClearStreamFrames(/*stream_id=*/0);

  constexpr StreamId kOpenedStream = 3;
  constexpr StreamId kIdleStream = kOpenedStream + 2;
  OpenStream(kOpenedStream);

  harness().reader.PushRstStream(kIdleStream, kHttp2ErrorCancel);
  harness().reader.WaitUntilIdle();
  EXPECT_EQ(harness().reader_runner->frame_status(), Status::Internal());

  Vector<RecordedFrame, kMaxStreamFrames> goaway_frames;
  harness().send_queue.TakeStreamFrames(/*stream_id=*/0, goaway_frames);
  ExpectSingleGoAwayFrame(
      goaway_frames, kOpenedStream, kHttp2ErrorProtocolError);
}

TEST_F(ConnectionTest, CloseConnectionInvokesCallbackAtMostOnce) {
  class CloseCaller final : public thread::ThreadCore {
   public:
    explicit CloseCaller(Connection& connection)
        : connection_(connection), thread_(context_.options(), *this) {}

    ~CloseCaller() override { Join(); }

    void Join() {
      if (thread_.joinable()) {
        thread_.join();
      }
    }

   private:
    void Run() override { connection_.CloseConnection(); }

    Connection& connection_;
    thread::test::TestThreadContext context_;
    thread::Thread thread_;
  };

  // 1. After SendGoAway invokes the close callback, explicit CloseConnection()
  // calls during server teardown do not invoke it again.
  StartConnection();
  harness().send_queue.ClearStreamFrames(/*stream_id=*/0);
  harness().reader.PushWindowUpdate(/*stream_id=*/0, /*increment=*/0);
  harness().reader.WaitUntilIdle();
  EXPECT_EQ(harness().close_connection_calls.load(), 1U);

  harness().connection.CloseConnection();
  harness().connection.CloseConnection();
  EXPECT_EQ(harness().close_connection_calls.load(), 1U);

  // 2. Concurrent CloseConnection() calls on a live connection invoke the
  // callback once without data races.
  ResetHarness(/*enable_assembly_allocator=*/false);
  StartConnection();
  {
    CloseCaller caller_a(harness().connection);
    CloseCaller caller_b(harness().connection);
    caller_a.Join();
    caller_b.Join();
  }
  EXPECT_EQ(harness().close_connection_calls.load(), 1U);
}

TEST_F(ConnectionTest, RstStreamInvokesOnCancelWithoutHoldingLock) {
  StartConnection();
  harness().callbacks.EnableOnCancelReentry(harness().connection);

  constexpr StreamId kCanceledStream = 1;
  OpenStream(kCanceledStream);

  harness().reader.PushRstStream(kCanceledStream, kHttp2ErrorCancel);
  harness().reader.WaitUntilIdle();

  EXPECT_EQ(harness().callbacks.canceled_stream_count(), 1U);
  EXPECT_TRUE(harness().callbacks.WasCanceled(kCanceledStream));
  EXPECT_EQ(harness().callbacks.on_cancel_reentry_status(), Status::NotFound());

  constexpr StreamId kHealthyStream = 3;
  constexpr auto kPayload = bytes::Array<0xab, 0xcd>();
  OpenStream(kHealthyStream);
  harness().reader.PushRequestData(
      kHealthyStream, kPayload, /*end_stream=*/true);
  harness().reader.WaitUntilIdle();
  EXPECT_EQ(harness().callbacks.last_message_stream_id(), kHealthyStream);
  EXPECT_TRUE(harness().callbacks.WasHalfClosed(kHealthyStream));
}

TEST_F(ConnectionTest, ZeroStreamWindowUpdateResetsStreamWithoutHoldingLock) {
  StartConnection();
  harness().callbacks.EnableOnCancelReentry(harness().connection);

  constexpr StreamId kStreamId = 1;
  OpenStream(kStreamId);

  harness().reader.PushWindowUpdate(kStreamId, /*increment=*/0);
  harness().reader.WaitUntilIdle();

  EXPECT_EQ(harness().callbacks.canceled_stream_count(), 1U);
  EXPECT_TRUE(harness().callbacks.WasCanceled(kStreamId));
  EXPECT_EQ(harness().callbacks.on_cancel_reentry_status(), Status::NotFound());

  Vector<RecordedFrame, kMaxStreamFrames> rst_frames;
  harness().send_queue.TakeStreamFrames(kStreamId, rst_frames);
  ExpectSingleRstStreamFrame(rst_frames, kStreamId, kHttp2ErrorProtocolError);
}

TEST_F(ConnectionTest, DataOnHalfClosedStreamResetsStreamWithoutHoldingLock) {
  StartConnection();
  harness().callbacks.EnableOnCancelReentry(harness().connection);

  constexpr StreamId kStreamId = 1;
  constexpr auto kPayload = bytes::Array<0x01, 0x02>();
  OpenStream(kStreamId);
  harness().reader.PushRequestData(kStreamId, kPayload, /*end_stream=*/true);
  harness().reader.WaitUntilIdle();
  ASSERT_TRUE(harness().callbacks.WasHalfClosed(kStreamId));

  harness().reader.PushRequestData(kStreamId, kPayload, /*end_stream=*/false);
  harness().reader.WaitUntilIdle();

  EXPECT_EQ(harness().callbacks.canceled_stream_count(), 1U);
  EXPECT_TRUE(harness().callbacks.WasCanceled(kStreamId));
  EXPECT_EQ(harness().callbacks.on_cancel_reentry_status(), Status::NotFound());

  Vector<RecordedFrame, kMaxStreamFrames> rst_frames;
  harness().send_queue.TakeStreamFrames(kStreamId, rst_frames);
  ExpectSingleRstStreamFrame(rst_frames, kStreamId, kHttp2ErrorStreamClosed);
}

TEST_F(ConnectionTest, OversizedIgnoredDataOnClosedStreamSendsFrameSizeError) {
  StartConnection();
  harness().callbacks.EnableOnCancelReentry(harness().connection);

  constexpr StreamId kClosedStream = 1;
  OpenStream(kClosedStream);
  ASSERT_EQ(
      harness().connection.SendResponseComplete(kClosedStream, OkStatus()),
      OkStatus());
  harness().send_queue.ClearStreamFrames(kClosedStream);

  constexpr StreamId kActiveStream = 3;
  OpenStream(kActiveStream);
  harness().send_queue.ClearStreamFrames(/*stream_id=*/0);

  constexpr auto kTrailingBytes = bytes::Array<0xaa, 0xbb>();
  harness().reader.PushFrameHeaderWithPartialPayload(
      internal::kMaxFramePayloadSize + 1U,
      kFrameTypeData,
      /*flags=*/0,
      kClosedStream,
      kTrailingBytes);
  harness().reader.WaitUntilIdle();

  EXPECT_EQ(harness().reader_runner->frame_status(), Status::Internal());
  EXPECT_TRUE(harness().callbacks.WasCanceled(kActiveStream));
  EXPECT_EQ(harness().callbacks.on_cancel_reentry_status(),
            Status::Unavailable());
  EXPECT_EQ(harness().close_connection_calls.load(), 1U);

  Vector<RecordedFrame, kMaxStreamFrames> control_frames;
  harness().send_queue.TakeStreamFrames(/*stream_id=*/0, control_frames);
  ExpectSingleGoAwayFrame(
      control_frames, kActiveStream, kHttp2ErrorFrameSizeError);
}

TEST_F(ConnectionTest,
       MaxLengthIgnoredDataOnHalfClosedStreamSendsFrameSizeError) {
  StartConnection();
  harness().callbacks.EnableOnCancelReentry(harness().connection);

  constexpr StreamId kHalfClosedStream = 1;
  OpenStream(kHalfClosedStream);
  harness().reader.PushRawDataFrame(
      kHalfClosedStream, ConstByteSpan(), /*end_stream=*/true);
  harness().reader.WaitUntilIdle();
  ASSERT_TRUE(harness().callbacks.WasHalfClosed(kHalfClosedStream));
  harness().send_queue.ClearStreamFrames(/*stream_id=*/0);

  constexpr auto kTrailingBytes = bytes::Array<0xcc, 0xdd>();
  harness().reader.PushFrameHeaderWithPartialPayload(kMax24BitFrameLength,
                                                     kFrameTypeData,
                                                     /*flags=*/0,
                                                     kHalfClosedStream,
                                                     kTrailingBytes);
  harness().reader.WaitUntilIdle();

  EXPECT_EQ(harness().reader_runner->frame_status(), Status::Internal());
  EXPECT_TRUE(harness().callbacks.WasCanceled(kHalfClosedStream));
  EXPECT_EQ(harness().close_connection_calls.load(), 1U);

  Vector<RecordedFrame, kMaxStreamFrames> control_frames;
  harness().send_queue.TakeStreamFrames(/*stream_id=*/0, control_frames);
  ExpectSingleGoAwayFrame(
      control_frames, kHalfClosedStream, kHttp2ErrorFrameSizeError);
}

TEST_F(ConnectionTest, OversizedHeadersWithEndStreamSendsFrameSizeError) {
  StartConnection();
  harness().send_queue.ClearStreamFrames(/*stream_id=*/0);

  constexpr StreamId kStreamId = 1;
  constexpr auto kTrailingBytes = bytes::Array<0x84, 0x00>();
  harness().reader.PushFrameHeaderWithPartialPayload(
      internal::kMaxFramePayloadSize + 1U,
      kFrameTypeHeaders,
      kFlagEndHeaders | kFlagEndStream,
      kStreamId,
      kTrailingBytes);
  harness().reader.WaitUntilIdle();

  EXPECT_EQ(harness().reader_runner->frame_status(), Status::Internal());
  EXPECT_EQ(harness().close_connection_calls.load(), 1U);

  Vector<RecordedFrame, kMaxStreamFrames> control_frames;
  harness().send_queue.TakeStreamFrames(/*stream_id=*/0, control_frames);
  ExpectSingleGoAwayFrame(control_frames, kStreamId, kHttp2ErrorFrameSizeError);
}

TEST_F(ConnectionTest,
       ExactMaxIgnoredFrameSucceedsAndOversizedPrioritySendsFrameSizeError) {
  StartConnection();

  constexpr StreamId kClosedStream = 1;
  OpenStream(kClosedStream);
  ASSERT_EQ(
      harness().connection.SendResponseComplete(kClosedStream, OkStatus()),
      OkStatus());
  harness().send_queue.ClearStreamFrames(kClosedStream);
  harness().send_queue.ClearStreamFrames(/*stream_id=*/0);

  // An ignored DATA frame at exact kMaxFramePayloadSize succeeds cleanly.
  const ConstByteSpan exact_max_payload(harness().drain_scratch.data(),
                                        internal::kMaxFramePayloadSize);
  harness().reader.PushRawDataFrame(
      kClosedStream, exact_max_payload, /*end_stream=*/false);
  harness().reader.WaitUntilIdle();
  EXPECT_EQ(harness().reader_runner->frame_status(), Status::Unknown());

  // An oversized PRIORITY frame (> kMaxFramePayloadSize) sends GOAWAY with
  // FRAME_SIZE_ERROR without reading payload bytes.
  constexpr auto kTrailingBytes = bytes::Array<0x00, 0x00, 0x00, 0x00, 0x10>();
  harness().reader.PushFrameHeaderWithPartialPayload(
      internal::kMaxFramePayloadSize + 1U,
      kFrameTypePriority,
      /*flags=*/0,
      kClosedStream,
      kTrailingBytes);
  harness().reader.WaitUntilIdle();

  EXPECT_EQ(harness().reader_runner->frame_status(), Status::Internal());

  Vector<RecordedFrame, kMaxStreamFrames> control_frames;
  harness().send_queue.TakeStreamFrames(/*stream_id=*/0, control_frames);
  ExpectSingleGoAwayFrame(
      control_frames, kClosedStream, kHttp2ErrorFrameSizeError);
}

TEST_F(ConnectionTest,
       LatchedStreamInOnMessageSuppressesTrailingPayloadAndHalfClose) {
  ResetHarnessWithReadDispatcher(/*enable_assembly_allocator=*/true);
  StartConnection();
  harness().send_queue.ClearStreamFrames(/*stream_id=*/0);

  constexpr StreamId kStreamId = 1;
  constexpr auto kFirstRequest = bytes::Array<0x01, 0x02>();
  constexpr auto kSecondRequest = bytes::Array<0x03, 0x04>();
  constexpr auto kFragmentChunk = bytes::Array<0xaa>();
  constexpr uint32_t kFragmentedLength = 20;
  static constexpr auto kResponsePayload = bytes::Array<0xde, 0xad>();

  harness().callbacks.SetNextOnMessageCallback([this](StreamId id,
                                                      ByteSpan /*message*/) {
    EXPECT_EQ(harness().connection.SendResponseMessage(id, kResponsePayload),
              OkStatus());
    harness().send_queue.SetRejectSends(/*reject=*/true);
    EXPECT_EQ(harness().connection.SendResponseComplete(id, OkStatus()),
              Status::Unavailable());
    return OkStatus();
  });

  OpenStream(kStreamId);
  ByteBuffer<kFrameScratchSize> coalesced_payload;
  AppendLengthPrefixedChunk(coalesced_payload,
                            static_cast<uint32_t>(kFirstRequest.size()),
                            kFirstRequest);
  AppendLengthPrefixedChunk(coalesced_payload,
                            static_cast<uint32_t>(kSecondRequest.size()),
                            kSecondRequest);
  AppendLengthPrefixedChunk(
      coalesced_payload, kFragmentedLength, kFragmentChunk);
  ASSERT_EQ(coalesced_payload.status(), OkStatus());
  harness().reader.PushRawDataFrame(
      kStreamId, coalesced_payload, /*end_stream=*/true);
  harness().reader.WaitUntilIdle();

  harness().owned_read_dispatcher.RunUntilIdle();
  EXPECT_EQ(harness().callbacks.message_count(), 1U);
  EXPECT_EQ(harness().assembly_allocator.allocate_calls(), 0U);
  EXPECT_EQ(harness().raw_assembly_allocator.GetAllocated(), 0U);
  EXPECT_FALSE(harness().callbacks.WasHalfClosed(kStreamId));
  EXPECT_FALSE(harness().callbacks.WasCanceled(kStreamId));

  harness().send_queue.SetRejectSends(/*reject=*/false);
  harness().owned_read_dispatcher.RunUntilIdle();

  ExpectUnaryResponseFrames(harness().send_queue, kStreamId, kResponsePayload);
}

TEST_F(ConnectionTest, LatchedStreamInOnMessageIgnoresCallbackErrorWithoutRst) {
  ResetHarnessWithReadDispatcher(/*enable_assembly_allocator=*/false);
  StartConnection();
  harness().send_queue.ClearStreamFrames(/*stream_id=*/0);

  constexpr StreamId kStreamId = 1;
  constexpr auto kRequestPayload = bytes::Array<0x10, 0x20>();
  static constexpr auto kResponsePayload = bytes::Array<0x30, 0x40>();

  // Re-open the send queue before returning Status::Internal() while
  // drain_task_ is still queued behind RunReadTask() on owned_read_dispatcher,
  // proving ProcessDataFramePayload() does not emit RST_STREAM on a latched
  // stream.
  harness().callbacks.SetNextOnMessageCallback([this](StreamId id,
                                                      ByteSpan /*message*/) {
    EXPECT_EQ(harness().connection.SendResponseMessage(id, kResponsePayload),
              OkStatus());
    harness().send_queue.SetRejectSends(/*reject=*/true);
    EXPECT_EQ(harness().connection.SendResponseComplete(id, OkStatus()),
              Status::Unavailable());
    harness().send_queue.SetRejectSends(/*reject=*/false);
    return Status::Internal();
  });

  OpenStream(kStreamId);
  harness().reader.PushRequestData(
      kStreamId, kRequestPayload, /*end_stream=*/false);
  harness().reader.WaitUntilIdle();
  harness().owned_read_dispatcher.RunUntilIdle();

  EXPECT_FALSE(harness().callbacks.WasCanceled(kStreamId));
  ExpectUnaryResponseFrames(harness().send_queue, kStreamId, kResponsePayload);
}

TEST_F(ConnectionTest,
       DataOnHalfClosedStreamLatchedDuringIgnoredReadPreservesResponse) {
  ResetHarnessWithReadDispatcher(/*enable_assembly_allocator=*/false);
  StartConnection();
  harness().send_queue.ClearStreamFrames(/*stream_id=*/0);

  constexpr StreamId kStreamId = 1;
  constexpr auto kRequestPayload = bytes::Array<0x01, 0x02>();
  constexpr auto kIgnoredPayload = bytes::Array<0x99, 0x98, 0x97, 0x96>();
  constexpr auto kResponsePayload = bytes::Array<0xab, 0xcd>();

  OpenStream(kStreamId);
  harness().reader.PushRequestData(
      kStreamId, kRequestPayload, /*end_stream=*/true);
  harness().reader.WaitUntilIdle();
  harness().owned_read_dispatcher.RunUntilIdle();
  ASSERT_TRUE(harness().callbacks.WasHalfClosed(kStreamId));

  // Push only the extra DATA header so ProcessDataFrame enters the half-closed
  // branch, unlocks SharedState, and blocks inside ProcessIgnoredFrame().
  harness().reader.PushDataFrameHeader(
      kStreamId,
      static_cast<uint32_t>(kIgnoredPayload.size()),
      /*end_stream=*/false);
  harness().reader.WaitUntilIdle();

  ASSERT_EQ(
      harness().connection.SendResponseMessage(kStreamId, kResponsePayload),
      OkStatus());
  harness().send_queue.SetRejectSends(/*reject=*/true);
  ASSERT_EQ(harness().connection.SendResponseComplete(kStreamId, OkStatus()),
            Status::Unavailable());
  harness().send_queue.SetRejectSends(/*reject=*/false);

  harness().reader.Push(kIgnoredPayload);
  harness().reader.WaitUntilIdle();
  harness().owned_read_dispatcher.RunUntilIdle();

  EXPECT_FALSE(harness().callbacks.WasCanceled(kStreamId));
  ExpectUnaryResponseFrames(harness().send_queue, kStreamId, kResponsePayload);
}

TEST_F(ConnectionTest,
       AsyncReadAllocFailureAfterStreamLatchedPreservesResponse) {
  ResetHarnessWithReadDispatcher(/*enable_assembly_allocator=*/false);
  StartConnection();
  harness().send_queue.ClearStreamFrames(/*stream_id=*/0);

  constexpr StreamId kStreamId = 1;
  constexpr StreamId kFillStreamId = 3;
  constexpr size_t kFillFrameBytes = kReaderBufferSize - kFrameHeaderSize;
  constexpr size_t kFillMessageBytes =
      kFillFrameBytes - kLengthPrefixedMessageHdrSize;
  constexpr auto kResponsePayload = bytes::Array<0x55, 0x66>();

  OpenStream(kStreamId);
  OpenStream(kFillStreamId);

  const ConstByteSpan fill_message(harness().drain_scratch.data(),
                                   kFillMessageBytes);
  while (harness().raw_read_allocator.GetMaxAllocatable() >= kFillFrameBytes) {
    const size_t prev_allocated = harness().raw_read_allocator.GetAllocated();
    harness().reader.PushRequestData(
        kFillStreamId, fill_message, /*end_stream=*/false);
    harness().reader.WaitUntilIdle();
    ASSERT_GT(harness().raw_read_allocator.GetAllocated(), prev_allocated);
  }

  // Push only the DATA header on kStreamId so ProcessDataFrame passes the
  // initial unlatched check, unlocks SharedState, and waits in
  // ReadFramePayload().
  const ConstByteSpan dropped_payload(harness().drain_scratch.data(),
                                      kFillFrameBytes);
  harness().reader.PushDataFrameHeader(
      kStreamId,
      static_cast<uint32_t>(dropped_payload.size()),
      /*end_stream=*/false);
  harness().reader.WaitUntilIdle();

  ASSERT_EQ(
      harness().connection.SendResponseMessage(kStreamId, kResponsePayload),
      OkStatus());
  harness().send_queue.SetRejectSends(/*reject=*/true);
  ASSERT_EQ(harness().connection.SendResponseComplete(kStreamId, OkStatus()),
            Status::Unavailable());
  harness().send_queue.SetRejectSends(/*reject=*/false);

  // Supplying the payload triggers read_allocator exhaustion (!alloc_ok) after
  // kStreamId has already latched END_STREAM.
  harness().reader.Push(dropped_payload);
  harness().reader.WaitUntilIdle();
  harness().owned_read_dispatcher.RunUntilIdle();

  EXPECT_FALSE(harness().callbacks.WasCanceled(kStreamId));
  ExpectUnaryResponseFrames(harness().send_queue, kStreamId, kResponsePayload);
}

}  // namespace
}  // namespace pw::grpc
