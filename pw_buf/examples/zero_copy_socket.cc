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

#include <cstring>

#include "pw_allocator/testing.h"
#include "pw_assert/check.h"
#include "pw_buf/buf.h"
#include "pw_containers/dynamic_deque.h"
#include "pw_unit_test/framework.h"

namespace examples {

class Socket;
class ExampleSocket;
class ExampleLayeredSocket;

/// Manages a reserved slice of memory for zero-copy packet building.
///
/// WriteReservation provides a mutable view of a buffer where a client can
/// write a payload. When the payload is complete, calling `Commit` sends
/// the packet through the associated socket.
class WriteReservation {
 public:
  using iterator = pw::Buf::iterator;

  std::byte* data() { return buf_.data(); }
  size_t size() const { return buf_.size(); }
  std::byte& operator[](size_t index) { return buf_[index]; }
  iterator begin() { return buf_.begin(); }
  iterator end() { return buf_.end(); }

  /// Finalizes the packet with the specified payload size and writes it.
  void Commit(size_t size_bytes);

 private:
  friend class Socket;
  friend class ExampleSocket;
  friend class ExampleLayeredSocket;

  explicit WriteReservation(pw::Buf&& buf, Socket* socket)
      : buf_(std::move(buf)), socket_(socket) {}

  pw::Buf buf_;
  Socket* socket_;
};

/// Abstract base class for a packet-oriented connection socket.
class Socket {
 public:
  virtual ~Socket() = default;

  /// Reads a packet payload.
  virtual pw::ConstBuf Read() = 0;

  /// Writes an existing packet payload.
  virtual void Write(pw::Buf&& buf) = 0;

  /// Reserves a buffer for constructing a packet payload.
  virtual WriteReservation ReserveWrite() = 0;

 protected:
  /// Retargets a WriteReservation to this socket, slicing the buffer to
  /// leave room for protocol headers/footers.
  void Adopt(WriteReservation& reservation,
             size_t prefix_trim,
             size_t suffix_trim) {
    reservation.socket_ = this;
    size_t size = reservation.buf_.size();
    PW_CHECK(prefix_trim + suffix_trim <= size);
    reservation.buf_ = pw::Slice(std::move(reservation.buf_),
                                 prefix_trim,
                                 size - (prefix_trim + suffix_trim));
  }
};

void WriteReservation::Commit(size_t size_bytes) {
  socket_->Write(pw::Truncate(std::move(buf_), size_bytes));
}

// DOCSTAG: [pw_buf-examples-socket]
/// An example socket that implements a simple 4-byte length-prefixed protocol.
class ExampleSocket : public Socket {
 public:
  explicit ExampleSocket(pw::Allocator& allocator)
      : allocator_(&allocator),
        read_queue_(allocator),
        outbound_queue_(allocator) {}

  pw::ConstBuf Read() override {
    PW_CHECK(!read_queue_.empty());
    pw::ConstBuf front = std::move(read_queue_.front());
    read_queue_.pop_front();
    return front;
  }

  bool HasReadPacket() const { return !read_queue_.empty(); }

  WriteReservation ReserveWrite() override {
    // Allocate a buffer of 128 bytes. Reserve the first 4 bytes for the header.
    auto owned = allocator_->MakeUnique<std::byte[]>(128);
    pw::Buf buf(std::move(owned), sizeof(uint32_t), 128 - sizeof(uint32_t));
    return WriteReservation(std::move(buf), this);
  }

  void EnqueueForRead(pw::UniquePtr<std::byte[]>&& owned) {
    if (owned == nullptr || owned.size() < sizeof(uint32_t)) {
      return;
    }
    uint32_t payload_len = 0;
    std::memcpy(&payload_len, owned.get(), sizeof(uint32_t));
    PW_CHECK(sizeof(uint32_t) + payload_len <= owned.size());

    pw::ConstBuf payload_buf =
        pw::Buf(std::move(owned), sizeof(uint32_t), payload_len);
    read_queue_.push_back(std::move(payload_buf));
  }

  bool HasOutboundPacket() const { return !outbound_queue_.empty(); }

  pw::Buf PopOutboundPacket() {
    PW_CHECK(!outbound_queue_.empty());
    pw::Buf front = std::move(outbound_queue_.front());
    outbound_queue_.pop_front();
    return front;
  }

  void Write(pw::Buf&& payload_buf) override {
    // Reclaim the 4-byte prefix space to write the length header.
    pw::Buf packet_buf =
        pw::ReclaimPrefix(std::move(payload_buf), sizeof(uint32_t));

    // Write length header
    uint32_t payload_len =
        static_cast<uint32_t>(packet_buf.size() - sizeof(uint32_t));
    std::memcpy(packet_buf.data(), &payload_len, sizeof(uint32_t));

    outbound_queue_.push_back(std::move(packet_buf));
  }

 private:
  pw::Allocator* allocator_;
  pw::DynamicDeque<pw::ConstBuf> read_queue_;
  pw::DynamicDeque<pw::Buf> outbound_queue_;
};

/// An example layered socket that wraps another socket and adds a 4-byte CRC
/// checksum footer, demonstrating zero-copy layered packet construction.
class ExampleLayeredSocket : public Socket {
 public:
  explicit ExampleLayeredSocket(Socket& lower_socket)
      : lower_socket_(&lower_socket) {}

  pw::ConstBuf Read() override {
    pw::ConstBuf raw = lower_socket_->Read();
    PW_CHECK(raw.size() >= sizeof(uint32_t) * 2);

    uint32_t payload_len = 0;
    std::memcpy(&payload_len, raw.data(), sizeof(uint32_t));
    PW_CHECK(sizeof(uint32_t) * 2 + payload_len <= raw.size());

    uint32_t crc = 0;
    std::memcpy(
        &crc, raw.data() + sizeof(uint32_t) + payload_len, sizeof(uint32_t));
    PW_CHECK(crc == 0xDEADBEEF);

    return pw::Slice(std::move(raw), sizeof(uint32_t), payload_len);
  }

  void Write(pw::Buf&& payload_buf) override {
    // Reclaim 4 bytes at the front (for header) and 4 bytes at the end (for CRC
    // footer).
    pw::Buf packet_buf =
        pw::Reclaim(std::move(payload_buf), sizeof(uint32_t), sizeof(uint32_t));

    // Write header (payload len)
    uint32_t payload_len =
        static_cast<uint32_t>(packet_buf.size() - sizeof(uint32_t) * 2);
    std::memcpy(packet_buf.data(), &payload_len, sizeof(uint32_t));

    // Write footer (CRC)
    uint32_t crc = 0xDEADBEEF;
    std::memcpy(packet_buf.data() + sizeof(uint32_t) + payload_len,
                &crc,
                sizeof(uint32_t));

    lower_socket_->Write(std::move(packet_buf));
  }

  WriteReservation ReserveWrite() override {
    // Delegate reservation to lower socket, then adopt it by shrinking
    // the available payload space to leave room for the layered header/footer.
    WriteReservation res = lower_socket_->ReserveWrite();
    Adopt(res, sizeof(uint32_t), sizeof(uint32_t));
    return res;
  }

 private:
  Socket* lower_socket_;
};
// DOCSTAG: [pw_buf-examples-socket]

}  // namespace examples

namespace {

TEST(ExampleTests, ExampleSocketTest) {
  pw::allocator::test::AllocatorForTest<512> test_allocator;
  examples::ExampleSocket socket(test_allocator);

  // Test Read path
  {
    auto unique_packet = test_allocator.MakeUnique<std::byte[]>(10);
    uint32_t len = 6;
    std::memcpy(unique_packet.get(), &len, sizeof(uint32_t));
    std::memcpy(unique_packet.get() + sizeof(uint32_t), "hello!", 6);

    socket.EnqueueForRead(std::move(unique_packet));
    EXPECT_TRUE(socket.HasReadPacket());

    pw::ConstBuf read_buf = socket.Read();
    EXPECT_FALSE(socket.HasReadPacket());
    EXPECT_EQ(read_buf.size(), 6u);
    EXPECT_EQ(std::memcmp(read_buf.data(), "hello!", 6), 0);
  }

  // Test Write path
  {
    examples::WriteReservation write_res = socket.ReserveWrite();
    EXPECT_GE(write_res.size(), 100u);

    std::memcpy(write_res.data(), "world!!", 7);
    write_res.Commit(7);

    EXPECT_TRUE(socket.HasOutboundPacket());
    pw::Buf outbound_buf = socket.PopOutboundPacket();
    EXPECT_FALSE(socket.HasOutboundPacket());

    EXPECT_EQ(outbound_buf.size(), 11u);
    uint32_t len = 0;
    std::memcpy(&len, outbound_buf.data(), sizeof(uint32_t));
    EXPECT_EQ(len, 7u);
    EXPECT_EQ(std::memcmp(outbound_buf.data() + sizeof(uint32_t), "world!!", 7),
              0);
  }
}

TEST(ExampleTests, ExampleLayeredSocketTest) {
  pw::allocator::test::AllocatorForTest<512> test_allocator;
  examples::ExampleSocket lower_socket(test_allocator);
  examples::ExampleLayeredSocket layered_socket(lower_socket);

  // Test Read path
  {
    auto unique_packet = test_allocator.MakeUnique<std::byte[]>(17);
    uint32_t outer_len = 13;
    uint32_t inner_len = 5;
    uint32_t crc = 0xDEADBEEF;

    std::memcpy(unique_packet.get(), &outer_len, sizeof(uint32_t));
    std::memcpy(unique_packet.get() + 4, &inner_len, sizeof(uint32_t));
    std::memcpy(unique_packet.get() + 8, "hello", 5);
    std::memcpy(unique_packet.get() + 13, &crc, sizeof(uint32_t));

    lower_socket.EnqueueForRead(std::move(unique_packet));
    EXPECT_TRUE(lower_socket.HasReadPacket());

    pw::ConstBuf read_buf = layered_socket.Read();
    EXPECT_EQ(read_buf.size(), 5u);
    EXPECT_EQ(std::memcmp(read_buf.data(), "hello", 5), 0);
  }

  // Test Write path
  {
    examples::WriteReservation write_res = layered_socket.ReserveWrite();
    EXPECT_EQ(write_res.size(), 116u);

    std::memcpy(write_res.data(), "world", 5);
    write_res.Commit(5);

    EXPECT_TRUE(lower_socket.HasOutboundPacket());
    pw::Buf outbound_buf = lower_socket.PopOutboundPacket();
    EXPECT_FALSE(lower_socket.HasOutboundPacket());

    EXPECT_EQ(outbound_buf.size(), 17u);

    uint32_t outer_len = 0;
    std::memcpy(&outer_len, outbound_buf.data(), sizeof(uint32_t));
    EXPECT_EQ(outer_len, 13u);

    uint32_t inner_len = 0;
    std::memcpy(&inner_len, outbound_buf.data() + 4, sizeof(uint32_t));
    EXPECT_EQ(inner_len, 5u);

    EXPECT_EQ(std::memcmp(outbound_buf.data() + 8, "world", 5), 0);

    uint32_t crc = 0;
    std::memcpy(&crc, outbound_buf.data() + 13, sizeof(uint32_t));
    EXPECT_EQ(crc, 0xDEADBEEF);
  }
}

}  // namespace
