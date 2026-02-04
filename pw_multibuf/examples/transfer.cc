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
#include <limits>

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
#include "pw_checksum/crc32.h"
#include "pw_multibuf/examples/protocol.h"
#include "pw_multibuf/multibuf.h"
#include "pw_status/try.h"
#include "pw_unit_test/framework.h"

namespace examples {

using pw::multibuf::examples::DemoLinkFooter;
using pw::multibuf::examples::DemoLinkHeader;
using pw::multibuf::examples::DemoNetworkHeader;

constexpr size_t kDemoLinkHeaderLen =
    pw::multibuf::examples::kDemoLinkHeaderLen;
constexpr size_t kDemoLinkFooterLen =
    pw::multibuf::examples::kDemoLinkFooterLen;
constexpr size_t kDemoNetworkHeaderLen =
    pw::multibuf::examples::kDemoNetworkHeaderLen;
constexpr uint16_t kLinkSrcAddr = 0x3b15;
constexpr uint16_t kLinkDstAddr = 0x91a0;
constexpr uint64_t kNetSrcAddr = 0xdeadbeefcafef00d;
constexpr uint64_t kNetDstAddr = 0x123456789abcdef0;

// DOCSTAG: [pw_multibuf-examples-transfer-fields]
template <typename T>
constexpr T GetField(pw::ConstByteSpan data, size_t offset) {
  return pw::bytes::ReadInOrder<T>(pw::endian::little, &data[offset]);
}

template <typename T>
constexpr void SetField(pw::ByteSpan data, size_t offset, T value) {
  return pw::bytes::CopyInOrder<T>(pw::endian::little, value, &data[offset]);
}
// DOCSTAG: [pw_multibuf-examples-transfer-fields]

// DOCSTAG: [pw_multibuf-examples-transfer-network_packet]
class NetworkPacket {
 public:
  /// Create and a return a new network packet, or return an error if unable to
  /// allocate the needed memory.
  static pw::Result<NetworkPacket> Create(pw::Allocator& allocator) {
    auto metadata = allocator.MakeUnique<std::byte[]>(kDemoNetworkHeaderLen);
    if (metadata == nullptr) {
      return pw::Status::ResourceExhausted();
    }
    NetworkPacket packet(allocator);
    if (!packet.mbuf_->TryReserveForPushBack()) {
      return pw::Status::ResourceExhausted();
    }
    packet.mbuf_->PushBack(std::move(metadata));
    return packet;
  }

  void set_src_addr(uint64_t addr) {
    SetField<uint64_t>(header(), offsetof(DemoNetworkHeader, src_addr), addr);
  }

  void set_dst_addr(uint64_t addr) {
    SetField<uint64_t>(header(), offsetof(DemoNetworkHeader, dst_addr), addr);
  }

  /// Interpret the first chunk as a network packet header.
  constexpr DemoNetworkHeader GetHeader() const {
    return DemoNetworkHeader{
        .src_addr =
            GetField<uint64_t>(header(), offsetof(DemoNetworkHeader, src_addr)),
        .dst_addr =
            GetField<uint64_t>(header(), offsetof(DemoNetworkHeader, dst_addr)),
        .length =
            GetField<uint32_t>(header(), offsetof(DemoNetworkHeader, length)),
    };
  }

  /// Add a payload to a network packet.
  [[nodiscard]] bool AddPayload(pw::UniquePtr<std::byte[]>&& payload) {
    if (!mbuf_->TryReserveForPushBack()) {
      return false;
    }
    mbuf_->PushBack(std::move(payload));
    size_t length = mbuf_->size();
    PW_CHECK_UINT_LE(length, std::numeric_limits<uint32_t>::max());
    SetField<uint32_t>(header(),
                       offsetof(DemoNetworkHeader, length),
                       static_cast<uint32_t>(length));
    return true;
  }

  /// Consume a network packet and return its payload.
  static pw::Result<pw::UniquePtr<std::byte[]>> ExtractPayload(
      NetworkPacket&& packet) {
    DemoNetworkHeader header = packet.GetHeader();
    if (header.length != packet.mbuf_->size()) {
      return pw::Status::DataLoss();
    }
    PW_TRY_ASSIGN(
        auto iter,
        packet.mbuf_->Discard(packet.mbuf_->cbegin(), kDemoNetworkHeaderLen));
    return packet.mbuf_->Release(iter);
  }

 private:
  constexpr NetworkPacket(pw::Allocator& allocator) : mbuf_(allocator) {}

  friend class LinkFrame;
  explicit NetworkPacket(pw::FlatMultiBuf&& mbuf) : mbuf_(std::move(mbuf)) {}

  pw::ByteSpan header() { return *(mbuf_->Chunks().begin()); }
  constexpr pw::ConstByteSpan header() const {
    return *(mbuf_->ConstChunks().cbegin());
  }

  pw::FlatMultiBuf::Instance mbuf_;
};
// DOCSTAG: [pw_multibuf-examples-transfer-network_packet]

// DOCSTAG: [pw_multibuf-examples-transfer-link_frame]
class LinkFrame {
 public:
  /// Create and a return a new link frame, or return an error if unable to
  /// allocate the needed memory.
  static pw::Result<LinkFrame> Create(pw::Allocator& allocator) {
    auto metadata = allocator.MakeUnique<std::byte[]>(kDemoLinkHeaderLen +
                                                      kDemoLinkFooterLen);
    if (metadata == nullptr) {
      return pw::Status::ResourceExhausted();
    }
    LinkFrame frame(allocator);
    frame.mbuf_->PushBack(std::move(metadata));
    return frame;
  }

  constexpr auto Chunks() { return mbuf_->Chunks(); }
  constexpr auto ConstChunks() const { return mbuf_->ConstChunks(); }

  void set_src_addr(uint16_t addr) {
    SetField<uint16_t>(header(), offsetof(DemoLinkHeader, src_addr), addr);
  }

  void set_dst_addr(uint16_t addr) {
    SetField<uint16_t>(header(), offsetof(DemoLinkHeader, dst_addr), addr);
  }

  /// Interpret the first chunk as a link frame header.
  constexpr DemoLinkHeader GetHeader() const {
    return DemoLinkHeader{
        .src_addr =
            GetField<uint16_t>(header(), offsetof(DemoLinkHeader, src_addr)),
        .dst_addr =
            GetField<uint16_t>(header(), offsetof(DemoLinkHeader, dst_addr)),
        .length =
            GetField<uint16_t>(header(), offsetof(DemoLinkHeader, length)),
    };
  }

  /// Interpret the last chunk as a link frame footer.
  constexpr DemoLinkFooter GetFooter() const {
    return DemoLinkFooter{
        .crc32 = GetField<uint32_t>(footer(), offsetof(DemoLinkFooter, crc32)),
    };
  }

  /// Moves the given netrowk packet into the payload of this frame.
  [[nodiscard]] bool AddNetworkPacket(NetworkPacket&& packet) {
    auto iter = mbuf_->cend() - kDemoLinkFooterLen;
    if (!mbuf_->TryReserveForInsert(iter, *packet.mbuf_)) {
      return false;
    }
    mbuf_->Insert(iter, std::move(*packet.mbuf_));
    size_t length = mbuf_->size();
    PW_CHECK_UINT_LE(length, std::numeric_limits<uint16_t>::max());
    SetField<uint16_t>(header(),
                       offsetof(DemoLinkHeader, length),
                       static_cast<uint16_t>(length));
    return true;
  }

  /// Updates the checksum for the finished frame.
  void Finalize() {
    SetField<uint32_t>(
        footer(), offsetof(DemoLinkFooter, crc32), CalculateCheckSum());
  }

  /// Examines a link frame. If it is valid, returns its payload as a network
  /// packet, otherwise returns an error.
  static pw::Result<NetworkPacket> ExtractNetworkPacket(LinkFrame&& frame) {
    DemoLinkHeader header = frame.GetHeader();
    DemoLinkFooter footer = frame.GetFooter();
    if (header.length != frame.mbuf_->size() ||
        footer.crc32 != frame.CalculateCheckSum()) {
      return pw::Status::DataLoss();
    }
    uint32_t packet_length =
        header.length - (kDemoLinkHeaderLen + kDemoLinkFooterLen);
    auto iter = frame.mbuf_->cbegin();
    PW_TRY_ASSIGN(iter, frame.mbuf_->Discard(iter, kDemoLinkHeaderLen));
    iter += packet_length;
    PW_TRY_ASSIGN(iter, frame.mbuf_->Discard(iter, kDemoLinkFooterLen));
    return NetworkPacket(std::move(*frame.mbuf_));
  }

 private:
  constexpr LinkFrame(pw::Allocator& allocator) : mbuf_(allocator) {}

  uint32_t CalculateCheckSum() const {
    pw::checksum::Crc32 crc32;
    pw::ConstByteSpan prev;
    for (pw::ConstByteSpan chunk : mbuf_->ConstChunks()) {
      crc32.Update(prev);
      prev = chunk;
    }
    return crc32.value();
  }

  constexpr pw::ByteSpan header() { return *(mbuf_->Chunks().begin()); }
  constexpr pw::ConstByteSpan header() const {
    return *(mbuf_->ConstChunks().cbegin());
  }

  constexpr pw::ByteSpan footer() { return *(--(mbuf_->Chunks().end())); }
  constexpr pw::ConstByteSpan footer() const {
    return *(--(mbuf_->ConstChunks().cend()));
  }

  pw::FlatMultiBuf::Instance mbuf_;
};
// DOCSTAG: [pw_multibuf-examples-transfer-link_frame]

/// Asynchronously forward that has be written to callers trying to read.
class Link {
 public:
  pw::async2::Poll<> Write(pw::async2::Context& context,
                           pw::ConstByteSpan tx_buffer) {
    if (!pending_.has_value()) {
      pending_ = tx_buffer;
      rx_waker_.Wake();
    } else if (pending_->empty()) {
      pending_.reset();
      return pw::async2::Ready();
    }
    PW_ASYNC_STORE_WAKER(context, tx_waker_, "transmitting data");
    return pw::async2::Pending();
  }

  pw::async2::Poll<size_t> Read(pw::async2::Context& context,
                                pw::ByteSpan rx_buffer) {
    if (!pending_.has_value() || pending_->empty()) {
      PW_ASYNC_STORE_WAKER(context, rx_waker_, "waiting for data");
      return pw::async2::Pending();
    }
    size_t len = std::min(pending_->size(), rx_buffer.size());
    std::memcpy(rx_buffer.data(), pending_->data(), len);
    pending_ = pending_->subspan(len);
    tx_waker_.Wake();
    return pw::async2::Ready(len);
  }

 private:
  std::optional<pw::ConstByteSpan> pending_;
  pw::async2::Waker tx_waker_;
  pw::async2::Waker rx_waker_;
};

const char* kLoremIpsum =
    "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod "
    "tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim "
    "veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea "
    "commodo consequat. Duis aute irure dolor in reprehenderit in voluptate "
    "velit esse cillum dolore eu fugiat nulla pariatur. Excepteur sint "
    "occaecat cupidatat non proident, sunt in culpa qui officia deserunt "
    "mollit anim id est laborum.";

TEST(LinkTest, SendAndReceiveData) {
  pw::allocator::test::AllocatorForTest<2048> allocator;
  Link link;
  pw::async2::BasicDispatcher dispatcher;

  auto tx_payload =
      allocator.MakeUnique<std::byte[]>(std::strlen(kLoremIpsum) + 1);
  ASSERT_NE(tx_payload, nullptr);
  std::memcpy(tx_payload.get(), kLoremIpsum, tx_payload.size());

  // DOCSTAG: [pw_multibuf-examples-transfer-create]
  pw::Result<NetworkPacket> tx_packet = NetworkPacket::Create(allocator);
  ASSERT_EQ(tx_packet.status(), pw::OkStatus());
  tx_packet->set_src_addr(kNetSrcAddr);
  tx_packet->set_dst_addr(kNetDstAddr);
  ASSERT_TRUE(tx_packet->AddPayload(std::move(tx_payload)));

  pw::Result<LinkFrame> tx_frame = LinkFrame::Create(allocator);
  ASSERT_EQ(tx_frame.status(), pw::OkStatus());
  tx_frame->set_src_addr(kLinkSrcAddr);
  tx_frame->set_dst_addr(kLinkDstAddr);
  ASSERT_TRUE(tx_frame->AddNetworkPacket(std::move(*tx_packet)));
  tx_frame->Finalize();
  // DOCSTAG: [pw_multibuf-examples-transfer-create]

  auto tx_iter = tx_frame->ConstChunks().begin();
  pw::async2::FuncTask write_frame(
      [&](pw::async2::Context& context) -> pw::async2::Poll<> {
        while (tx_iter != tx_frame->ConstChunks().end()) {
          if (!tx_iter->empty()) {
            PW_TRY_READY(link.Write(context, *tx_iter));
          }
          ++tx_iter;
        }
        return pw::async2::Ready();
      });
  dispatcher.Post(write_frame);

  pw::Result<LinkFrame> rx_frame = LinkFrame::Create(allocator);
  ASSERT_EQ(rx_frame.status(), pw::OkStatus());
  pw::ByteSpan raw_frame_header =
      rx_frame->Chunks().begin()->subspan(0, kDemoLinkHeaderLen);

  pw::Result<NetworkPacket> rx_packet = NetworkPacket::Create(allocator);
  ASSERT_EQ(rx_packet.status(), pw::OkStatus());

  bool read_frame_header_finished = false;
  pw::async2::FuncTask read_frame_header(
      [&](pw::async2::Context& context) -> pw::async2::Poll<> {
        while (!raw_frame_header.empty()) {
          PW_TRY_READY_ASSIGN(size_t bytes_read,
                              link.Read(context, raw_frame_header));
          raw_frame_header = raw_frame_header.subspan(bytes_read);
        }
        read_frame_header_finished = true;
        return pw::async2::Ready();
      });
  dispatcher.Post(read_frame_header);
  dispatcher.RunUntilStalled();
  EXPECT_TRUE(read_frame_header_finished);

  DemoLinkHeader frame_header = rx_frame->GetHeader();
  EXPECT_EQ(frame_header.src_addr, kLinkSrcAddr);
  EXPECT_EQ(frame_header.dst_addr, kLinkDstAddr);

  size_t payload_len =
      frame_header.length -
      (kDemoLinkHeaderLen + kDemoNetworkHeaderLen + kDemoLinkFooterLen);
  auto rx_payload = allocator.MakeUnique<std::byte[]>(payload_len);
  ASSERT_NE(rx_payload, nullptr);

  ASSERT_TRUE(rx_packet->AddPayload(std::move(rx_payload)));
  ASSERT_TRUE(rx_frame->AddNetworkPacket(std::move(*rx_packet)));

  auto iter = rx_frame->Chunks().begin();
  pw::ByteSpan chunk = *(++iter);
  pw::async2::FuncTask read_remaining_frame(
      [&](pw::async2::Context& context) -> pw::async2::Poll<> {
        while (true) {
          PW_TRY_READY_ASSIGN(size_t bytes_read, link.Read(context, chunk));
          chunk = chunk.subspan(bytes_read);
          if (!chunk.empty()) {
            continue;
          }
          ++iter;
          if (iter == rx_frame->ConstChunks().end()) {
            break;
          }
          chunk = *iter;
        }
        return pw::async2::Ready();
      });
  dispatcher.Post(read_remaining_frame);
  dispatcher.RunToCompletion();

  rx_packet = LinkFrame::ExtractNetworkPacket(std::move(*rx_frame));
  ASSERT_EQ(rx_packet.status(), pw::OkStatus());

  DemoNetworkHeader packet_header = rx_packet->GetHeader();
  EXPECT_EQ(packet_header.src_addr, kNetSrcAddr);
  EXPECT_EQ(packet_header.dst_addr, kNetDstAddr);

  auto rx_payload_result = NetworkPacket::ExtractPayload(std::move(*rx_packet));
  ASSERT_EQ(rx_payload_result.status(), pw::OkStatus());
  rx_payload = std::move(rx_payload_result.value());

  auto* rx_text = reinterpret_cast<const char*>(rx_payload.get());
  EXPECT_STREQ(rx_text, kLoremIpsum);
}

}  // namespace examples
