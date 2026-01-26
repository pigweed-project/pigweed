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
#include <lib/fit/function.h>

#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "pw_bluetooth/vendor.h"
#include "pw_bluetooth_sapphire/internal/host/common/inspectable.h"
#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/a2dp_offload_manager.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/autosniff.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/bredr_command_handler.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/channel.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/channel_manager.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/dynamic_channel_registry.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/fragmenter.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/l2cap_defs.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/low_energy_command_handler.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/recombiner.h"
#include "pw_bluetooth_sapphire/internal/host/transport/acl_data_packet.h"
#include "pw_bluetooth_sapphire/internal/host/transport/command_channel.h"
#include "pw_bluetooth_sapphire/internal/host/transport/link_type.h"

namespace bt::l2cap::internal {

class ChannelImpl;
class LESignalingChannel;
class SignalingChannel;

// Represents a controller logical link. Each instance aids in mapping L2CAP
// channels to their corresponding controller logical link and vice versa. This
// owns each link's signaling fixed channel and the dynamic channel logic that
// operates on that channel. A LogicalLink must be explicitly `Close`d before
// destruction, and will assert on this behavior in its destructor.
//
// Instances are created and owned by a ChannelManager.
class LogicalLink : public hci::AclDataChannel::ConnectionInterface {
 public:
  // Returns a function that accepts opened channels for a registered local
  // service identified by |psm| on a given connection identified by |handle|,
  // or nullptr if there is no service registered for that PSM.
  using QueryServiceCallback =
      fit::function<std::optional<ChannelManager::ServiceInfo>(
          hci_spec::ConnectionHandle handle, Psm psm)>;

  // Constructs a new LogicalLink and initializes the signaling fixed channel.
  // |max_payload_size| shall be the maximum "host to controller" data packet
  // payload size for the link |type|, per Core Spec v5.0 Vol 2, Part E,
  // Sec 4.1. Both |query_service_cb| and the inbound channel delivery callback
  // that it returns will be executed on this object's creation thread. If
  // |random_channel_ids| is true, assign dynamic channels randomly instead of
  // starting at the beginning of the dynamic channel range.
  // |a2dp_offload_manager| is a reference to A2dpOffloadManager that is passed
  // to channels to use
  LogicalLink(hci_spec::ConnectionHandle handle,
              bt::LinkType type,
              pw::bluetooth::emboss::ConnectionRole role,
              uint16_t max_acl_payload_size,
              QueryServiceCallback query_service_cb,
              hci::AclDataChannel* acl_data_channel,
              hci::CommandChannel* cmd_channel,
              bool random_channel_ids,
              A2dpOffloadManager& a2dp_offload_manager,
              pw::async::Dispatcher& dispatcher,
              pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider);

  // When a logical link is destroyed it notifies all of its channels to close
  // themselves. Data packets will no longer be routed to the associated
  // channels.
  ~LogicalLink() override;

  // Notifies and closes all open channels on this link. This must be called to
  // cleanly shut down a LogicalLink.
  //
  // The link MUST not be closed when this is called.
  void Close();

  // Opens the channel with |channel_id| over this logical link synchronously,
  // regardless of peer support. See channel.h for documentation on
  // |rx_callback| and |closed_callback|. Returns nullptr if a Channel for
  // |channel_id| already exists.
  //
  // The link MUST not be closed when this is called.
  Channel::WeakPtr OpenFixedChannel(ChannelId channel_id);

  // Same as |OpenFixedChannel|, but waits for interrogation of the peer to
  // complete before returning the channel asynchronously via |callback|. The
  // channel will be null if it is not supported by the peer.
  void OpenFixedChannelAsync(ChannelId channel_id, ChannelCallback callback);

  // Opens a dynamic channel to the requested |psm| with the preferred
  // parameters |params| and returns a channel asynchronously via |callback|.
  //
  // The link MUST not be closed when this is called.
  void OpenChannel(Psm psm, ChannelParameters params, ChannelCallback callback);

  // Takes ownership of |packet| for PDU processing and routes it to its target
  // channel. This must be called on this object's creation thread.
  //
  // The link MUST not be closed when this is called.
  void HandleRxPacket(hci::ACLDataPacketPtr packet);

  // Called by l2cap::Channel when a packet is available.
  void OnOutboundPacketAvailable();

  // Requests a security upgrade using the registered security upgrade callback.
  // Invokes the |callback| argument with the result of the operation.
  //
  // Has no effect if the link is closed.
  void UpgradeSecurity(sm::SecurityLevel level, sm::ResultFunction<> callback);

  // Assigns the security level of this link and resolves pending security
  // upgrade requests. Has no effect if the link is closed.
  void AssignSecurityProperties(const sm::SecurityProperties& security);

  // Send a Connection Parameter Update Request on the LE signaling channel.
  // When the Connection Parameter Update Response is received, |request_cb|
  // will be called with the result, |accepted|. NOTE: the local Host must be an
  // LE peripheral.
  void SendConnectionParameterUpdateRequest(
      hci_spec::LEPreferredConnectionParameters params,
      ConnectionParameterUpdateRequestCallback request_cb);

  // Request a change of this link's ACL priority to |priority|.
  // |channel| must indicate the channel making the request, which must be
  // alive. |callback| will be called with the result of the request. The
  // request will fail if |priority| conflicts with another channel's priority
  // or the controller does not support changing the ACL priority.
  //
  // Requests are queued and handled sequentially in order to prevent race
  // conditions.
  void RequestAclPriority(
      Channel::WeakPtr channel,
      pw::bluetooth::AclPriority priority,
      fit::callback<void(fit::result<fit::failed>)> callback);

  // Sets an automatic flush timeout with duration |flush_timeout|. |callback|
  // will be called with the result of the operation. This is only supported if
  // the link type is kACL (BR/EDR). |flush_timeout| must be in the range [1ms -
  // hci_spec::kMaxAutomaticFlushTimeoutDuration]. A flush timeout of
  // pw::chrono::SystemClock::duration::max() indicates an infinite flush
  // timeout (no automatic flush), the default.
  void SetBrEdrAutomaticFlushTimeout(
      pw::chrono::SystemClock::duration flush_timeout,
      hci::ResultCallback<> callback);

  // Attach LogicalLink's inspect node as a child of |parent| with the given
  // |name|.
  void AttachInspect(inspect::Node& parent, std::string name);

  // Assigns the link error callback to be invoked when a channel signals a link
  // error.
  void set_error_callback(fit::closure callback);

  // Assigns the security upgrade delegate for this link.
  void set_security_upgrade_callback(SecurityUpgradeCallback callback);

  // Assigns the callback to be invoked when a valid Connection Parameter Update
  // Request is received on the signaling channel.
  void set_connection_parameter_update_callback(
      LEConnectionParameterUpdateCallback callback);

  const sm::SecurityProperties security() { return security_; }

  using WeakPtr = WeakSelf<LogicalLink>::WeakPtr;
  WeakPtr GetWeakPtr() { return weak_self_.GetWeakPtr(); }

  // ConnectionInterface overrides:
  hci_spec::ConnectionHandle handle() const override { return handle_; }
  bt::LinkType type() const override { return type_; }
  std::unique_ptr<hci::ACLDataPacket> GetNextOutboundPacket() override;
  bool HasAvailablePacket() const override;

  // Called by ChannelImpl::OnRxPacket() to return credits after the associated
  // packet has been handled.
  void SignalCreditsAvailable(ChannelId channel, uint16_t credits);

  // Returns true if autosniff is enabled
  bool AutosniffEnabled() const;

  // Returns true if autosniff is disabled or suppressed.
  bool AutosniffIsSuppressed() const;

  // Returns the current connection mode as per autosniff. This will always
  // return ACTIVE if autosniff is not enabled.
  pw::bluetooth::emboss::AclConnectionMode AutosniffMode() const;

  std::unique_ptr<AutosniffSuppressInterest> SuppressAutosniff(
      const char* reason);

  // Duration to wait without events before switching into sniff mode.
  static constexpr pw::chrono::SystemClock::duration kAutosniffTimeout =
      std::chrono::seconds(1);

 private:
  friend class ChannelImpl;

  // Returns true if |id| is valid and supported by the peer.
  bool AllowsFixedChannel(ChannelId id);

  // Called by ChannelImpl::Deactivate(). Removes the channel from the given
  // link. Calls |removed_cb| when the channel no longer exists.
  void RemoveChannel(Channel* chan, fit::closure removed_cb);

  // Called by ChannelImpl::SignalLinkError() to disconnect all channels then
  // signal an error to the lower layers (usually GAP, to request a link
  // disconnection). Has no effect if the link is closed.
  void SignalError();

  // If the service identified by |psm| can be opened, return a function to
  // complete the channel open for a newly-opened DynamicChannel. Otherwise,
  // return nullptr.
  //
  // This MUST not be called on a closed link.
  std::optional<DynamicChannelRegistry::ServiceInfo> OnServiceRequest(Psm psm);

  // Called by |dynamic_registry_| when the peer requests the closure of a
  // dynamic channel using a signaling PDU.
  //
  // This MUST not be called on a closed link.
  void OnChannelDisconnectRequest(const DynamicChannel* dyn_chan);

  // Given a newly-opened dynamic channel as reported by this link's
  // DynamicChannelRegistry, create a ChannelImpl for it to carry user data,
  // then pass a pointer to it through |open_cb|. If |dyn_chan| is null, then
  // pass nullptr into |open_cb|.
  //
  // This MUST not be called on a closed link.
  void CompleteDynamicOpen(const DynamicChannel* dyn_chan,
                           ChannelCallback open_cb);

  // Send an Information Request signaling packet of type Fixed Channels
  // Supported.
  void SendFixedChannelsSupportedInformationRequest();

  // Handler for Information Response signaling packet. This is used to handle
  // the Fixed Channels Supported information response, which indicates which
  // fixed channels the peer supports (Core Spec v5.1, Vol 3, Part A, Sec 4.13).
  // Except for the signaling channels, fixed channels may not be created until
  // this response has been received.
  // TODO(fxbug.dev/42119997): save fixed channels mask and use to verify opened
  // fixed channel ids are supported
  void OnRxFixedChannelsSupportedInfoRsp(
      const BrEdrCommandHandler::InformationResponse& rsp);

  // Start serving Connection Parameter Update Requests on the LE signaling
  // channel.
  void ServeConnectionParameterUpdateRequest();

  // Handler called when a Connection Parameter Update Request is received on
  // the LE signaling channel.
  void OnRxConnectionParameterUpdateRequest(
      uint16_t interval_min,
      uint16_t interval_max,
      uint16_t peripheral_latency,
      uint16_t timeout_multiplier,
      LowEnergyCommandHandler::ConnectionParameterUpdateResponder* responder);

  void ServeFlowControlCreditInd();
  void OnRxFlowControlCreditInd(ChannelId remote_cid, uint16_t credits);

  // Processes the next ACL priority request in the  |pending_acl_requests_|
  // queue. In order to optimize radio performance, ACL priority is downgraded
  // whenever possible (i.e. when no more channels are requesting high
  // priority).
  void HandleNextAclPriorityRequest();

  // Return true if |current_channel_|'s next packet is part of a PDU that is
  // already in the process of being sent
  bool IsNextPacketContinuingFragment() const;

  // Round robins through channels in logical link to get next packet to send
  // Returns nullptr if there are no connections with pending packets
  void RoundRobinChannels();

  pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider_;

  pw::async::Dispatcher& pw_dispatcher_;

  sm::SecurityProperties security_;

  // Information about the underlying controller logical link.
  hci_spec::ConnectionHandle handle_;
  bt::LinkType type_;
  pw::bluetooth::emboss::ConnectionRole role_;

  // Maximum number of bytes that should be allowed in a ACL data packet,
  // excluding the header
  uint16_t max_acl_payload_size_;

  // The duration after which BR/EDR packets are flushed from the controller.
  // By default, the flush timeout is pw::chrono::SystemClock::duration::max()
  // (no automatic flush).
  UintInspectable<pw::chrono::SystemClock::duration> flush_timeout_;

  fit::closure link_error_cb_;

  SecurityUpgradeCallback security_callback_;

  LEConnectionParameterUpdateCallback connection_parameter_update_callback_;

  // No data packets are processed once this gets set to true.
  bool closed_;

  // Recombiner is always accessed on the L2CAP thread.
  Recombiner recombiner_;

  // Channels that were created on this link. Channels notify the link for
  // removal when deactivated.
  using ChannelMap =
      std::unordered_map<ChannelId, std::unique_ptr<ChannelImpl>>;
  ChannelMap channels_;

  // Round robin iterator for sending packets from channels
  ChannelMap::iterator current_channel_;

  // Channel that Logical Link is currently sending PDUs from
  ChannelImpl::WeakPtr current_pdus_channel_;

  // Manages the L2CAP signaling channel on this logical link. Depending on
  // |type_| this will either implement the LE or BR/EDR signaling commands.
  std::unique_ptr<SignalingChannel> signaling_channel_;

  std::optional<FixedChannelsSupported> fixed_channels_supported_;
  std::unordered_map<ChannelId, fit::closure>
      fixed_channels_supported_callbacks_;

  // Stores packets that have been received on a currently closed channel. We
  // buffer these for fixed channels so that the data is available when the
  // channel is opened.
  using PendingPduMap = std::unordered_map<ChannelId, std::list<PDU>>;
  PendingPduMap pending_pdus_;

  struct PendingAclRequest {
    Channel::WeakPtr channel;
    pw::bluetooth::AclPriority priority;
    fit::callback<void(fit::result<fit::failed>)> callback;
  };
  std::queue<PendingAclRequest> pending_acl_requests_;

  // The current ACL priority of this link.
  pw::bluetooth::AclPriority acl_priority_ =
      pw::bluetooth::AclPriority::kNormal;

  // Dynamic channels opened with the remote. The registry is destroyed and all
  // procedures terminated when this link gets closed.
  std::unique_ptr<DynamicChannelRegistry> dynamic_registry_;

  hci::AclDataChannel* acl_data_channel_;
  hci::CommandChannel* cmd_channel_;

  // Search function for inbound service requests. Returns handler that accepts
  // opened channels.
  QueryServiceCallback query_service_cb_;

  // Automatically toggles the connection between sniff mode and active.
  std::optional<Autosniff> autosniff_;

  struct InspectProperties {
    inspect::Node node;
    inspect::Node channels_node;
    inspect::Node sniff_node;
    inspect::StringProperty handle;
    inspect::StringProperty link_type;
  };
  InspectProperties inspect_properties_;

  A2dpOffloadManager& a2dp_offload_manager_;

  WeakSelf<hci::AclDataChannel::ConnectionInterface> weak_conn_interface_;
  WeakSelf<LogicalLink> weak_self_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(LogicalLink);
};

}  // namespace bt::l2cap::internal
