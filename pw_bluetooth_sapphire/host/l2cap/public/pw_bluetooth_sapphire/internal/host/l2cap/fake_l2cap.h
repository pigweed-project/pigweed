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
#include "pw_bluetooth_sapphire/internal/host/l2cap/autosniff.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/channel.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/channel_manager.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/fake_channel.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/types.h"

namespace bt::l2cap::testing {

class FakeL2cap;

class FakeAutosniffSuppression : public AutosniffSuppressInterface {
 public:
  ~FakeAutosniffSuppression() override { Release(); }

  void Release() override;

 private:
  friend class FakeL2cap;

  FakeL2cap* l2cap_;
  hci_spec::ConnectionHandle handle_;

  FakeAutosniffSuppression(FakeL2cap* l2cap, hci_spec::ConnectionHandle handle)
      : l2cap_(l2cap), handle_(handle) {}
};

// This is a fake version of the ChannelManager class that can be injected into
// other layers for unit testing.
class FakeL2cap final : public ChannelManager {
 public:
  explicit FakeL2cap(pw::async::Dispatcher& pw_dispatcher)
      : heap_dispatcher_(pw_dispatcher) {}
  ~FakeL2cap() override;

  void AttachInspect(inspect::Node&, std::string) override {}

  // Returns true if and only if a link identified by |handle| has been added
  // and connected.
  [[nodiscard]] bool IsLinkConnected(hci_spec::ConnectionHandle handle) const;

  // Triggers a LE connection parameter update callback on the given link.
  void TriggerLEConnectionParameterUpdate(
      hci_spec::ConnectionHandle handle,
      const hci_spec::LEPreferredConnectionParameters& params);

  // Sets up the expectation that an outbound dynamic channel will be opened
  // on the given link. Each call will expect one dyanmic channel to be
  // created.  If a call to OpenL2capChannel is made without expectation, it
  // will assert.
  // Multiple expectations for the same PSM should be queued in FIFO order.
  void ExpectOutboundL2capChannel(hci_spec::ConnectionHandle handle,
                                  Psm psm,
                                  ChannelId id,
                                  ChannelId remote_id,
                                  ChannelParameters params);

  // Triggers the creation of an inbound dynamic channel on the given link. The
  // channels created will be provided to handlers passed to RegisterService.
  // Returns false if unable to create the channel.
  bool TriggerInboundL2capChannel(hci_spec::ConnectionHandle handle,
                                  Psm psm,
                                  ChannelId id,
                                  ChannelId remote_id,
                                  uint16_t tx_mtu = kDefaultMTU);

  // Triggers a link error callback on the given link.
  void TriggerLinkError(hci_spec::ConnectionHandle handle);

  // L2cap overrides:
  void AddACLConnection(
      hci_spec::ConnectionHandle handle,
      pw::bluetooth::emboss::ConnectionRole role,
      l2cap::LinkErrorCallback link_error_callback,
      l2cap::SecurityUpgradeCallback security_callback,
      fit::callback<void(BrEdrFixedChannels)> fixed_channels_callback) override;
  LEFixedChannels AddLEConnection(
      hci_spec::ConnectionHandle handle,
      pw::bluetooth::emboss::ConnectionRole role,
      LinkErrorCallback link_error_callback,
      LEConnectionParameterUpdateCallback conn_param_callback,
      SecurityUpgradeCallback security_callback) override;
  void RemoveConnection(hci_spec::ConnectionHandle handle) override;
  void AssignLinkSecurityProperties(hci_spec::ConnectionHandle handle,
                                    sm::SecurityProperties security) override;

  // Immediately posts accept on |dispatcher|.
  void RequestConnectionParameterUpdate(
      hci_spec::ConnectionHandle handle,
      hci_spec::LEPreferredConnectionParameters params,
      ConnectionParameterUpdateRequestCallback request_cb) override;

  Channel::WeakPtr OpenFixedChannel(hci_spec::ConnectionHandle,
                                    ChannelId) override {
    return Channel::WeakPtr();
  }
  void OpenL2capChannel(hci_spec::ConnectionHandle handle,
                        Psm psm,
                        ChannelParameters params,
                        ChannelCallback cb) override;
  bool RegisterService(Psm psm,
                       ChannelParameters params,
                       ChannelCallback channel_callback) override;
  void UnregisterService(Psm psm) override;

  WeakSelf<internal::LogicalLink>::WeakPtr LogicalLinkForTesting(
      hci_spec::ConnectionHandle) override {
    return WeakSelf<internal::LogicalLink>::WeakPtr();
  }

  std::optional<std::unique_ptr<AutosniffSuppressInterface>> SuppressAutosniff(
      hci_spec::ConnectionHandle handle, const char* /*reason*/) override {
    autosniff_suppressions_.insert(handle);
    return std::unique_ptr<AutosniffSuppressInterface>(
        new FakeAutosniffSuppression(this, handle));
  }

  bool AutosniffIsSuppressed(hci_spec::ConnectionHandle handle) {
    return autosniff_suppressions_.find(handle) !=
           autosniff_suppressions_.end();
  }

  // Called when a new channel gets opened. Tests can use this to obtain a
  // reference to all channels.
  using FakeChannelCallback =
      fit::function<void(testing::FakeChannel::WeakPtr)>;
  void set_channel_callback(FakeChannelCallback callback) {
    chan_cb_ = std::move(callback);
  }
  void set_simulate_open_channel_failure(bool simulate_failure) {
    simulate_open_channel_failure_ = simulate_failure;
  }

  // Called when RequestConnectionParameterUpdate is called. |request_cb| will
  // be called with return value. Defaults to returning true if not set.
  using ConnectionParameterUpdateRequestResponder = fit::function<bool(
      hci_spec::ConnectionHandle, hci_spec::LEPreferredConnectionParameters)>;
  void set_connection_parameter_update_request_responder(
      ConnectionParameterUpdateRequestResponder responder) {
    connection_parameter_update_request_responder_ = std::move(responder);
  }

 private:
  // TODO(armansito): Consider moving the following logic into an internal fake
  // that is L2CAP-specific.
  struct ChannelData {
    ChannelId local_id;
    ChannelId remote_id;
    ChannelParameters params;
  };
  struct LinkData {
    // Expectations on links can be created before they are connected.
    bool connected;
    hci_spec::ConnectionHandle handle;
    pw::bluetooth::emboss::ConnectionRole role;
    bt::LinkType type;
    bool link_error_signaled = false;

    // Dual-mode callbacks
    LinkErrorCallback link_error_cb;
    std::unordered_map<Psm, std::queue<ChannelData>> expected_outbound_conns;

    // LE-only callbacks
    LEConnectionParameterUpdateCallback le_conn_param_cb;

    std::unordered_map<ChannelId, std::unique_ptr<FakeChannel>> channels_;
  };

  LinkData* RegisterInternal(hci_spec::ConnectionHandle handle,
                             pw::bluetooth::emboss::ConnectionRole role,
                             bt::LinkType link_type,
                             LinkErrorCallback link_error_callback);

  testing::FakeChannel::WeakPtr OpenFakeChannel(
      LinkData* link,
      ChannelId id,
      ChannelId remote_id,
      ChannelInfo info = ChannelInfo::MakeBasicMode(kDefaultMTU, kDefaultMTU));
  testing::FakeChannel::WeakPtr OpenFakeFixedChannel(LinkData* link,
                                                     ChannelId id);

  // Gets the link data for |handle|, creating it if necessary.
  LinkData& GetLinkData(hci_spec::ConnectionHandle handle);
  // Gets the link data for |handle|. Asserts if the link is not connected yet.
  LinkData& ConnectedLinkData(hci_spec::ConnectionHandle handle);

  std::unordered_map<hci_spec::ConnectionHandle, LinkData> links_;
  FakeChannelCallback chan_cb_;
  bool simulate_open_channel_failure_ = false;

  ConnectionParameterUpdateRequestResponder
      connection_parameter_update_request_responder_;

  std::unordered_map<Psm, ServiceInfo> registered_services_;

  pw::async::HeapDispatcher heap_dispatcher_;

  std::unordered_multiset<hci_spec::ConnectionHandle> autosniff_suppressions_;

  friend class FakeAutosniffSuppression;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(FakeL2cap);
};

}  // namespace bt::l2cap::testing
