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

#include "pw_bluetooth_sapphire/internal/host/l2cap/dynamic_channel_registry.h"

#include "pw_unit_test/framework.h"

namespace bt::l2cap::internal {
namespace {

constexpr uint16_t kNumChannelsAllowed = 256;
constexpr uint16_t kPsm = 0x0001;
constexpr ChannelId kRemoteCId = 0x60a3;
constexpr ChannelParameters kChannelParams;

class FakeDynamicChannel final : public DynamicChannel {
 public:
  FakeDynamicChannel(DynamicChannelRegistry* registry,
                     Psm psm,
                     ChannelId local_cid,
                     ChannelId remote_cid)
      : DynamicChannel(registry, psm, local_cid, remote_cid) {}

  // DynamicChannel overrides
  bool IsConnected() const override { return connected_; }
  bool IsOpen() const override { return open_; }

  ChannelInfo info() const override {
    return ChannelInfo::MakeBasicMode(kDefaultMTU, kDefaultMTU);
  }

  void DoConnect(ChannelId remote_cid) {
    ASSERT_TRUE(SetRemoteChannelId(remote_cid))
        << "Could not set non-unique remote_cid " << remote_cid;
    connected_ = true;
  }

  void DoOpen(bool new_open = true) {
    open_ = new_open;
    open_result_cb_();
  }

  void DoRemoteClose() {
    open_ = false;
    connected_ = false;
    OnDisconnected();
  }

  // After calling |set_defer_disconnect_callback|, this returns the callback
  // passed to |Disconnect|, or an empty callback if |Disconnect| hasn't been
  // called.
  DisconnectDoneCallback& disconnect_done_callback() {
    return disconnect_done_callback_;
  }

  void set_defer_disconnect_done_callback() {
    defer_disconnect_done_callback_ = true;
  }

 private:
  // DynamicChannel overrides
  void Open(fit::closure open_result_cb) override {
    open_result_cb_ = std::move(open_result_cb);
  }
  void Disconnect(DisconnectDoneCallback done_cb) override {
    open_ = false;
    connected_ = false;

    bt_log(DEBUG,
           "l2cap",
           "Got Disconnect %#.4x callback: %d",
           local_cid(),
           psm());

    ASSERT_FALSE(disconnect_done_callback_);
    if (defer_disconnect_done_callback_) {
      disconnect_done_callback_ = std::move(done_cb);
    } else {
      done_cb();
    }
  }

  fit::closure open_result_cb_;
  DisconnectDoneCallback disconnect_done_callback_;

  // If true, the Disconnect call does not immediately signal its callback. The
  // test will have to call it explicitly with |disconnect_callback()|.
  bool defer_disconnect_done_callback_ = false;

  bool connected_ = false;
  bool open_ = false;
};

// Fake registry subclass for testing inherited logic. Stubs out |MakeOutbound|
// to vend FakeDynamicChannels.
class TestDynamicChannelRegistry final : public DynamicChannelRegistry {
 public:
  TestDynamicChannelRegistry(DynamicChannelCallback close_cb,
                             ServiceRequestCallback service_request_cb)
      : DynamicChannelRegistry(kNumChannelsAllowed,
                               std::move(close_cb),
                               std::move(service_request_cb),
                               /*random_channel_ids=*/true) {}

  // Returns previous channel created.
  FakeDynamicChannel* last_channel() { return last_channel_; }

  // Make public for testing.
  using DynamicChannelRegistry::AliveChannelCount;
  using DynamicChannelRegistry::FindAvailableChannelId;
  using DynamicChannelRegistry::FindChannelByLocalId;
  using DynamicChannelRegistry::FindChannelByRemoteId;
  using DynamicChannelRegistry::ForEach;
  using DynamicChannelRegistry::RequestService;

 private:
  // DynamicChannelRegistry overrides
  DynamicChannelPtr MakeOutbound(Psm psm,
                                 ChannelId local_cid,
                                 ChannelParameters) override {
    return MakeChannelInternal(psm, local_cid, kInvalidChannelId);
  }

  DynamicChannelPtr MakeInbound(Psm psm,
                                ChannelId local_cid,
                                ChannelId remote_cid,
                                ChannelParameters) override {
    auto channel = MakeChannelInternal(psm, local_cid, remote_cid);
    channel->DoConnect(remote_cid);
    return channel;
  }

  std::unique_ptr<FakeDynamicChannel> MakeChannelInternal(
      Psm psm, ChannelId local_cid, ChannelId remote_cid) {
    auto channel =
        std::make_unique<FakeDynamicChannel>(this, psm, local_cid, remote_cid);
    last_channel_ = channel.get();
    return channel;
  }

  FakeDynamicChannel* last_channel_ = nullptr;
};

// DynamicChannelCallback static handler
void DoNothing(const DynamicChannel*) {}

// ServiceRequestCallback static handler
std::optional<DynamicChannelRegistry::ServiceInfo> RejectAllServices(
    Psm /*psm*/) {
  return std::nullopt;
}

TEST(DynamicChannelRegistryTest, OpenAndRemoteCloseChannel) {
  ChannelId local_cid = kInvalidChannelId;
  ChannelId remote_cid = kInvalidChannelId;
  bool close_cb_called = false;
  auto close_cb = [&](const DynamicChannel* chan) {
    EXPECT_FALSE(close_cb_called);
    close_cb_called = true;
    EXPECT_TRUE(chan);
    EXPECT_FALSE(chan->IsConnected());
    EXPECT_FALSE(chan->IsOpen());
    EXPECT_EQ(local_cid, chan->local_cid());
    EXPECT_EQ(remote_cid, chan->remote_cid());
  };

  TestDynamicChannelRegistry registry(std::move(close_cb), RejectAllServices);
  EXPECT_NE(kInvalidChannelId, registry.FindAvailableChannelId());

  bool open_result_cb_called = false;
  auto open_result_cb = [&](const DynamicChannel* chan) {
    EXPECT_FALSE(open_result_cb_called);
    open_result_cb_called = true;
    EXPECT_TRUE(chan);
    EXPECT_EQ(kPsm, chan->psm());
    // The registry marks the channel opened when handing it to the caller.
    EXPECT_TRUE(chan->opened());
    local_cid = chan->local_cid();
    remote_cid = chan->remote_cid();
  };

  registry.OpenOutbound(kPsm, kChannelParams, std::move(open_result_cb));
  registry.last_channel()->DoConnect(kRemoteCId);
  registry.last_channel()->DoOpen();

  EXPECT_TRUE(open_result_cb_called);
  EXPECT_FALSE(close_cb_called);
  auto channel_by_local_id = registry.FindChannelByLocalId(local_cid);
  auto channel_by_remote_id = registry.FindChannelByRemoteId(remote_cid);
  EXPECT_TRUE(channel_by_local_id);
  EXPECT_TRUE(channel_by_remote_id);
  EXPECT_EQ(channel_by_local_id, channel_by_remote_id);

  registry.last_channel()->DoRemoteClose();
  EXPECT_TRUE(close_cb_called);
  EXPECT_FALSE(registry.FindChannelByLocalId(local_cid));
  EXPECT_FALSE(registry.FindChannelByRemoteId(remote_cid));
}

TEST(DynamicChannelRegistryTest, OpenAndLocalCloseChannel) {
  bool registry_close_cb_called = false;
  auto registry_close_cb = [&](const DynamicChannel*) {
    registry_close_cb_called = true;
  };

  TestDynamicChannelRegistry registry(std::move(registry_close_cb),
                                      RejectAllServices);

  bool open_result_cb_called = false;
  ChannelId local_cid = kInvalidChannelId;
  auto open_result_cb = [&](const DynamicChannel* chan) {
    EXPECT_FALSE(open_result_cb_called);
    open_result_cb_called = true;
    EXPECT_TRUE(chan);
    local_cid = chan->local_cid();
  };

  registry.OpenOutbound(kPsm, kChannelParams, std::move(open_result_cb));
  registry.last_channel()->DoConnect(kRemoteCId);
  registry.last_channel()->DoOpen();

  EXPECT_TRUE(open_result_cb_called);
  EXPECT_TRUE(registry.FindChannelByLocalId(local_cid));

  bool close_cb_called = false;
  registry.CloseChannel(local_cid, [&] { close_cb_called = true; });
  EXPECT_FALSE(registry_close_cb_called);
  EXPECT_TRUE(close_cb_called);
  EXPECT_FALSE(registry.FindChannelByLocalId(local_cid));
}

TEST(DynamicChannelRegistryTest, RejectServiceRequest) {
  bool service_request_cb_called = false;
  auto service_request_cb = [&service_request_cb_called](Psm psm) {
    EXPECT_FALSE(service_request_cb_called);
    EXPECT_EQ(kPsm, psm);
    service_request_cb_called = true;
    return std::nullopt;
  };

  TestDynamicChannelRegistry registry(DoNothing, std::move(service_request_cb));

  registry.RequestService(kPsm, registry.FindAvailableChannelId(), kRemoteCId);
  EXPECT_TRUE(service_request_cb_called);
  EXPECT_FALSE(registry.last_channel());
}

TEST(DynamicChannelRegistryTest, AcceptServiceRequestThenOpenOk) {
  bool open_result_cb_called = false;
  ChannelId local_cid = kInvalidChannelId;
  ChannelId remote_cid = kInvalidChannelId;
  DynamicChannelRegistry::DynamicChannelCallback open_result_cb =
      [&](const DynamicChannel* chan) {
        EXPECT_FALSE(open_result_cb_called);
        open_result_cb_called = true;
        EXPECT_TRUE(chan);
        EXPECT_EQ(kPsm, chan->psm());
        local_cid = chan->local_cid();
        remote_cid = chan->remote_cid();
      };

  bool service_request_cb_called = false;
  auto service_request_cb = [&service_request_cb_called,
                             cb = std::move(open_result_cb)](Psm psm) mutable {
    EXPECT_FALSE(service_request_cb_called);
    EXPECT_EQ(kPsm, psm);
    service_request_cb_called = true;
    return DynamicChannelRegistry::ServiceInfo{ChannelParameters(), cb.share()};
  };

  TestDynamicChannelRegistry registry(DoNothing, std::move(service_request_cb));

  registry.RequestService(kPsm, registry.FindAvailableChannelId(), kRemoteCId);
  EXPECT_TRUE(service_request_cb_called);
  ASSERT_TRUE(registry.last_channel());
  registry.last_channel()->DoOpen();

  EXPECT_TRUE(open_result_cb_called);
  EXPECT_NE(kInvalidChannelId, local_cid);
  EXPECT_NE(kInvalidChannelId, remote_cid);
  EXPECT_TRUE(registry.FindChannelByLocalId(local_cid));

  bool close_cb_called = false;
  registry.CloseChannel(local_cid, [&] { close_cb_called = true; });
  EXPECT_TRUE(close_cb_called);
  EXPECT_FALSE(registry.FindChannelByLocalId(local_cid));
}

TEST(DynamicChannelRegistryTest, AcceptServiceRequestThenOpenFails) {
  bool open_result_cb_called = false;
  DynamicChannelRegistry::DynamicChannelCallback open_result_cb =
      [&open_result_cb_called](const DynamicChannel*) {
        open_result_cb_called = true;
      };

  bool service_request_cb_called = false;
  auto service_request_cb = [&service_request_cb_called,
                             cb = std::move(open_result_cb)](Psm psm) mutable {
    EXPECT_FALSE(service_request_cb_called);
    EXPECT_EQ(kPsm, psm);
    service_request_cb_called = true;
    return DynamicChannelRegistry::ServiceInfo{ChannelParameters(), cb.share()};
  };

  TestDynamicChannelRegistry registry(DoNothing, std::move(service_request_cb));

  ChannelId local_cid = registry.FindAvailableChannelId();
  EXPECT_NE(kInvalidChannelId, local_cid);
  registry.RequestService(kPsm, local_cid, kRemoteCId);
  EXPECT_TRUE(service_request_cb_called);
  ASSERT_TRUE(registry.last_channel());
  registry.last_channel()->DoOpen(false);

  // Don't get channels that failed to open.
  EXPECT_FALSE(open_result_cb_called);
  EXPECT_FALSE(registry.FindChannelByLocalId(local_cid));

  // The channel should be released upon this failure.
  EXPECT_EQ(0u, registry.AliveChannelCount());
}

TEST(DynamicChannelRegistryTest,
     DestroyRegistryWithOpenChannelNoDisconnectionRequest) {
  bool close_cb_called = false;
  auto close_cb = [&close_cb_called](const DynamicChannel*) {
    close_cb_called = true;
  };

  TestDynamicChannelRegistry registry(std::move(close_cb), RejectAllServices);

  bool open_result_cb_called = false;
  auto open_result_cb = [&open_result_cb_called](const DynamicChannel* chan) {
    EXPECT_FALSE(open_result_cb_called);
    open_result_cb_called = true;
    EXPECT_TRUE(chan);
  };

  registry.OpenOutbound(kPsm, kChannelParams, std::move(open_result_cb));
  registry.last_channel()->DoConnect(kRemoteCId);
  registry.last_channel()->DoOpen();

  EXPECT_TRUE(open_result_cb_called);
  EXPECT_TRUE(registry.FindChannelByRemoteId(kRemoteCId));
  EXPECT_FALSE(close_cb_called);
}

TEST(DynamicChannelRegistryTest, ErrorConnectingChannel) {
  bool open_result_cb_called = false;
  auto open_result_cb = [&open_result_cb_called](const DynamicChannel* chan) {
    EXPECT_FALSE(open_result_cb_called);
    open_result_cb_called = true;
    EXPECT_FALSE(chan);
  };
  bool close_cb_called = false;
  auto close_cb = [&close_cb_called](auto) { close_cb_called = true; };

  TestDynamicChannelRegistry registry(std::move(close_cb), RejectAllServices);

  registry.OpenOutbound(kPsm, kChannelParams, std::move(open_result_cb));
  registry.last_channel()->DoOpen(false);

  EXPECT_TRUE(open_result_cb_called);
  EXPECT_FALSE(close_cb_called);

  // Should be no alive channels anymore.
  EXPECT_EQ(0u, registry.AliveChannelCount());
}

TEST(DynamicChannelRegistryTest, ExhaustedChannelIds) {
  int open_result_cb_count = 0;

  // This callback expects the channel to be creatable.
  DynamicChannelRegistry::DynamicChannelCallback success_open_result_cb =
      [&open_result_cb_count](const DynamicChannel* chan) {
        ASSERT_NE(nullptr, chan);
        EXPECT_NE(kInvalidChannelId, chan->local_cid());
        open_result_cb_count++;
      };

  int close_cb_count = 0;
  auto close_cb = [&close_cb_count](auto) { close_cb_count++; };

  TestDynamicChannelRegistry registry(std::move(close_cb), RejectAllServices);

  // Open a lot of channels.
  for (ChannelId i = 0; i < kNumChannelsAllowed; i++) {
    registry.OpenOutbound(
        kPsm + i, kChannelParams, success_open_result_cb.share());
    registry.last_channel()->DoConnect(kRemoteCId + i);
    registry.last_channel()->DoOpen();
  }
  EXPECT_EQ(kNumChannelsAllowed, open_result_cb_count);
  EXPECT_EQ(0, close_cb_count);

  // Ensure that channel IDs are exhausted.
  EXPECT_EQ(kInvalidChannelId, registry.FindAvailableChannelId());

  // This callback expects the channel to fail creation.
  auto fail_open_result_cb =
      [&open_result_cb_count](const DynamicChannel* chan) {
        EXPECT_FALSE(chan);
        open_result_cb_count++;
      };

  // Try to open a new channel.
  registry.OpenOutbound(kPsm, kChannelParams, std::move(fail_open_result_cb));
  EXPECT_EQ(kNumChannelsAllowed + 1, open_result_cb_count);
  EXPECT_EQ(0, close_cb_count);

  // Close the most recently opened channel.
  auto last_remote_cid = registry.last_channel()->remote_cid();
  registry.last_channel()->DoRemoteClose();
  EXPECT_EQ(1, close_cb_count);
  EXPECT_NE(kInvalidChannelId, registry.FindAvailableChannelId());

  // Try to open a channel again.
  registry.OpenOutbound(kPsm, kChannelParams, success_open_result_cb.share());
  registry.last_channel()->DoConnect(last_remote_cid);
  registry.last_channel()->DoOpen();
  EXPECT_EQ(kNumChannelsAllowed + 2, open_result_cb_count);
  EXPECT_EQ(1, close_cb_count);

  ChannelId last_local_cid = registry.last_channel()->local_cid();

  int close_cb_called = 0;
  for (ChannelId i = 0; i < kNumChannelsAllowed; i++) {
    registry.CloseChannel(kFirstDynamicChannelId + i,
                          [&] { close_cb_called++; });
  }
  EXPECT_EQ(close_cb_called, kNumChannelsAllowed);
  EXPECT_FALSE(registry.FindChannelByLocalId(last_local_cid));
}

TEST(DynamicChannelRegistryTest,
     ChannelIdNotReusedUntilDisconnectionCompletes) {
  TestDynamicChannelRegistry registry(DoNothing, RejectAllServices);

  // This callback expects the channel to be creatable.
  int open_result_cb_count = 0;
  DynamicChannelRegistry::DynamicChannelCallback success_open_result_cb =
      [&](const DynamicChannel* chan) {
        ASSERT_NE(nullptr, chan);
        EXPECT_NE(kInvalidChannelId, chan->local_cid());
        open_result_cb_count++;
      };

  // Open all but one of the available channels.
  for (ChannelId i = 0; i < kNumChannelsAllowed - 1; i++) {
    registry.OpenOutbound(
        kPsm + i, kChannelParams, success_open_result_cb.share());
    registry.last_channel()->DoConnect(kRemoteCId + i);
    registry.last_channel()->DoOpen();
  }
  EXPECT_EQ(kNumChannelsAllowed - 1, open_result_cb_count);

  // This callback records the info on channel that was created
  ChannelId last_local_cid = kInvalidChannelId;
  auto record_open_result_cb = [&](const DynamicChannel* chan) {
    ASSERT_TRUE(chan);
    last_local_cid = chan->local_cid();
  };

  // Ensure that channel IDs are not exhausted.
  EXPECT_NE(kInvalidChannelId, registry.FindAvailableChannelId());

  // Open a the last available channel.
  registry.OpenOutbound(kPsm, kChannelParams, std::move(record_open_result_cb));

  registry.last_channel()->DoConnect(kRemoteCId + kNumChannelsAllowed - 1);
  registry.last_channel()->DoOpen();

  EXPECT_NE(kInvalidChannelId, last_local_cid);
  ASSERT_TRUE(registry.FindChannelByLocalId(last_local_cid));

  // The channels are exhausted now.
  ASSERT_EQ(kInvalidChannelId, registry.FindAvailableChannelId());

  // Close the channel but don't let the disconnection complete.
  FakeDynamicChannel* const last_channel = registry.last_channel();
  last_channel->set_defer_disconnect_done_callback();
  int close_cb_called = 0;
  registry.CloseChannel(last_local_cid, [&] { close_cb_called++; });

  // New channels should not reuse the "mostly disconnected" channel's ID.
  EXPECT_EQ(close_cb_called, 0);
  // There should still be no channels left.
  EXPECT_EQ(kInvalidChannelId, registry.FindAvailableChannelId());
  ASSERT_TRUE(registry.FindChannelByLocalId(last_local_cid));
  EXPECT_FALSE(registry.FindChannelByLocalId(last_local_cid)->IsConnected());

  // Complete the disconnection for the first channel opened.
  ASSERT_TRUE(last_channel->disconnect_done_callback());
  last_channel->disconnect_done_callback()();
  EXPECT_EQ(close_cb_called, 1);
  EXPECT_EQ(last_local_cid, registry.FindAvailableChannelId());

  // Open a new channel and make sure that last ID can be reused now.
  bool open_result_cb_called = false;
  auto open_result_cb = [&](const DynamicChannel* chan) {
    EXPECT_FALSE(open_result_cb_called);
    open_result_cb_called = true;
    ASSERT_TRUE(chan);
    EXPECT_EQ(last_local_cid, chan->local_cid());
  };
  registry.OpenOutbound(kPsm, kChannelParams, std::move(open_result_cb));
  registry.last_channel()->DoConnect(kRemoteCId + kNumChannelsAllowed - 1);
  registry.last_channel()->DoOpen();
  EXPECT_TRUE(open_result_cb_called);

  close_cb_called = 0;
  for (ChannelId i = 0; i < kNumChannelsAllowed; i++) {
    registry.CloseChannel(kFirstDynamicChannelId + i,
                          [&] { close_cb_called++; });
  }
  EXPECT_EQ(close_cb_called, kNumChannelsAllowed);
  EXPECT_FALSE(registry.FindChannelByLocalId(last_local_cid));
}

// Removing a channel from the channel map while iterating the channels in
// ForEach should not cause a use-after-free of the invalidated pointer.
TEST(DynamicChannelRegistryTest, CloseChannelInForEachCallback) {
  bool registry_close_cb_called = false;
  auto registry_close_cb = [&](const DynamicChannel*) {
    registry_close_cb_called = true;
  };

  TestDynamicChannelRegistry registry(std::move(registry_close_cb),
                                      RejectAllServices);

  bool open_result_cb_called = false;
  ChannelId local_cid = kInvalidChannelId;
  auto open_result_cb = [&](const DynamicChannel* chan) {
    EXPECT_FALSE(open_result_cb_called);
    open_result_cb_called = true;
    EXPECT_TRUE(chan);
    local_cid = chan->local_cid();
  };

  registry.OpenOutbound(kPsm, kChannelParams, std::move(open_result_cb));
  registry.last_channel()->DoConnect(kRemoteCId);
  registry.last_channel()->DoOpen();

  EXPECT_TRUE(open_result_cb_called);
  EXPECT_TRUE(registry.FindChannelByLocalId(local_cid));

  // Even if the next iterator is "end", it would still be unsafe to advance the
  // erased iterator.
  registry.ForEach([&](DynamicChannel* chan) {
    registry.CloseChannel(chan->local_cid(), [] {});
  });

  EXPECT_FALSE(registry.FindChannelByLocalId(local_cid));
}

}  // namespace
}  // namespace bt::l2cap::internal
