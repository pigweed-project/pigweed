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

#include "pw_grpc/default_send_queue.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>

#include "pw_allocator/best_fit.h"
#include "pw_allocator/synchronized_allocator.h"
#include "pw_bytes/span.h"
#include "pw_grpc/send_queue.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"
#include "pw_stream/stream.h"
#include "pw_sync/lock_annotations.h"
#include "pw_sync/mutex.h"
#include "pw_sync/thread_notification.h"
#include "pw_thread/test_thread_context.h"
#include "pw_thread/thread.h"
#include "pw_unit_test/framework.h"

namespace pw::grpc {
namespace {

class RecordingStreamWriter : public stream::NonSeekableReaderWriter {
 public:
  explicit RecordingStreamWriter(size_t target_bytes)
      : target_bytes_(target_bytes) {}

  void WaitUntilTargetWritten() { target_reached_.acquire(); }

  size_t total_written() const PW_LOCKS_EXCLUDED(mutex_) {
    std::lock_guard<sync::Mutex> lock(mutex_);
    return total_written_;
  }

  ConstByteSpan bytes() const PW_LOCKS_EXCLUDED(mutex_) {
    std::lock_guard<sync::Mutex> lock(mutex_);
    return ConstByteSpan(buffer_.data(), total_written_);
  }

 private:
  StatusWithSize DoRead(ByteSpan /*dest*/) override {
    return StatusWithSize::OutOfRange();
  }

  Status DoWrite(ConstByteSpan data) override PW_LOCKS_EXCLUDED(mutex_) {
    bool reached = false;
    {
      std::lock_guard<sync::Mutex> lock(mutex_);
      const size_t copy =
          std::min(data.size(), buffer_.size() - total_written_);
      std::memcpy(buffer_.data() + total_written_, data.data(), copy);
      total_written_ += copy;
      reached = total_written_ >= target_bytes_;
    }
    if (reached) {
      target_reached_.release();
    }
    return OkStatus();
  }

  mutable sync::Mutex mutex_;
  std::array<std::byte, 4096> buffer_{};
  size_t total_written_ PW_GUARDED_BY(mutex_) = 0;
  size_t target_bytes_;
  sync::ThreadNotification target_reached_;
};

class PatternFrameSource : public FrameSource {
 public:
  explicit PatternFrameSource(size_t total_bytes) : remaining_(total_bytes) {}

  StatusWithSize FillPacket(ByteSpan packet) override {
    if (block_next_pull_.exchange(false, std::memory_order_acq_rel)) {
      entered_pull_.release();
      release_pull_.acquire();
      pull_released_.store(true, std::memory_order_release);
    }
    if (remaining_ == 0) {
      return StatusWithSize(0);
    }
    const size_t n = std::min(packet.size(), remaining_);
    for (size_t i = 0; i < n; ++i) {
      packet[i] = static_cast<std::byte>((offset_ + i) & 0xFFU);
    }
    offset_ += n;
    remaining_ -= n;
    return StatusWithSize(n);
  }

  void SetBlockNextPull() {
    pull_released_.store(false, std::memory_order_release);
    block_next_pull_.store(true, std::memory_order_release);
  }
  void WaitUntilEnteredPull() { entered_pull_.acquire(); }
  void ReleasePull() { release_pull_.release(); }
  bool pull_released() const {
    return pull_released_.load(std::memory_order_acquire);
  }

 private:
  size_t remaining_;
  size_t offset_ = 0;
  std::atomic<bool> block_next_pull_{false};
  std::atomic<bool> pull_released_{false};
  sync::ThreadNotification entered_pull_;
  sync::ThreadNotification release_pull_;
};

TEST(DefaultSendQueuePullTest, PullDrainsMultiPacketStream) {
  constexpr size_t kTotalBytes =
      DefaultSendQueue::kDefaultPullPacketSize * 2 + 19;
  std::array<std::byte, 1024> pool{};
  allocator::BestFitAllocator<> raw_allocator(pool);
  allocator::SynchronizedAllocator<sync::Mutex> allocator(raw_allocator);
  RecordingStreamWriter writer(kTotalBytes);
  PatternFrameSource source(kTotalBytes);
  DefaultSendQueue send_queue(writer, allocator);
  EXPECT_TRUE(send_queue.SupportsPull());

  thread::test::TestThreadContext send_ctx;
  thread::Thread send_thread(send_ctx.options(), [&]() { send_queue.Run(); });

  send_queue.SetFrameSource(&source);
  writer.WaitUntilTargetWritten();

  ASSERT_EQ(writer.total_written(), kTotalBytes);
  const ConstByteSpan received = writer.bytes();
  for (size_t i = 0; i < kTotalBytes; ++i) {
    EXPECT_EQ(received[i], static_cast<std::byte>(i & 0xFFU));
  }

  send_queue.SetFrameSource(nullptr);
  send_queue.RequestStop();
  send_thread.join();
}

TEST(DefaultSendQueuePullTest, ReplaceFrameSourceWaitsForInFlightPull) {
  std::array<std::byte, 1024> pool{};
  allocator::BestFitAllocator<> raw_allocator(pool);
  allocator::SynchronizedAllocator<sync::Mutex> allocator(raw_allocator);
  RecordingStreamWriter writer(/*target_bytes=*/8);
  PatternFrameSource blocking_source(/*total_bytes=*/8);
  PatternFrameSource replacement_source(/*total_bytes=*/0);
  DefaultSendQueue send_queue(writer, allocator);

  thread::test::TestThreadContext send_ctx;
  thread::Thread send_thread(send_ctx.options(), [&]() { send_queue.Run(); });

  blocking_source.SetBlockNextPull();
  send_queue.SetFrameSource(&blocking_source);
  blocking_source.WaitUntilEnteredPull();

  struct ReplacerState {
    DefaultSendQueue& send_queue;
    PatternFrameSource& blocking_source;
    PatternFrameSource& replacement_source;
    sync::ThreadNotification started{};
    std::atomic<bool> replaced{false};
    std::atomic<bool> released_before_replace{false};
  } state{send_queue, blocking_source, replacement_source};
  thread::test::TestThreadContext replacer_ctx;
  thread::Thread replacer(replacer_ctx.options(), [&state]() {
    state.started.release();
    state.send_queue.SetFrameSource(&state.replacement_source);
    state.released_before_replace.store(state.blocking_source.pull_released(),
                                        std::memory_order_release);
    state.replaced.store(true, std::memory_order_release);
  });
  state.started.acquire();
  EXPECT_FALSE(state.replaced.load(std::memory_order_acquire));

  blocking_source.ReleasePull();
  replacer.join();
  EXPECT_TRUE(state.replaced.load(std::memory_order_acquire));
  EXPECT_TRUE(state.released_before_replace.load(std::memory_order_acquire));

  send_queue.SetFrameSource(nullptr);
  send_queue.RequestStop();
  send_thread.join();
}

TEST(DefaultSendQueuePullTest, ClearFrameSourceWaitsForInFlightPull) {
  std::array<std::byte, 1024> pool{};
  allocator::BestFitAllocator<> raw_allocator(pool);
  allocator::SynchronizedAllocator<sync::Mutex> allocator(raw_allocator);
  RecordingStreamWriter writer(/*target_bytes=*/8);
  PatternFrameSource blocking_source(/*total_bytes=*/8);
  DefaultSendQueue send_queue(writer, allocator);

  thread::test::TestThreadContext send_ctx;
  thread::Thread send_thread(send_ctx.options(), [&]() { send_queue.Run(); });

  blocking_source.SetBlockNextPull();
  send_queue.SetFrameSource(&blocking_source);
  blocking_source.WaitUntilEnteredPull();

  struct CloserState {
    DefaultSendQueue& send_queue;
    PatternFrameSource& blocking_source;
    sync::ThreadNotification started{};
    std::atomic<bool> cleared{false};
    std::atomic<bool> released_before_clear{false};
  } state{send_queue, blocking_source};
  thread::test::TestThreadContext closer_ctx;
  thread::Thread closer(closer_ctx.options(), [&state]() {
    state.started.release();
    state.send_queue.SetFrameSource(nullptr);
    state.released_before_clear.store(state.blocking_source.pull_released(),
                                      std::memory_order_release);
    state.cleared.store(true, std::memory_order_release);
  });
  state.started.acquire();
  EXPECT_FALSE(state.cleared.load(std::memory_order_acquire));

  blocking_source.ReleasePull();
  closer.join();
  EXPECT_TRUE(state.cleared.load(std::memory_order_acquire));
  EXPECT_TRUE(state.released_before_clear.load(std::memory_order_acquire));

  send_queue.RequestStop();
  send_thread.join();
}

TEST(DefaultSendQueuePullTest, DestroyWithoutRunDoesNotCrash) {
  std::array<std::byte, 1024> pool{};
  allocator::BestFitAllocator<> raw_allocator(pool);
  allocator::SynchronizedAllocator<sync::Mutex> allocator(raw_allocator);
  RecordingStreamWriter writer(/*target_bytes=*/1);
  PatternFrameSource source(/*total_bytes=*/8);

  {
    DefaultSendQueue send_queue(writer, allocator);
    send_queue.SetFrameSource(&source);
    send_queue.SetFrameSource(nullptr);
  }
  {
    DefaultSendQueue send_queue(writer, allocator);
    send_queue.SetFrameSource(&source);
  }
}

}  // namespace
}  // namespace pw::grpc
