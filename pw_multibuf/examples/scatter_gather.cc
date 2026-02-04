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

#include "pw_allocator/allocator.h"
#include "pw_allocator/shared_ptr.h"
#include "pw_allocator/testing.h"
#include "pw_allocator/unique_ptr.h"
#include "pw_bytes/span.h"
#include "pw_chrono/system_clock.h"
#include "pw_containers/dynamic_vector.h"
#include "pw_i2c/address.h"
#include "pw_i2c/message.h"
#include "pw_multibuf/multibuf.h"
#include "pw_sync/timed_thread_notification.h"
#include "pw_thread/test_thread_context.h"
#include "pw_thread/thread.h"

namespace pw::multibuf::examples {

using chrono::SystemClock;
using i2c::Address;
using i2c::Message;

// DOCSTAG: [pw_multibuf-examples-scatter_gather-message_vector]
class MessageVector {
 public:
  explicit MessageVector(Allocator& allocator)
      : messages_(allocator), rx_buffers_(allocator), tx_buffers_(allocator) {}

  void AddRead(Address addr, UniquePtr<std::byte[]> dst) {
    messages_.push_back(Message::ReadMessage(addr, {dst.get(), dst.size()}));
    rx_buffers_->PushBack(std::move(dst));
  }

  void AddRead(Address addr, SharedPtr<std::byte[]> dst) {
    messages_.push_back(Message::ReadMessage(addr, {dst.get(), dst.size()}));
    rx_buffers_->PushBack(dst);
  }

  void AddWrite(Address addr, UniquePtr<const std::byte[]>&& src) {
    messages_.push_back(Message::WriteMessage(addr, {src.get(), src.size()}));
    tx_buffers_->PushBack(std::move(src));
  }

  void AddWrite(Address addr, const SharedPtr<const std::byte[]>& src) {
    messages_.push_back(Message::WriteMessage(addr, {src.get(), src.size()}));
    tx_buffers_->PushBack(src);
  }

 private:
  friend class TestInitiator;

  DynamicVector<Message> messages_;
  TrackedMultiBuf::Instance rx_buffers_;
  TrackedConstMultiBuf::Instance tx_buffers_;
};
// DOCSTAG: [pw_multibuf-examples-scatter_gather-message_vector]

// DOCSTAG: [pw_multibuf-examples-scatter_gather-observer]
class MessageVectorObserver : public multibuf::Observer {
 public:
  void AddBytes(size_t num_bytes) { num_bytes_ += num_bytes; }

  Status Await(SystemClock::duration timeout) {
    return notification_.try_acquire_for(timeout) ? OkStatus()
                                                  : Status::DeadlineExceeded();
  }

 private:
  void DoNotify(Event event, size_t value) override {
    if (event == multibuf::Observer::Event::kBytesAdded) {
      num_bytes_ += value;
    } else if (event == multibuf::Observer::Event::kBytesRemoved) {
      num_bytes_ -= value;
    }
    if (num_bytes_ == 0) {
      notification_.release();
    }
  }

  sync::TimedThreadNotification notification_;
  size_t num_bytes_ = 0;
};
// DOCSTAG: [pw_multibuf-examples-scatter_gather-observer]

// DOCSTAG: [pw_multibuf-examples-scatter_gather-initiator]
class TestInitiator {
 public:
  explicit TestInitiator(Allocator& allocator) : msg_vec_(allocator) {}

  constexpr Status status() const { return status_; }

  void StageForTransfer(MessageVector&& msg_vec) {
    msg_vec_ = std::move(msg_vec);
    observer_.AddBytes(msg_vec_.rx_buffers_->size());
    observer_.AddBytes(msg_vec_.tx_buffers_->size());
    msg_vec_.rx_buffers_->set_observer(&observer_);
    msg_vec_.tx_buffers_->set_observer(&observer_);
  }

  void TransferFor(SystemClock::duration timeout) {
    // The actual I2C transfer would be performed here...
    status_ = observer_.Await(timeout);
  }

  void Complete() {
    msg_vec_.rx_buffers_->Clear();
    msg_vec_.tx_buffers_->Clear();
  }

 private:
  MessageVector msg_vec_;
  MessageVectorObserver observer_;
  Status status_ = OkStatus();
};
// DOCSTAG: [pw_multibuf-examples-scatter_gather-initiator]

// TODO: https://pwbug.dev/365161669 - Express joinability as a build-system
// constraint.
#if PW_THREAD_JOINING_ENABLED

TEST(ScatterGatherTest, NotifiedWhenDropped) {
  allocator::test::AllocatorForTest<512> allocator;
  MessageVector msg_vec(allocator);

  auto rx_owned = allocator.MakeUnique<std::byte[]>(16);
  msg_vec.AddRead(Address::TenBit(0x10), std::move(rx_owned));

  auto tx_shared = allocator.MakeShared<std::byte[]>(16);
  msg_vec.AddWrite(Address::SevenBit(0x77), tx_shared);

  TestInitiator initiator(allocator);
  initiator.StageForTransfer(std::move(msg_vec));

  thread::test::TestThreadContext context;
  Thread thread(context.options(), [&initiator]() {
    initiator.TransferFor(
        SystemClock::for_at_least(std::chrono::milliseconds(42)));
  });

  initiator.Complete();
  thread.join();
  EXPECT_EQ(initiator.status(), OkStatus());
}

#endif  // PW_THREAD_JOINING_ENABLED

}  // namespace pw::multibuf::examples
