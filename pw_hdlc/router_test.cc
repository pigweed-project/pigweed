// Copyright 2024 The Pigweed Authors
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

#include "pw_hdlc/router.h"

#include "pw_allocator/testing.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/func_task.h"
#include "pw_bytes/suffix.h"
#include "pw_channel/forwarding_channel.h"
#include "pw_channel/loopback_channel.h"
#include "pw_containers/inline_queue.h"
#include "pw_containers/vector.h"
#include "pw_multibuf/simple_allocator.h"

namespace pw::hdlc {
namespace {

using ::pw::allocator::test::AllocatorForTest;
using ::pw::async2::Context;
using ::pw::async2::DispatcherForTest;

using ::pw::async2::FuncTask;
using ::pw::async2::Pending;
using ::pw::async2::Poll;
using ::pw::async2::PollResult;
using ::pw::async2::Ready;
using ::pw::async2::Task;
using ::pw::async2::Waker;
using ::pw::operator""_b;
using ::pw::channel::DatagramReader;
using ::pw::channel::DatagramWriter;
using ::pw::channel::ForwardingByteChannelPair;
using ::pw::channel::ForwardingDatagramChannelPair;
using ::pw::channel::LoopbackByteChannel;
using ::pw::multibuf::MultiBuf;
using ::pw::multibuf::MultiBufAllocator;
using ::pw::multibuf::SimpleAllocator;

class SimpleAllocatorForTest {
 public:
  SimpleAllocatorForTest() : simple_allocator_(data_area_, meta_alloc_) {}
  MultiBufAllocator& operator*() { return simple_allocator_; }
  MultiBufAllocator* operator->() { return &simple_allocator_; }

 private:
  static constexpr size_t kArbitraryDataSize = 256;
  static constexpr size_t kArbitraryMetaSize = 2048;
  std::array<std::byte, kArbitraryDataSize> data_area_;
  AllocatorForTest<kArbitraryMetaSize> meta_alloc_;
  SimpleAllocator simple_allocator_;
};

class SendDatagrams : public Task {
 public:
  SendDatagrams(pw::InlineQueue<MultiBuf>& to_send, DatagramWriter& channel)
      : to_send_(to_send), channel_(channel) {}

 private:
  Poll<> DoPend(Context& cx) final {
    while (!to_send_.empty()) {
      if (channel_.PendReadyToWrite(cx).IsPending()) {
        return Pending();
      }
      PW_TEST_EXPECT_OK(channel_.StageWrite(std::move(to_send_.front())));
      to_send_.pop();
    }
    return Ready();
  }

  pw::InlineQueue<MultiBuf>& to_send_;
  DatagramWriter& channel_;
};

static constexpr size_t kMaxReceiveDatagrams = 16;
class ReceiveDatagramsUntilClosed : public Task {
 public:
  ReceiveDatagramsUntilClosed(DatagramReader& channel) : channel_(channel) {}

  pw::Vector<MultiBuf, kMaxReceiveDatagrams> received;

 private:
  Poll<> DoPend(Context& cx) final {
    while (true) {
      PollResult<MultiBuf> result = channel_.PendRead(cx);
      if (result.IsPending()) {
        return Pending();
      }
      if (!result->ok()) {
        EXPECT_EQ(result->status(), pw::Status::FailedPrecondition());
        return Ready();
      }
      received.push_back(std::move(**result));
    }
    // Unreachable.
    return Ready();
  }

  DatagramReader& channel_;
};

template <typename ActualIterable, typename ExpectedIterable>
void ExpectElementsEqual(const ActualIterable& actual,
                         const ExpectedIterable& expected) {
  auto actual_iter = actual.begin();
  auto expected_iter = expected.begin();
  for (; expected_iter != expected.end(); ++actual_iter, ++expected_iter) {
    ASSERT_NE(actual_iter, actual.end());
    EXPECT_EQ(*actual_iter, *expected_iter);
  }
}

template <typename ActualIterable, typename T>
void ExpectElementsEqual(const ActualIterable& actual,
                         std::initializer_list<T> expected) {
  ExpectElementsEqual<ActualIterable, std::initializer_list<T>>(actual,
                                                                expected);
}

// TODO: b/331285977 - Fuzz test this function.
void ExpectSendAndReceive(
    std::initializer_list<std::initializer_list<std::byte>> data) {
  SimpleAllocatorForTest alloc;

  LoopbackByteChannel io_loopback(*alloc);
  ForwardingDatagramChannelPair outgoing_pair(*alloc, *alloc);
  ForwardingDatagramChannelPair incoming_pair(*alloc, *alloc);

  static constexpr size_t kMaxSendDatagrams = 16;
  ASSERT_LE(data.size(), kMaxSendDatagrams);

  pw::InlineQueue<MultiBuf, kMaxSendDatagrams> datagrams_to_send;
  for (size_t i = 0; i < data.size(); i++) {
    std::optional<MultiBuf> buf = alloc->Allocate(std::data(data)[i].size());
    ASSERT_TRUE(buf.has_value());
    std::copy(
        std::data(data)[i].begin(), std::data(data)[i].end(), buf->begin());
    datagrams_to_send.push(std::move(*buf));
  }

  static constexpr uint64_t kAddress = 27;
  static constexpr uint64_t kArbitraryAddressOne = 13802183;
  static constexpr uint64_t kArbitraryAddressTwo = 4284900;
  static constexpr size_t kDecodeBufferSize = 256;

  std::array<std::byte, kDecodeBufferSize> decode_buffer;
  Router router(io_loopback.channel(), decode_buffer);
  FuncTask router_task([&router](Context& cx) { return router.Pend(cx); });

  SendDatagrams send_task(datagrams_to_send, outgoing_pair.first());
  ReceiveDatagramsUntilClosed recv_task(incoming_pair.first());

  EXPECT_EQ(
      router.AddChannel(outgoing_pair.second(), kArbitraryAddressOne, kAddress),
      OkStatus());
  EXPECT_EQ(
      router.AddChannel(incoming_pair.second(), kAddress, kArbitraryAddressTwo),
      OkStatus());

  DispatcherForTest dispatcher;
  dispatcher.Post(router_task);
  dispatcher.Post(send_task);
  dispatcher.Post(recv_task);

  EXPECT_TRUE(dispatcher.RunUntilStalled());
  ASSERT_EQ(recv_task.received.size(), data.size());
  for (size_t i = 0; i < data.size(); i++) {
    ExpectElementsEqual(recv_task.received[i], std::data(data)[i]);
  }
}

TEST(Router, SendsAndReceivesSingleDatagram) {
  ExpectSendAndReceive({{2_b, 4_b, 6_b, 0_b, 1_b}});
}

TEST(Router, SendsAndReceivesMultipleDatagrams) {
  ExpectSendAndReceive({
      {1_b, 3_b, 5_b},
      {2_b, 4_b, 6_b, 7_b},
  });
}

TEST(Router, SendsAndReceivesReservedBytes) {
  ExpectSendAndReceive({
      // Control octets.
      {0x7D_b},
      {0x7E_b},
      {0x7D_b, 0x7E_b},
      {0x7D_b, 0x5E_b},
      // XON / XOFF
      {0x13_b},
      {0x11_b},
  });
}

TEST(Router, PendOnClosedIoChannelReturnsReady) {
  static constexpr size_t kDecodeBufferSize = 256;

  SimpleAllocatorForTest alloc;

  ForwardingByteChannelPair byte_pair(*alloc, *alloc);
  std::array<std::byte, kDecodeBufferSize> decode_buffer;
  Router router(byte_pair.first(), decode_buffer);

  ForwardingDatagramChannelPair datagram_pair(*alloc, *alloc);
  ReceiveDatagramsUntilClosed recv_task(datagram_pair.first());
  EXPECT_EQ(router.AddChannel(datagram_pair.second(),
                              /*arbitrary incoming address*/ 5017,
                              /*arbitrary outgoing address*/ 2019),
            OkStatus());

  FuncTask router_task([&router](Context& cx) { return router.Pend(cx); });

  DispatcherForTest dispatcher;
  dispatcher.Post(router_task);
  dispatcher.Post(recv_task);

  EXPECT_TRUE(dispatcher.RunUntilStalled());

  // Close the underlying byte channel.
  Waker null_waker;
  Context null_cx(dispatcher, null_waker);
  EXPECT_EQ(byte_pair.second().PendClose(null_cx), Ready(OkStatus()));

  // Both the router and the receive task should complete.
  dispatcher.RunToCompletion();
}

}  // namespace
}  // namespace pw::hdlc
