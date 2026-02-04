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

#include <cstddef>

#include "pw_allocator/allocator.h"
#include "pw_allocator/testing.h"
#include "pw_assert/assert.h"
#include "pw_async2/basic_dispatcher.h"
#include "pw_async2/context.h"
#include "pw_async2/func_task.h"
#include "pw_async2/poll.h"
#include "pw_async2/try.h"
#include "pw_async2/waker.h"
#include "pw_bytes/span.h"
#include "pw_multibuf/multibuf.h"
#include "pw_unit_test/framework.h"

namespace examples {

// DOCSTAG: [pw_multibuf-examples-async_queue-observer]
class AsyncMultiBufQueueObserver : public pw::multibuf::Observer {
 public:
  pw::async2::Poll<> PendNotFull(pw::async2::Context& context) {
    PW_ASYNC_STORE_WAKER(context, full_waker_, "waiting for space");
    return pw::async2::Pending();
  }

  pw::async2::Poll<> PendNotEmpty(pw::async2::Context& context) {
    PW_ASYNC_STORE_WAKER(context, empty_waker_, "waiting for data");
    return pw::async2::Pending();
  }

 private:
  void DoNotify(Event event, size_t) override {
    if (event == Event::kBytesAdded) {
      empty_waker_.Wake();
    } else if (event == Event::kBytesRemoved) {
      full_waker_.Wake();
    }
  }

  pw::async2::Waker empty_waker_;
  pw::async2::Waker full_waker_;
};
// DOCSTAG: [pw_multibuf-examples-async_queue-observer]

// DOCSTAG: [pw_multibuf-examples-async_queue]
class AsyncMultiBufQueue {
 public:
  AsyncMultiBufQueue(pw::Allocator& allocator, size_t max_chunks)
      : mbuf_(allocator) {
    PW_ASSERT(mbuf_->TryReserveChunks(max_chunks));
    mbuf_->set_observer(&observer_);
  }

  pw::async2::Poll<> PendNotFull(pw::async2::Context& context) {
    return full() ? observer_.PendNotFull(context) : pw::async2::Ready();
  }

  pw::async2::Poll<> PendNotEmpty(pw::async2::Context& context) {
    return empty() ? observer_.PendNotEmpty(context) : pw::async2::Ready();
  }

 private:
  AsyncMultiBufQueueObserver observer_;
  pw::TrackedConstMultiBuf::Instance mbuf_;

  // Remaining methods are the same as MultiBufQueue....
  // DOCSTAG: [pw_multibuf-examples-async_queue]
 public:
  [[nodiscard]] bool empty() const { return mbuf_->empty(); }

  [[nodiscard]] bool full() const { return mbuf_->at_capacity(); }

  void push_back(pw::UniquePtr<std::byte[]>&& bytes) {
    PW_ASSERT(!full());
    mbuf_->PushBack(std::move(bytes));
  }

  pw::UniquePtr<const std::byte[]> pop_front() {
    return mbuf_->Release(mbuf_->cbegin());
  }
};

TEST(RingBufferTest, CanPushAndPop) {
  pw::allocator::test::AllocatorForTest<512> allocator;
  constexpr std::array<const char*, 3> kWords = {"foo", "bar", "baz"};
  constexpr size_t kNumMsgs = kWords.size() * 5;

  AsyncMultiBufQueue queue(allocator, 2);
  EXPECT_TRUE(queue.empty());

  // DOCSTAG: [pw_multibuf-examples-async_queue-producer]
  size_t producer_index = 0;
  pw::async2::FuncTask producer(
      [&](pw::async2::Context& context) -> pw::async2::Poll<> {
        while (producer_index < kNumMsgs) {
          PW_TRY_READY(queue.PendNotFull(context));
          auto s = allocator.MakeUnique<std::byte[]>(4);
          const char* word = kWords[producer_index % kWords.size()];
          std::strncpy(reinterpret_cast<char*>(s.get()), word, s.size());
          queue.push_back(std::move(s));
          ++producer_index;
        }
        return pw::async2::Ready();
      });
  // DOCSTAG: [pw_multibuf-examples-async_queue-producer]

  // DOCSTAG: [pw_multibuf-examples-async_queue-consumer]
  size_t consumer_index = 0;
  pw::async2::FuncTask consumer(
      [&](pw::async2::Context& context) -> pw::async2::Poll<> {
        while (consumer_index < kNumMsgs) {
          PW_TRY_READY(queue.PendNotEmpty(context));
          auto s = queue.pop_front();
          const char* word = kWords[consumer_index % kWords.size()];
          EXPECT_STREQ(reinterpret_cast<const char*>(s.get()), word);
          ++consumer_index;
        }
        return pw::async2::Ready();
      });
  // DOCSTAG: [pw_multibuf-examples-async_queue-consumer]

  pw::async2::BasicDispatcher dispatcher;
  dispatcher.Post(producer);
  dispatcher.Post(consumer);
  dispatcher.RunToCompletion();
  EXPECT_EQ(producer_index, kNumMsgs);
  EXPECT_EQ(consumer_index, kNumMsgs);
}

}  // namespace examples
