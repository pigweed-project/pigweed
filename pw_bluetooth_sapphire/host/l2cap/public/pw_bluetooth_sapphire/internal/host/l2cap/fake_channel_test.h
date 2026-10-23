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

#include <memory>

#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/fake_channel.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/l2cap_defs.h"

namespace bt::l2cap::testing {

// Provides a common GTest harness base class for protocols tests that operate
// over a L2CAP channel. This harness provides:
//
//   * A simple way to initialize and access a FakeChannel.
//   * Basic command<->response expectation.
class FakeChannelTest : public pw::async::test::FakeDispatcherFixture {
 public:
  FakeChannelTest() = default;
  ~FakeChannelTest() override = default;

 protected:
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

  void SetUp() override;

  // Creates a new FakeChannel and returns it. A WeakPtr to the returned
  // channel is stored internally so that the returned channel can be accessed
  // by tests even if its ownership is passed outside of the test harness.
  std::unique_ptr<FakeChannel> CreateFakeChannel(const ChannelOptions& options);

  // Runs the event loop and returns true if |expected| is received within a 10
  // second period.
  //
  // Returns false if no such response is received or no FakeChannel has been
  // initialized via CreateFakeChannel().
  //
  // NOTE: This overwrites the underlying FakeChannel's "send callback" by
  // calling FakeChannel::SetSendCallback().
  bool Expect(const ByteBuffer& expected);

  // Emulates the receipt of |packet| and returns true if a response that
  // matches |expected_response| is sent back over the underlying FakeChannel.
  // Returns false if no such response is received or no FakeChannel has been
  // initialized via CreateFakeChannel().
  //
  // NOTE: This overwrites the underlying FakeChannel's "send callback" by
  // calling FakeChannel::SetSendCallback().
  bool ReceiveAndExpect(const ByteBuffer& packet,
                        const ByteBuffer& expected_response);

  FakeChannel::WeakPtr fake_chan() const { return fake_chan_; }

  void set_fake_chan(FakeChannel::WeakPtr chan) { fake_chan_ = chan; }

 private:
  // Helper that sets a reception expectation callback with |expected| then
  // sends |packet| if it is not std::nullopt, returning whether |expected| was
  // received when the test loop run until idle.
  bool ExpectAfterMaybeReceiving(std::optional<BufferView> packet,
                                 const ByteBuffer& expected);

  FakeChannel::WeakPtr fake_chan_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(FakeChannelTest);
};

}  // namespace bt::l2cap::testing
