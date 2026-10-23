// Copyright 2023 The Pigweed Authors
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

#pragma once
#include <pw_async/fake_dispatcher_fixture.h>

#include "pw_bluetooth_sapphire/internal/host/l2cap/fake_channel.h"

namespace bt::l2cap::testing {

// Provides a GTest harness base class for tests that operate over an L2CAP
// channel. Outgoing packet expectations are written using the
// EXPECT_PACKET_OUT() macro, and may specify 0 or more responses that should be
// sent when the outgoing packet is received by MockChannelTest:
//
// StaticByteBuffer kRequest_0(...);
// StaticByteBuffer kRequest_1(...);
// ...
// EXPECT_PACKET_OUT(kRequest_0);
// EXPECT_PACKET_OUT(kRequest_1, kResponse_0, kResponse_1);
// foo.Start();
// EXPECT_TRUE(AllExpectedPacketsSent());
// fake_chan()->Receive(kRequest_2); // Simulate inbound packet
class MockChannelTest : public pw::async::test::FakeDispatcherFixture {
 public:
  struct ExpectationMetadata {
    const char* file;
    int line;
    const char* expectation;
  };

  struct PacketExpectation {
    DynamicByteBuffer data;
    ExpectationMetadata meta;
  };

  class Transaction {
   public:
    // The |expected| buffer and the buffers in |replies| will be copied, so
    // their lifetime does not need to extend past Transaction construction.
    Transaction(const ByteBuffer& expected,
                const std::vector<const ByteBuffer*>& replies,
                ExpectationMetadata meta);
    virtual ~Transaction() = default;
    Transaction(Transaction&& other) = default;
    Transaction& operator=(Transaction&& other) = default;

    // Returns true if the transaction matches the given packet.
    bool Match(const ByteBuffer& packet);

    const PacketExpectation& expected() { return expected_; }
    void set_expected(const PacketExpectation& expected) {
      expected_ = PacketExpectation{.data = DynamicByteBuffer(expected.data),
                                    .meta = expected.meta};
    }

    std::queue<DynamicByteBuffer>& replies() { return replies_; }

   private:
    PacketExpectation expected_;
    std::queue<DynamicByteBuffer> replies_;
  };

  struct ChannelOptions {
    explicit ChannelOptions(ChannelId id_val, uint16_t mtu_val = kDefaultMTU)
        : ChannelOptions(id_val, id_val, mtu_val) {}
    ChannelOptions(ChannelId id_val, ChannelId remote_id_val, uint16_t mtu_val)
        : id(id_val), remote_id(remote_id_val), mtu(mtu_val) {}

    ChannelId id;
    ChannelId remote_id;
    uint16_t mtu;
    hci_spec::ConnectionHandle conn_handle = 0x0001;
    bt::LinkType link_type = bt::LinkType::kLE;
  };

  MockChannelTest() = default;
  ~MockChannelTest() override = default;

  void TearDown() override;

  pw::async::HeapDispatcher& heap_dispatcher() { return heap_dispatcher_; }

  // Queues a transaction into the MockChannelTest's expected packet queue. Each
  // packet received through the channel will be verified against the next
  // expected transaction in the queue. A mismatch will cause a fatal assertion.
  // On a match, MockChannelTest will send back the replies provided in the
  // transaction. NOTE: It is recommended to use the EXPECT_PACKET_OUT macro
  // instead of calling this method directly.
  void QueueTransaction(const ByteBuffer& expected,
                        const std::vector<const ByteBuffer*>& replies,
                        ExpectationMetadata meta);

  // Create a FakeChannel owned by this test fixture. Replaces any existing
  // channel.
  FakeChannel::WeakPtr CreateFakeChannel(const ChannelOptions& options);

  using PacketCallback = fit::function<void(const ByteBuffer& packet)>;
  void SetPacketCallback(PacketCallback callback) {
    packet_callback_ = std::move(callback);
  }

  bool AllExpectedPacketsSent() const { return transactions_.empty(); }

  FakeChannel::WeakPtr fake_chan() const { return fake_chan_->AsWeakPtr(); }

 private:
  void OnPacketSent(std::unique_ptr<ByteBuffer> packet);

  std::queue<Transaction> transactions_;
  std::unique_ptr<FakeChannel> fake_chan_;
  PacketCallback packet_callback_;
  pw::async::HeapDispatcher heap_dispatcher_{dispatcher()};
};

// Helper macro for expecting a packet and receiving a variable number of
// responses.
#define EXPECT_PACKET_OUT(expected, ...)                                     \
  QueueTransaction((expected),                                               \
                   {__VA_ARGS__},                                            \
                   bt::l2cap::testing::MockChannelTest::ExpectationMetadata{ \
                       __FILE__, __LINE__, #expected})

}  // namespace bt::l2cap::testing
