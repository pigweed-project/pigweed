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
#include "pw_allocator/synchronized_allocator.h"
#include "pw_bytes/array.h"
#include "pw_bytes/byte_builder.h"
#include "pw_bytes/endian.h"
#include "pw_bytes/span.h"
#include "pw_containers/inline_deque.h"
#include "pw_containers/vector.h"
#include "pw_grpc/send_queue.h"
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
constexpr uint32_t kHttp2ErrorProtocolError = 0x01;
constexpr uint32_t kHttp2ErrorFlowControlError = 0x03;
constexpr uint32_t kHttp2ErrorCancel = 0x08;

constexpr size_t kReaderBufferSize = 512;
constexpr size_t kFrameScratchSize = 128;
constexpr size_t kMaxRecordedFrames = 32;
constexpr size_t kMaxStreamFrames = 8;
constexpr size_t kMaxRecordedMessageSize = 64;
constexpr size_t kMaxTrackedStreams = 8;
constexpr size_t kSendAllocatorPoolSize = 32 * 1024;

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

// Blocking in-memory reader that parks the reader thread on a
// ThreadNotification when empty and notifies the test thread once all pushed
// bytes have been consumed and processed up to the next frame boundary.
class BlockingInMemoryReader : public stream::NonSeekableReader {
 public:
  void Push(ConstByteSpan data) PW_LOCKS_EXCLUDED(mutex_) {
    if (data.empty()) {
      return;
    }
    {
      std::lock_guard<sync::Mutex> lock(mutex_);
      for (std::byte b : data) {
        ASSERT_FALSE(buffer_.full());
        buffer_.push_back(b);
      }
      static_cast<void>(idle_notification_.try_acquire());
    }
    readable_.release();
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
    ByteBuffer<kFrameScratchSize> bytes;
    const uint32_t framed_len =
        static_cast<uint32_t>(kLengthPrefixedMessageHdrSize + message.size());
    AppendFrameHeader(bytes,
                      framed_len,
                      kFrameTypeData,
                      end_stream ? kFlagEndStream : 0,
                      stream_id);
    constexpr uint8_t kUncompressedFlag = 0;
    bytes.PutUint8(kUncompressedFlag);
    bytes.PutUint32(static_cast<uint32_t>(message.size()), endian::big);
    bytes.append(message);
    ASSERT_EQ(bytes.status(), OkStatus());
    Push(bytes);
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
        if (buffer_.empty() && (reader_waiting_ || reader_exited_)) {
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
    idle_notification_.release();
  }

 private:
  StatusWithSize DoRead(ByteSpan dest) override PW_LOCKS_EXCLUDED(mutex_) {
    while (true) {
      {
        std::lock_guard<sync::Mutex> lock(mutex_);
        if (!buffer_.empty()) {
          const size_t to_copy = std::min<size_t>(dest.size(), buffer_.size());
          for (size_t i = 0; i < to_copy; ++i) {
            dest[i] = buffer_.front();
            buffer_.pop_front();
          }
          return StatusWithSize(to_copy);
        }
        if (closed_) {
          return StatusWithSize::OutOfRange();
        }
        reader_waiting_ = true;
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
    if (buffer == nullptr || buffer.size() < kFrameHeaderSize ||
        frames_.full()) {
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
  void Run() override {}
  void RequestStop() override {}

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
    std::lock_guard<sync::Mutex> lock(mutex_);
    last_message_stream_id_ = id;
    last_message_.clear();
    last_message_.append(message);
    return last_message_.status();
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

  Status on_cancel_reentry_status() const {
    return Status(static_cast<pw_Status>(
        on_cancel_reentry_code_.load(std::memory_order_acquire)));
  }

  size_t new_connection_count() const PW_LOCKS_EXCLUDED(mutex_) {
    std::lock_guard<sync::Mutex> lock(mutex_);
    return new_connection_count_;
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
  StreamId last_stream_id_ PW_GUARDED_BY(mutex_) = 0;
  InlineString<kMaxMethodNameSize> last_method_ PW_GUARDED_BY(mutex_);
  StreamId last_message_stream_id_ PW_GUARDED_BY(mutex_) = 0;
  ByteBuffer<kMaxRecordedMessageSize> last_message_ PW_GUARDED_BY(mutex_);
  Vector<StreamId, kMaxTrackedStreams> half_closed_streams_
      PW_GUARDED_BY(mutex_);
  Vector<StreamId, kMaxTrackedStreams> canceled_streams_ PW_GUARDED_BY(mutex_);
  Connection* on_cancel_reentry_connection_ PW_GUARDED_BY(mutex_) = nullptr;
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

struct ConnectionHarness {
  std::array<std::byte, kSendAllocatorPoolSize> send_pool{};
  std::array<std::byte, internal::kMaxGrpcMessageSize> drain_scratch{};
  allocator::BestFitAllocator<> raw_send_allocator{send_pool};
  allocator::SynchronizedAllocator<sync::Mutex> send_allocator{
      raw_send_allocator};
  BlockingInMemoryReader reader;
  RecordingSendQueue send_queue;
  RecordingCallbacks callbacks;
  std::atomic<size_t> close_connection_calls{0};
  std::atomic<int> close_callback_reentry_code{PW_STATUS_UNKNOWN};
  Connection connection{reader,
                        send_queue,
                        callbacks,
                        /*message_assembly_allocator=*/nullptr,
                        send_allocator,
                        /*close_connection_callback=*/[this]() {
                          close_connection_calls.fetch_add(
                              1, std::memory_order_relaxed);
                          const Status status = connection.SendResponseComplete(
                              /*stream_id=*/1, OkStatus());
                          close_callback_reentry_code.store(
                              status.code(), std::memory_order_release);
                        }};
  std::optional<ReaderRunner> reader_runner;

  Status close_callback_reentry_status() const {
    return Status(static_cast<pw_Status>(
        close_callback_reentry_code.load(std::memory_order_acquire)));
  }
};

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
  void SetUp() override {
    g_harness.emplace();
    std::fill(g_harness->drain_scratch.begin(),
              g_harness->drain_scratch.end(),
              std::byte{0x5a});
    g_harness->reader_runner.emplace(g_harness->connection, g_harness->reader);
  }

  void TearDown() override {
    if (g_harness.has_value()) {
      StopReader();
      g_harness.reset();
    }
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
  EXPECT_EQ(harness().callbacks.on_cancel_reentry_status(), Status::NotFound());
  EXPECT_EQ(harness().close_connection_calls.load(), 1U);
  EXPECT_EQ(harness().close_callback_reentry_status(), Status::NotFound());

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
  EXPECT_EQ(harness().callbacks.on_cancel_reentry_status(), Status::NotFound());
  EXPECT_EQ(harness().close_connection_calls.load(), 1U);
  EXPECT_EQ(harness().close_callback_reentry_status(), Status::NotFound());

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
  EXPECT_EQ(harness().callbacks.on_cancel_reentry_status(), Status::NotFound());
  EXPECT_EQ(harness().close_connection_calls.load(), 1U);
  EXPECT_EQ(harness().close_callback_reentry_status(), Status::NotFound());

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

}  // namespace
}  // namespace pw::grpc
