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
#include <memory>
#include <unordered_map>

#include "pw_bluetooth_sapphire/internal/host/common/inspect.h"
#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/channel.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/l2cap_defs.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/le_signaling_channel.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/types.h"
#include "pw_bluetooth_sapphire/internal/host/transport/acl_data_channel.h"
#include "pw_bluetooth_sapphire/internal/host/transport/acl_data_packet.h"
#include "pw_bluetooth_sapphire/internal/host/transport/link_type.h"

namespace bt::l2cap {

// ChannelManager implements the "Channel Manager" control block of L2CAP. In
// particular:
//
//   * It acts as a routing table for incoming ACL data by directing packets to
//     the appropriate internal logical link handler;
//
//   * Manages priority based scheduling.
//
//   * Provides an API surface for L2CAP channel creation and logical link
//     management bound to a single creation thread.
//
// There can be a single instance of ChannelManager for a HCI transport.
class ChannelManager {
 public:
  // Information stored and returned for registered services that is needed to
  // configure and forward new channels for this service.
  using ServiceInfo = ServiceInfo<ChannelCallback>;

  struct LEFixedChannels {
    l2cap::Channel::WeakPtr att;
    l2cap::Channel::WeakPtr smp;
  };

  struct BrEdrFixedChannels {
    l2cap::Channel::WeakPtr smp;
  };

  // Create a ChannelManager. FakeL2cap can be used instead in tests.
  // |wake_lease_provider| will be used to acquire wake leases, and must outlive
  // the returned ChannelManager.
  static std::unique_ptr<ChannelManager> Create(
      hci::AclDataChannel* acl_data_channel,
      hci::CommandChannel* cmd_channel,
      bool random_channel_ids,
      pw::async::Dispatcher& dispatcher,
      pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider);

  virtual ~ChannelManager() = default;

  // Attach ChannelManager's inspect node as a child of |parent| with the given
  // |name|
  static constexpr const char* kInspectNodeName = "l2cap";
  virtual void AttachInspect(inspect::Node& parent, std::string name) = 0;

  // Registers an ACL connection with the L2CAP layer. L2CAP channels can be
  // opened on the logical link represented by |handle| after a call to this
  // method. It is an error to register the same |handle| value more than once
  // as either kind of channel without first unregistering it.
  //
  // |link_error_callback| will be used to notify when a channel signals a link
  // error.
  //
  // |security_callback| will be used to request an upgrade to the link security
  // level. This can be triggered by dynamic L2CAP channel creation or by a
  // service-level client via Channel::UpgradeSecurity().
  //
  // |fixed_channels_callback| will be called with the fixed channels after
  // interrogating the peer. Channels not supported by the peer will be null.
  virtual void AddACLConnection(
      hci_spec::ConnectionHandle handle,
      pw::bluetooth::emboss::ConnectionRole role,
      l2cap::LinkErrorCallback link_error_callback,
      l2cap::SecurityUpgradeCallback security_callback,
      fit::callback<void(BrEdrFixedChannels)> fixed_channels_callback) = 0;

  // Registers an LE connection with the L2CAP layer. L2CAP channels can be
  // opened on the logical link represented by |handle| after a call to this
  // method.  It is an error to register the same |handle| value more than once
  // as either kind of channel without first unregistering it (asserted in debug
  // builds).
  //
  // |conn_param_callback| will be used to notify the caller if new connection
  // parameters were accepted from the remote end of the link.
  //
  // |link_error_callback| will be used to notify when a channel signals a link
  // error.
  //
  // |security_callback| will be used to request an upgrade to the link security
  // level. This can be triggered by dynamic L2CAP channel creation or by a
  // service-level client via Channel::UpgradeSecurity().
  //
  // Returns the ATT and SMP fixed channels of this link.
  [[nodiscard]] virtual LEFixedChannels AddLEConnection(
      hci_spec::ConnectionHandle handle,
      pw::bluetooth::emboss::ConnectionRole role,
      l2cap::LinkErrorCallback link_error_callback,
      l2cap::LEConnectionParameterUpdateCallback conn_param_callback,
      l2cap::SecurityUpgradeCallback security_callback) = 0;

  // Removes a previously registered connection. All corresponding Channels will
  // be closed and all incoming data packets on this link will be dropped.
  //
  // NOTE: It is recommended that a link entry be removed AFTER the controller
  // sends a HCI Disconnection Complete Event for the corresponding logical
  // link. This is to prevent incorrectly buffering data if the controller has
  // more packets to send after removing the link entry.
  virtual void RemoveConnection(hci_spec::ConnectionHandle handle) = 0;

  // Assigns the security level of a logical link. Has no effect if |handle| has
  // not been previously registered or the link is closed.
  virtual void AssignLinkSecurityProperties(
      hci_spec::ConnectionHandle handle, sm::SecurityProperties security) = 0;

  // Send an LE Connection Parameter Update Request requesting |params| on the
  // LE signaling channel of the LE connection represented by |handle|. This
  // should only be used if the LE peripheral and LE central do not support the
  // Connection Parameters Request Link Layer Control Procedure (Core Spec v5.2
  // Vol 3, Part A, Sec 4.20). This should only be called when the local host is
  // an LE peripheral.
  //
  // |request_cb| will be called when a response (acceptance or rejection) is
  // received.
  virtual void RequestConnectionParameterUpdate(
      hci_spec::ConnectionHandle handle,
      hci_spec::LEPreferredConnectionParameters params,
      l2cap::ConnectionParameterUpdateRequestCallback request_cb) = 0;

  // Opens the L2CAP fixed channel with |channel_id| over the logical link
  // identified by |connection_handle| and starts routing packets.
  //
  // Returns nullptr if the channel is already open.
  virtual Channel::WeakPtr OpenFixedChannel(
      hci_spec::ConnectionHandle connection_handle, ChannelId channel_id) = 0;

  // Open an outbound dynamic channel against a peer's Protocol/Service
  // Multiplexing (PSM) code |psm| on a link identified by |handle| using the
  // preferred channel parameters |params|. If the peer requires different
  // higher priority parameters, the local device will accept those instead.
  //
  // |cb| will be called with the channel created to the remote, or nullptr if
  // the channel creation resulted in an error.
  virtual void OpenL2capChannel(hci_spec::ConnectionHandle handle,
                                l2cap::Psm psm,
                                l2cap::ChannelParameters params,
                                l2cap::ChannelCallback cb) = 0;

  // Registers a handler for peer-initiated dynamic channel requests that have
  // the Protocol/Service Multiplexing (PSM) code |psm|. The local device will
  // attempt to configure these channels using the preferred parameters
  // |params|, but will accept different channel parameters required by the peer
  // if they are higher priority.
  //
  // |cb| will be called with the channel created by each inbound connection
  // request received. Handlers must be unregistered before they are replaced.
  //
  // Returns false if |psm| is invalid or already has a handler registered.
  //
  // Inbound connection requests with a PSM that has no registered handler will
  // be rejected.
  //
  // TODO(fxbug.dev/42181829): Dynamic PSMs may need their routing space (ACL or
  // LE) identified
  virtual bool RegisterService(l2cap::Psm psm,
                               l2cap::ChannelParameters params,
                               l2cap::ChannelCallback callback) = 0;

  // Removes the handler for inbound channel requests for the previously-
  // registered service identified by |psm|. This only prevents new inbound
  // channels from being opened but does not close already-open channels.
  virtual void UnregisterService(l2cap::Psm psm) = 0;

  // Returns a pointer to the internal LogicalLink with the corresponding link
  // |handle|, or nullptr if none exists. NOTE: This is intended ONLY for unit
  // tests. Clients should use the other public methods to interact with the
  // link.
  virtual WeakSelf<internal::LogicalLink>::WeakPtr LogicalLinkForTesting(
      hci_spec::ConnectionHandle handle) = 0;

  // Suppresses autosniff on the given connection |handle|. Returns an RAII
  // object that, upon destruction, will notify the autosniff manager that this
  // particular suppression is now gone. The |reason| is used for logging only
  // and must be a static-life C-string.
  virtual std::optional<std::unique_ptr<AutosniffSuppressInterface>>
  SuppressAutosniff(hci_spec::ConnectionHandle handle, const char* reason) = 0;
};

}  // namespace bt::l2cap
