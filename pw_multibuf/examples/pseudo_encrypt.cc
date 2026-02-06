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
#include "pw_async2/channel.h"
#include "pw_async2/context.h"
#include "pw_async2/func_task.h"
#include "pw_async2/poll.h"
#include "pw_async2/try.h"
#include "pw_async2/waker.h"
#include "pw_bytes/endian.h"
#include "pw_bytes/span.h"
#include "pw_multibuf/examples/protocol.h"
#include "pw_multibuf/multibuf.h"
#include "pw_random/xor_shift.h"
#include "pw_span/cast.h"
#include "pw_unit_test/framework.h"

namespace examples {

struct DemoLinkHeader : pw::multibuf::examples::DemoLinkHeader {
  static constexpr size_t kLength = pw::multibuf::examples::kDemoLinkHeaderLen;
};

struct DemoLinkFooter : pw::multibuf::examples::DemoLinkFooter {
  static constexpr size_t kLength = pw::multibuf::examples::kDemoLinkFooterLen;
};

struct DemoNetworkHeader : pw::multibuf::examples::DemoNetworkHeader {
  static constexpr size_t kLength =
      pw::multibuf::examples::kDemoNetworkHeaderLen;
};

struct DemoTransportHeader : pw::multibuf::examples::DemoTransportHeader {
  static constexpr size_t kLength =
      pw::multibuf::examples::kDemoTransportHeaderLen;
};

constexpr size_t kMaxDemoLinkFrameLength =
    pw::multibuf::examples::kMaxDemoLinkFrameLength;

// The maximum number of active link frame buffers.
constexpr size_t kCapacity = 4;

/// Gets a protocol field from a protocol header stored in a MultiBuf.
template <typename T>
constexpr T GetHeaderField(const pw::ConstMultiBuf& mbuf, size_t offset) {
  pw::ConstByteSpan header = *(mbuf.ConstChunks().cbegin());
  return pw::bytes::ReadInOrder<T>(pw::endian::little, &header[offset]);
}

/// Sets a protocol field for a protocol header stored in a MultiBuf.
template <typename T>
constexpr void SetHeaderField(pw::MultiBuf& mbuf, size_t offset, T value) {
  pw::ByteSpan header = *(mbuf.Chunks().begin());
  return pw::bytes::CopyInOrder<T>(pw::endian::little, value, &header[offset]);
}

// Forward decalrations.
class TransportSegment;
class NetworkPacket;

class LinkFrame {
 public:
  /// Creates a new link frame backed by the given `buffer`, and using the given
  /// `allocator` to allocate MultiBuf metadata.
  static pw::Result<LinkFrame> Create(pw::Allocator& allocator,
                                      pw::ByteSpan buffer) {
    pw::MultiBuf::Instance mbuf(allocator);
    if (!mbuf->TryReserveLayers(4)) {
      return pw::Status::ResourceExhausted();
    }
    mbuf->PushBack(buffer);
    PW_CHECK(mbuf->AddLayer(0));
    return LinkFrame(std::move(*mbuf));
  }

  /// Convert a network packet to its underlying link frame.
  static LinkFrame From(NetworkPacket&& packet);

  constexpr uint16_t length() const {
    return GetHeaderField<uint16_t>(*mbuf_, offsetof(DemoLinkHeader, length));
  }

  /// Erases all data and sets the frame length back to the size of the buffer
  /// that was provided in the constructor.
  void Reset() {
    mbuf_->PopLayer();
    for (auto chunk : mbuf_->Chunks()) {
      std::memset(chunk.data(), 0, chunk.size());
    }
    PW_CHECK(mbuf_->AddLayer(0));
    ResetLength();
  }

 private:
  friend class NetworkPacket;

  constexpr explicit LinkFrame(pw::MultiBuf&& mbuf) : mbuf_(std::move(mbuf)) {
    PW_CHECK_UINT_EQ(mbuf_->NumLayers(), 2u);
    ResetLength();
  }

  /// Updates the link frame header to match the MultiBuf.
  void ResetLength() {
    size_t length = mbuf_->size();
    PW_CHECK_UINT_LE(length, std::numeric_limits<uint16_t>::max());
    SetHeaderField<uint16_t>(*mbuf_,
                             offsetof(DemoLinkHeader, length),
                             static_cast<uint16_t>(mbuf_->size()));
  }

  pw::MultiBuf::Instance mbuf_;
};

class NetworkPacket {
 public:
  /// Convert a network packet to the link frame it holds.
  static NetworkPacket From(LinkFrame&& frame);

  /// Convert a transport segment to its underlying network packet.
  static NetworkPacket From(TransportSegment&& segment);

  constexpr uint32_t length() const {
    return GetHeaderField<uint32_t>(*mbuf_,
                                    offsetof(DemoNetworkHeader, length));
  }

 private:
  friend class LinkFrame;
  friend class TransportSegment;

  constexpr explicit NetworkPacket(pw::MultiBuf&& mbuf)
      : mbuf_(std::move(mbuf)) {
    PW_CHECK_UINT_EQ(mbuf_->NumLayers(), 3u);
    size_t length = mbuf_->size();
    PW_CHECK_UINT_LE(length, std::numeric_limits<uint32_t>::max());
    SetHeaderField<uint32_t>(*mbuf_,
                             offsetof(DemoNetworkHeader, length),
                             static_cast<uint32_t>(length));
  }

  pw::MultiBuf::Instance mbuf_;
};

class TransportSegment {
 public:
  /// Convert a network packet to the transport segment it holds.
  static TransportSegment From(NetworkPacket&& packet);

  constexpr uint64_t id() const {
    return GetHeaderField<uint64_t>(*mbuf_,
                                    offsetof(DemoTransportHeader, segment_id));
  }

  constexpr uint32_t length() const {
    return GetHeaderField<uint32_t>(*mbuf_,
                                    offsetof(DemoTransportHeader, length));
  }

  constexpr pw::ByteSpan payload() {
    return mbuf_->Chunks().begin()->subspan(DemoTransportHeader::kLength);
  }

  constexpr void set_id(uint64_t id) {
    SetHeaderField<uint64_t>(
        *mbuf_, offsetof(DemoTransportHeader, segment_id), id);
  }

  // DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-transport_segment-payload]
  /// `TransportSegment::CopyFrom` writes a given message into the transport
  /// segment's payload, and then updates the segment length to match.
  void CopyFrom(const char* msg, size_t msg_len) {
    uint32_t length = DemoTransportHeader::kLength;
    size_t copied =
        mbuf_->CopyFrom(as_bytes(pw::span<const char>(msg, msg_len)), length);
    PW_CHECK_UINT_EQ(copied, msg_len);
    length += static_cast<uint32_t>(msg_len);
    mbuf_->TruncateTopLayer(length);
    return SetHeaderField<uint32_t>(
        *mbuf_, offsetof(DemoTransportHeader, length), length);
  }
  // DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-transport_segment-payload]

 private:
  friend class NetworkPacket;

  constexpr explicit TransportSegment(pw::MultiBuf&& mbuf)
      : mbuf_(std::move(mbuf)) {
    PW_CHECK_UINT_EQ(mbuf_->NumLayers(), 4u);
    size_t length = mbuf_->size();
    PW_CHECK_UINT_LE(length, std::numeric_limits<uint32_t>::max());
    SetHeaderField<uint32_t>(*mbuf_,
                             offsetof(DemoTransportHeader, length),
                             static_cast<uint32_t>(length));
  }

  pw::MultiBuf::Instance mbuf_;
};

// DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-from]
LinkFrame LinkFrame::From(NetworkPacket&& packet) {
  size_t length =
      packet.length() + DemoLinkHeader::kLength + DemoLinkFooter::kLength;
  PW_CHECK_UINT_LE(length, std::numeric_limits<uint16_t>::max());
  pw::MultiBuf::Instance mbuf = std::move(*packet.mbuf_);
  mbuf->PopLayer();
  mbuf->TruncateTopLayer(length);
  return LinkFrame(std::move(*mbuf));
}

NetworkPacket NetworkPacket::From(LinkFrame&& frame) {
  size_t length =
      frame.length() - (DemoLinkHeader::kLength + DemoLinkFooter::kLength);
  pw::MultiBuf::Instance mbuf = std::move(*frame.mbuf_);
  PW_CHECK(mbuf->AddLayer(DemoLinkHeader::kLength, length));
  return NetworkPacket(std::move(*mbuf));
}

NetworkPacket NetworkPacket::From(TransportSegment&& segment) {
  size_t length = segment.length() + DemoNetworkHeader::kLength;
  pw::MultiBuf::Instance mbuf = std::move(*segment.mbuf_);
  mbuf->PopLayer();
  mbuf->TruncateTopLayer(length);
  return NetworkPacket(std::move(*mbuf));
}

TransportSegment TransportSegment::From(NetworkPacket&& packet) {
  size_t length = packet.length() - DemoNetworkHeader::kLength;
  pw::MultiBuf::Instance mbuf = std::move(*packet.mbuf_);
  PW_CHECK(mbuf->AddLayer(DemoNetworkHeader::kLength, length));
  return TransportSegment(std::move(*mbuf));
}
// DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-from]

template <typename FromProtocol, typename ToProtocol>
class Transformer : public pw::async2::Task {
 public:
  constexpr explicit Transformer(pw::log::Token name)
      : pw::async2::Task(name) {}

  template <typename U>
  void ConnectTo(Transformer<ToProtocol, U>& next) {
    std::tie(std::ignore, tx_, next.rx_) =
        pw::async2::CreateSpscChannel<ToProtocol>(storage_);
  }

 protected:
  // Exposes the send queue to derived classes. Can be used to inject messages.
  pw::async2::Sender<ToProtocol>& tx() { return tx_; }

 private:
  template <typename, typename>
  friend class Transformer;

  // DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-transformer]
  /// `Transformer::DoPend` reads from `rx_` and sends to `tx_`.
  pw::async2::Poll<> DoPend(pw::async2::Context& context) override {
    while (true) {
      // First, send any previously transformed value.
      if (tx_future_.is_pendable()) {
        PW_TRY_READY_ASSIGN(bool sent, tx_future_.Pend(context));
        if (!sent) {
          break;
        }
      }

      // Get a future read a message from the incoming channel.
      if (!rx_future_.is_pendable()) {
        rx_future_ = rx_.Receive();
      }

      // Try to resolve the future to a value.
      PW_TRY_READY_ASSIGN(auto pending, rx_future_.Pend(context));

      //  Check if the channel has closed.
      if (!pending.has_value()) {
        break;
      }

      // Transform the message.
      auto result = Transform(std::move(*pending));
      if (!result.has_value()) {
        break;
      }

      // Create a future to send it.
      tx_future_ = tx_.Send(std::move(*result));
    }
    rx_.Disconnect();
    tx_.Disconnect();
    return pw::async2::Ready();
  }
  // DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-transformer]

  virtual std::optional<ToProtocol> Transform(FromProtocol&& msg) = 0;

  pw::async2::Receiver<FromProtocol> rx_;
  pw::async2::ReceiveFuture<FromProtocol> rx_future_;

  pw::async2::ChannelStorage<ToProtocol, kCapacity> storage_;
  pw::async2::Sender<ToProtocol> tx_;
  pw::async2::SendFuture<ToProtocol> tx_future_;
};

template <typename FromProtocol, typename ToProtocol>
class Relay : public Transformer<FromProtocol, ToProtocol> {
 public:
  constexpr explicit Relay(pw::log::Token name)
      : Transformer<FromProtocol, ToProtocol>(name) {}

 private:
  // DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-relay]
  /// `Relay::Transform` converts messages of one protocol into another.
  std::optional<ToProtocol> Transform(FromProtocol&& msg) override {
    return std::make_optional(ToProtocol::From(std::move(msg)));
  }
  // DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-relay]
};

class Encryptor : public Transformer<TransportSegment, TransportSegment> {
 public:
  constexpr Encryptor(pw::log::Token name, uint64_t key)
      : Transformer<TransportSegment, TransportSegment>(name), key_(key) {}

 private:
  // DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-encryptor]
  /// `Encryptor::Transform` "encrypts" the message. "Encrypting" again with the
  /// same key is equivalent to decrypting.
  std::optional<TransportSegment> Transform(
      TransportSegment&& segment) override {
    pw::random::XorShiftStarRng64 rng(key_ ^ segment.id());
    std::array<std::byte, sizeof(uint64_t)> pad;
    pw::ByteSpan payload = segment.payload();
    for (size_t i = 0; i < payload.size(); ++i) {
      if ((i % pad.size()) == 0) {
        rng.Get(pad);
      }
      payload[i] ^= pad[i % pad.size()];
    }
    return std::make_optional(std::move(segment));
  }
  // DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-encryptor]

  const uint64_t key_;
};

class LineTransformer : public Transformer<TransportSegment, TransportSegment> {
 public:
  constexpr LineTransformer(pw::log::Token name, pw::span<const char*>& lines)
      : Transformer<TransportSegment, TransportSegment>(name),
        lines_(lines),
        iter_(lines.begin()) {}

  [[nodiscard]] constexpr bool completed() const {
    return iter_ == lines_.end();
  }

 private:
  // DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-line_transformer]
  /// `LineTransformer::Transform` acts sequentially of lines of a message.
  std::optional<TransportSegment> Transform(
      TransportSegment&& segment) override {
    if (completed()) {
      return std::nullopt;
    }
    TransformLine(*iter_++, segment);
    return std::make_optional(std::move(segment));
  }
  // DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-line_transformer]

  virtual void TransformLine(const char* line, TransportSegment& segment) = 0;

  pw::span<const char*> lines_;
  pw::span<const char*>::iterator iter_;
};

class Producer : public LineTransformer {
 public:
  constexpr Producer(pw::log::Token name, pw::span<const char*>& lines)
      : LineTransformer(name, lines) {}

 private:
  // DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-producer]
  /// `Producer::TransformLine` writes lines into payloads to be sent.
  void TransformLine(const char* line, TransportSegment& segment) override {
    segment.CopyFrom(line, strlen(line) + 1);
  }
  // DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-producer]
};

class Consumer : public LineTransformer {
 public:
  constexpr Consumer(pw::log::Token name, pw::span<const char*>& lines)
      : LineTransformer(name, lines) {}

 private:
  // DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-consumer]
  /// `Consumer::TransformLine` verifies received lines match expected ones.
  void TransformLine(const char* line, TransportSegment& segment) override {
    auto payload = pw::span_cast<char>(segment.payload());
    EXPECT_STREQ(line, payload.data());
  }
  // DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-consumer]
};

class Recycler : public Transformer<TransportSegment, TransportSegment> {
 public:
  constexpr Recycler(pw::log::Token name)
      : Transformer<TransportSegment, TransportSegment>(name) {}

  /// Injects a new segment into the recycler.
  pw::Status AddTransportSegment(TransportSegment&& segment) {
    return tx().TrySend(Launder(std::move(segment)));
  }

 private:
  // DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-recycler]
  /// `Recycler::Transform` turns sent segments into freshly initialized ones.
  std::optional<TransportSegment> Transform(
      TransportSegment&& segment) override {
    return std::make_optional(Launder(std::move(segment)));
  }

  TransportSegment Launder(TransportSegment&& segment) {
    auto packet = NetworkPacket::From(std::move(segment));
    auto frame = LinkFrame::From(std::move(packet));
    frame.Reset();
    packet = NetworkPacket::From(std::move(frame));
    segment = TransportSegment::From(std::move(packet));
    segment.set_id(next_segment_id_++);
    return segment;
  }
  // DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-recycler]

  uint64_t next_segment_id_ = 0x1000;
};

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

TEST(PseudoEncrypt, RoundTrip) {
  pw::allocator::test::AllocatorForTest<2048> allocator;
  pw::async2::BasicDispatcher dispatcher;
  pw::span<const char*> poem(kTheAmaranth, kNumLines);
  constexpr uint64_t kKey = 0xDEADBEEFFEEDFACEull;

  // DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-e2e]
  // Instantiate the sending tasks.
  Producer producer(PW_ASYNC_TASK_NAME("producer"), poem);

  Encryptor encryptor(PW_ASYNC_TASK_NAME("encryptor"), kKey);
  producer.ConnectTo(encryptor);

  Relay<TransportSegment, NetworkPacket> net_sender(
      PW_ASYNC_TASK_NAME("net_sender"));
  encryptor.ConnectTo(net_sender);

  Relay<NetworkPacket, LinkFrame> link_sender(
      PW_ASYNC_TASK_NAME("link_sender"));
  net_sender.ConnectTo(link_sender);

  // Instantiate the receiving tasks.
  Relay<LinkFrame, NetworkPacket> link_receiver(
      PW_ASYNC_TASK_NAME("link_receiver"));
  link_sender.ConnectTo(link_receiver);

  Relay<NetworkPacket, TransportSegment> net_receiver(
      PW_ASYNC_TASK_NAME("net_receiver"));
  link_receiver.ConnectTo(net_receiver);

  Encryptor decryptor(PW_ASYNC_TASK_NAME("decryptor"), kKey);
  net_receiver.ConnectTo(decryptor);

  Consumer consumer(PW_ASYNC_TASK_NAME("consumer"), poem);
  decryptor.ConnectTo(consumer);

  // And finally, instantiate a task to recycle messages back to the producer.
  Recycler recycler(PW_ASYNC_TASK_NAME("recycler"));
  consumer.ConnectTo(recycler);
  recycler.ConnectTo(producer);

  // Add some blank segments for the recycler to vend.
  std::array<std::array<std::byte, kMaxDemoLinkFrameLength>, kCapacity> buffers;
  for (auto& buffer : buffers) {
    auto frame = LinkFrame::Create(allocator, buffer);
    PW_CHECK_OK(frame.status());
    auto packet = NetworkPacket::From(std::move(*frame));
    auto segment = TransportSegment::From(std::move(packet));
    PW_CHECK_OK(recycler.AddTransportSegment(std::move(segment)));
  }

  // Run all tasks on the dispatcher.
  dispatcher.Post(producer);
  dispatcher.Post(encryptor);
  dispatcher.Post(net_sender);
  dispatcher.Post(link_sender);
  dispatcher.Post(link_receiver);
  dispatcher.Post(net_receiver);
  dispatcher.Post(decryptor);
  dispatcher.Post(consumer);
  dispatcher.Post(recycler);
  // DOCSTAG: [pw_multibuf-examples-pseudo_encrypt-e2e]

  dispatcher.RunToCompletion();
  EXPECT_TRUE(producer.completed());
  EXPECT_TRUE(consumer.completed());
}

}  // namespace examples
