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
#include "pw_assert/check.h"
#include "pw_async2/basic_dispatcher.h"
#include "pw_async2/context.h"
#include "pw_async2/func_task.h"
#include "pw_async2/poll.h"
#include "pw_async2/try.h"
#include "pw_async2/waker.h"
#include "pw_bytes/endian.h"
#include "pw_bytes/span.h"
#include "pw_containers/inline_async_queue.h"
#include "pw_multibuf/examples/protocol.h"
#include "pw_multibuf/multibuf_v2.h"
#include "pw_random/xor_shift.h"
#include "pw_unit_test/framework.h"

namespace pw::multibuf::examples {

using async2::Context;
using async2::Pending;
using async2::Poll;
using async2::Ready;
using async2::Task;
using async2::Waker;

constexpr size_t kCapacity = 4;

template <typename T>
constexpr T GetHeaderField(const ConstMultiBuf& mbuf, size_t offset) {
  ConstByteSpan header = *(mbuf.ConstChunks().cbegin());
  return bytes::ReadInOrder<T>(endian::little, &header[offset]);
}

template <typename T>
constexpr void SetHeaderField(MultiBuf& mbuf, size_t offset, T value) {
  ByteSpan header = *(mbuf.Chunks().begin());
  return bytes::CopyInOrder<T>(endian::little, value, &header[offset]);
}

class TransportSegment;
class NetworkPacket;

class LinkFrame {
 public:
  static LinkFrame From(NetworkPacket&& packet);

  constexpr uint16_t length() const {
    return GetHeaderField<uint16_t>(*mbuf_, offsetof(DemoLinkHeader, length));
  }

 private:
  friend class NetworkPacket;

  constexpr explicit LinkFrame(MultiBuf&& mbuf) : mbuf_(std::move(mbuf)) {
    SetHeaderField<uint16_t>(*mbuf_,
                             offsetof(DemoLinkHeader, length),
                             static_cast<uint16_t>(mbuf_->size()));
  }

  // DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-link_frame-create]
  static Result<LinkFrame> Create(Allocator& allocator) {
    MultiBuf::Instance mbuf(allocator);
    if (!mbuf->TryReserveLayers(4)) {
      return Status::ResourceExhausted();
    }
    auto buffer = allocator.MakeUnique<std::byte[]>(kMaxDemoLinkFrameLength);
    if (buffer == nullptr) {
      return Status::ResourceExhausted();
    }
    mbuf->PushBack(std::move(buffer));
    PW_CHECK(mbuf->AddLayer(0));
    return LinkFrame(std::move(*mbuf));
  }
  // DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-link_frame-create]

  MultiBuf::Instance mbuf_;
};

class NetworkPacket {
 public:
  static NetworkPacket From(LinkFrame&& frame);
  static NetworkPacket From(TransportSegment&& segment);

  constexpr uint32_t length() const {
    return GetHeaderField<uint32_t>(*mbuf_,
                                    offsetof(DemoNetworkHeader, length));
  }

 private:
  friend class LinkFrame;
  friend class TransportSegment;

  constexpr explicit NetworkPacket(MultiBuf&& mbuf) : mbuf_(std::move(mbuf)) {}

  // DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-network_packet-create]
  static Result<NetworkPacket> Create(Allocator& allocator) {
    auto frame = LinkFrame::Create(allocator);
    if (!frame.ok()) {
      return frame.status();
    }
    return NetworkPacket::From(std::move(*frame));
  }
  // DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-network_packet-create]

  MultiBuf::Instance mbuf_;
};

class TransportSegment {
 public:
  // DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-transport_segment-create]
  static Result<TransportSegment> Create(Allocator& allocator, uint64_t id) {
    auto packet = NetworkPacket::Create(allocator);
    if (!packet.ok()) {
      return packet.status();
    }
    TransportSegment segment = TransportSegment::From(std::move(*packet));
    SetHeaderField<uint64_t>(
        *segment.mbuf_, offsetof(DemoTransportHeader, segment_id), id);
    return segment;
  }
  // DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-transport_segment-create]

  static TransportSegment From(NetworkPacket&& packet);

  constexpr uint64_t id() const {
    return GetHeaderField<uint64_t>(*mbuf_,
                                    offsetof(DemoTransportHeader, segment_id));
  }

  constexpr uint32_t length() const {
    return GetHeaderField<uint32_t>(*mbuf_,
                                    offsetof(DemoTransportHeader, length));
  }

  constexpr ByteSpan payload() {
    return mbuf_->Chunks().begin()->subspan(kDemoTransportHeaderLen);
  }

  // DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-transport_segment-payload]
  void CopyFrom(const char* msg, size_t msg_len) {
    uint32_t length = kDemoTransportHeaderLen;
    size_t copied =
        mbuf_->CopyFrom(as_bytes(span<const char>(msg, msg_len)), length);
    PW_CHECK_UINT_EQ(copied, msg_len);
    length += static_cast<uint32_t>(msg_len);
    mbuf_->TruncateTopLayer(length);
    return SetHeaderField<uint32_t>(
        *mbuf_, offsetof(DemoTransportHeader, length), length);
  }

  const char* AsCString() const {
    ConstByteSpan bytes = *(mbuf_->ConstChunks().cbegin());
    bytes = bytes.subspan(kDemoTransportHeaderLen);
    return reinterpret_cast<const char*>(bytes.data());
  }
  // DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-transport_segment-payload]

 private:
  friend class NetworkPacket;

  constexpr explicit TransportSegment(MultiBuf&& mbuf)
      : mbuf_(std::move(mbuf)) {}

  MultiBuf::Instance mbuf_;
};

// DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-from]
LinkFrame LinkFrame::From(NetworkPacket&& packet) {
  size_t length = packet.length() + kDemoLinkHeaderLen + kDemoLinkFooterLen;
  PW_CHECK_UINT_LE(length, std::numeric_limits<uint16_t>::max());
  LinkFrame frame(std::move(*packet.mbuf_));
  frame.mbuf_->PopLayer();
  frame.mbuf_->TruncateTopLayer(length);
  SetHeaderField<uint16_t>(*frame.mbuf_,
                           offsetof(DemoLinkHeader, length),
                           static_cast<uint16_t>(length));
  return frame;
}

NetworkPacket NetworkPacket::From(LinkFrame&& frame) {
  size_t length = frame.length() - (kDemoLinkHeaderLen + kDemoLinkFooterLen);
  NetworkPacket packet(std::move(*frame.mbuf_));
  PW_CHECK(packet.mbuf_->AddLayer(kDemoLinkHeaderLen, length));
  SetHeaderField<uint32_t>(*packet.mbuf_,
                           offsetof(DemoNetworkHeader, length),
                           static_cast<uint32_t>(length));
  return packet;
}

NetworkPacket NetworkPacket::From(TransportSegment&& segment) {
  size_t length = segment.length() + kDemoNetworkHeaderLen;
  PW_CHECK_UINT_LE(length, std::numeric_limits<uint32_t>::max());
  NetworkPacket packet(std::move(*segment.mbuf_));
  packet.mbuf_->PopLayer();
  packet.mbuf_->TruncateTopLayer(length);
  SetHeaderField<uint32_t>(*packet.mbuf_,
                           offsetof(DemoNetworkHeader, length),
                           static_cast<uint32_t>(length));
  return packet;
}

TransportSegment TransportSegment::From(NetworkPacket&& packet) {
  size_t length = packet.length() - kDemoNetworkHeaderLen;
  TransportSegment segment(std::move(*packet.mbuf_));
  PW_CHECK(segment.mbuf_->AddLayer(kDemoNetworkHeaderLen, length));
  SetHeaderField<uint32_t>(*segment.mbuf_,
                           offsetof(DemoTransportHeader, length),
                           static_cast<uint32_t>(length));
  return segment;
}
// DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-from]

/// Wrapper that allows signalling when closed.
template <typename T>
class Closeable {
 public:
  constexpr explicit Closeable(InlineAsyncQueue<T>& queue) : queue_(queue) {}

  Poll<> PendHasSpace(Context& context) { return queue_.PendHasSpace(context); }

  void push(T&& t) { queue_.push(std::move(t)); }

  T& front() { return queue_.front(); }

  void pop() { queue_.pop(); }

  Poll<Status> PendNotEmpty(Context& context) {
    auto poll_result = queue_.PendNotEmpty(context);
    if (poll_result.IsReady()) {
      return Ready(OkStatus());
    }
    if (closed_) {
      return Ready(Status::ResourceExhausted());
    }
    PW_ASYNC_STORE_WAKER(context, waker_, "waiting for data or close");
    return Pending();
  }

  void Close() {
    closed_ = true;
    waker_.Wake();
  }

 private:
  InlineAsyncQueue<T>& queue_;
  bool closed_ = false;
  Waker waker_;
};

// DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-relay]
template <typename FromProtocol, typename ToProtocol>
class Relay : public Task {
 public:
  Relay(pw::log::Token name,
        Closeable<FromProtocol>& rx,
        Closeable<ToProtocol>& tx)
      : Task(name), rx_(rx), tx_(tx) {}

 private:
  Poll<> DoPend(Context& context) override {
    while (true) {
      if (pending_.has_value()) {
        PW_TRY_READY(tx_.PendHasSpace(context));
        tx_.push(std::move(*pending_));
        pending_.reset();
      }
      PW_TRY_READY_ASSIGN(auto status, rx_.PendNotEmpty(context));
      if (!status.ok()) {
        tx_.Close();
        return Ready();
      }
      FromProtocol from(std::move(rx_.front()));
      rx_.pop();
      if constexpr (std::is_same_v<FromProtocol, ToProtocol>) {
        pending_ = std::move(from);
      } else {
        pending_ = ToProtocol::From(std::move(from));
      }
    }
  }

  std::optional<ToProtocol> pending_;
  Closeable<FromProtocol>& rx_;
  Closeable<ToProtocol>& tx_;
};
// DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-relay]

// DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-sender]
template <typename NestedProtocol, typename OuterProtocol>
class Sender : public Relay<NestedProtocol, OuterProtocol> {  //...
  // DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-sender]
 public:
  Sender(pw::log::Token name, Closeable<NestedProtocol>& rx)
      : Relay<NestedProtocol, OuterProtocol>(name, rx, tx_), tx_(tx_queue_) {}

  constexpr Closeable<OuterProtocol>& queue() { return tx_; }

 private:
  InlineAsyncQueue<OuterProtocol, kCapacity> tx_queue_;
  Closeable<OuterProtocol> tx_;
};

// DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-receiver]
template <typename OuterProtocol, typename NestedProtocol>
class Receiver : public Relay<OuterProtocol, NestedProtocol> {  //...
  // DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-receiver]
 public:
  Receiver(pw::log::Token name, Closeable<NestedProtocol>& tx)
      : Relay<OuterProtocol, NestedProtocol>(name, rx_, tx), rx_(rx_queue_) {}

  constexpr Closeable<OuterProtocol>& queue() { return rx_; }

 private:
  InlineAsyncQueue<OuterProtocol, kCapacity> rx_queue_;
  Closeable<OuterProtocol> rx_;
};

class Link : public Relay<LinkFrame, LinkFrame> {
 public:
  Link(Closeable<LinkFrame>& rx, Closeable<LinkFrame>& tx)
      : Relay<LinkFrame, LinkFrame>(PW_ASYNC_TASK_NAME("link"), rx, tx) {}
};

// DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-encryptor]
class Encryptor : public Task {
 public:
  constexpr explicit Encryptor(pw::log::Token name, uint64_t key)
      : Task(name), key_(key), rx_(rx_queue_), tx_(tx_queue_) {}

  Closeable<TransportSegment>& rx() { return rx_; }
  Closeable<TransportSegment>& tx() { return tx_; }

 private:
  Poll<> DoPend(Context& context) override {
    std::array<std::byte, sizeof(uint64_t)> pad;
    while (true) {
      if (segment_.has_value()) {
        PW_TRY_READY(tx_.PendHasSpace(context));
        tx_.push(std::move(*segment_));
        segment_.reset();
      }

      PW_TRY_READY_ASSIGN(auto status, rx_.PendNotEmpty(context));
      if (!status.ok()) {
        tx_.Close();
        return Ready();
      }
      segment_ = std::move(rx_.front());
      rx_.pop();

      // "Encrypt" the message. "Encrypting" again with the same key is
      // equivalent to decrypting.
      random::XorShiftStarRng64 rng(key_ ^ segment_->id());
      ByteSpan payload = segment_->payload();
      for (size_t i = 0; i < payload.size(); ++i) {
        if ((i % pad.size()) == 0) {
          rng.Get(pad);
        }
        payload[i] ^= pad[i % pad.size()];
      }
    }
  }

  const uint64_t key_;
  std::optional<TransportSegment> segment_;

  InlineAsyncQueue<TransportSegment, kCapacity> rx_queue_;
  Closeable<TransportSegment> rx_;

  InlineAsyncQueue<TransportSegment, kCapacity> tx_queue_;
  Closeable<TransportSegment> tx_;
};
// DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-encryptor]

// Excerpt from the public domain poem by Vachel Lindsay.
const char* kTheAmaranth[] = {
    "Ah, in the night, all music haunts me here....",
    "Is it for naught high Heaven cracks and yawns",
    "And the tremendous Amaranth descends",
    "Sweet with the glory of ten thousand dawns?",

    "Does it not mean my God would have me say: -",
    "'Whether you will or no, O city young,",
    "Heaven will bloom like one great flower for you,",
    "Flash and loom greatly all your marts among?'",

    "Friends, I will not cease hoping though you weep.",
    "Such things I see, and some of them shall come",
    "Though now our streets are harsh and ashen-gray,",
    "Though our strong youths are strident now, or dumb.",
    "Friends, that sweet torn, that wonder-town, shall rise.",
    "Naught can delay it. Though it may not be",
    "Just as I dream, it comes at last I know",
    "With streets like channels of an incense-sea.",
};
constexpr size_t kNumLines = sizeof(kTheAmaranth) / sizeof(kTheAmaranth[0]);

/// A simple allocator wrapper that facilitates asynchronous allocations.
class SimpleAsyncAllocator : public Allocator {
 public:
  static constexpr size_t kAllocatorCapacity = 4096;

  Poll<> PendCanAllocate(async2::Context& context, size_t num_bytes) {
    size_t available = kAllocatorCapacity - allocator_.GetAllocated();
    if (num_bytes <= available) {
      return Ready();
    }
    PW_ASYNC_STORE_WAKER(context, waker_, "waiting for memory");
    return Pending();
  }

 private:
  void* DoAllocate(Layout layout) override {
    return allocator_.Allocate(layout);
  }

  void DoDeallocate(void* ptr) override {
    allocator_.Deallocate(ptr);
    waker_.Wake();
  }

  allocator::test::AllocatorForTest<kAllocatorCapacity> allocator_;
  async2::Waker waker_;
};

TEST(PseudoEncrypt, RoundTrip) {
  SimpleAsyncAllocator allocator;
  async2::BasicDispatcher dispatcher;
  constexpr uint64_t kKey = 0xDEADBEEFFEEDFACEull;

  // DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-e2e]
  // Instantiate the sending tasks.
  Encryptor encryptor(PW_ASYNC_TASK_NAME("encryptor"), kKey);
  Sender<TransportSegment, NetworkPacket> net_sender(
      PW_ASYNC_TASK_NAME("net_sender"), encryptor.tx());
  Sender<NetworkPacket, LinkFrame> link_sender(
      PW_ASYNC_TASK_NAME("link_sender"), net_sender.queue());

  // Instantiate the receiving tasks.
  Encryptor decryptor(PW_ASYNC_TASK_NAME("decryptor"), kKey);
  Receiver<NetworkPacket, TransportSegment> net_receiver(
      PW_ASYNC_TASK_NAME("net_receiver"), decryptor.rx());
  Receiver<LinkFrame, NetworkPacket> link_receiver(
      PW_ASYNC_TASK_NAME("link_receiver"), net_receiver.queue());

  // Connect both ends.
  Link link(link_sender.queue(), link_receiver.queue());

  // Define a task that sends messages.
  size_t tx_index = 0;
  uint64_t segment_id = 0x1000;
  async2::PendFuncTask msg_sender(
      [&](async2::Context& context) -> async2::Poll<> {
        auto& queue = encryptor.rx();
        while (tx_index < kNumLines) {
          PW_TRY_READY(
              allocator.PendCanAllocate(context, kMaxDemoLinkFrameLength));
          PW_TRY_READY(queue.PendHasSpace(context));
          auto segment = TransportSegment::Create(allocator, segment_id++);
          PW_CHECK_OK(segment.status());
          const char* line = kTheAmaranth[tx_index];
          segment->CopyFrom(line, strlen(line) + 1);
          queue.push(std::move(*segment));
          ++tx_index;
        }
        queue.Close();
        return Ready();
      });

  // Define a task that receives messages.
  size_t rx_index = 0;
  async2::PendFuncTask msg_receiver(
      [&](async2::Context& context) -> async2::Poll<> {
        auto& queue = decryptor.tx();
        while (true) {
          PW_TRY_READY_ASSIGN(auto status, queue.PendNotEmpty(context));
          if (!status.ok()) {
            return Ready();
          }
          TransportSegment segment(std::move(queue.front()));
          queue.pop();
          EXPECT_STREQ(segment.AsCString(), kTheAmaranth[rx_index]);
          ++rx_index;
        }
      });

  // Run all tasks on the dispatcher.
  dispatcher.Post(msg_sender);
  dispatcher.Post(encryptor);
  dispatcher.Post(net_sender);
  dispatcher.Post(link_sender);
  dispatcher.Post(link);
  dispatcher.Post(link_receiver);
  dispatcher.Post(net_receiver);
  dispatcher.Post(decryptor);
  dispatcher.Post(msg_receiver);
  // DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-e2e]

  dispatcher.RunToCompletion();
  EXPECT_EQ(tx_index, kNumLines);
  EXPECT_EQ(rx_index, kNumLines);
}

}  // namespace pw::multibuf::examples
