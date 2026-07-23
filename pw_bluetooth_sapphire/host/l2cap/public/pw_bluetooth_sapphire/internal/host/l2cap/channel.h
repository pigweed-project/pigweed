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

#include <atomic>
#include <climits>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <queue>

#include "pw_bluetooth/vendor.h"
#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/common/inspect.h"
#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_bluetooth_sapphire/internal/host/common/pipeline_monitor.h"
#include "pw_bluetooth_sapphire/internal/host/common/weak_self.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/a2dp_offload_manager.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/fragmenter.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/l2cap_defs.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/pdu.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/rx_engine.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/tx_engine.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/types.h"
#include "pw_bluetooth_sapphire/internal/host/sm/error.h"
#include "pw_bluetooth_sapphire/internal/host/sm/types.h"
#include "pw_bluetooth_sapphire/internal/host/transport/acl_data_channel.h"
#include "pw_bluetooth_sapphire/internal/host/transport/acl_data_packet.h"
#include "pw_bluetooth_sapphire/internal/host/transport/command_channel.h"
#include "pw_bluetooth_sapphire/internal/host/transport/error.h"
#include "pw_bluetooth_sapphire/internal/host/transport/link_type.h"
#include "pw_bluetooth_sapphire/lease.h"

namespace bt::l2cap {

// Maximum count of packets a channel can queue before it must drop old packets
inline constexpr uint16_t kDefaultTxMaxQueuedCount = 500;

// Represents a L2CAP channel. Each instance is owned by a service
// implementation that operates on the corresponding channel. Instances can only
// be obtained from a ChannelManager.
//
// A Channel can operate in one of 6 L2CAP Modes of Operation (see Core Spec
// v5.0, Vol 3, Part A, Section 2.4). Only Basic Mode is currently supported.
//
// USAGE:
//
// Channel is an abstract base class. There are two concrete implementations:
//
//   * internal::ChannelImpl (defined below) which implements a real L2CAP
//     channel. Instances are obtained from ChannelManager and tied to
//     internal::LogicalLink instances.
//
//   * FakeChannel, which can be used for unit testing service-layer entities
//     that operate on one or more L2CAP channel(s).
//
// Production instances are obtained from a ChannelManager. Channels are not
// thread safe.
//
// A Channel's owner must explicitly call Deactivate() and must not rely on
// dropping its reference to close the channel.
//
// When a LogicalLink closes, all of its active channels become deactivated
// when it closes and this is signaled by running the ClosedCallback passed to
// Activate().
class Channel : public WeakSelf<Channel> {
 public:
  // TODO(fxbug.dev/42053109): define a preferred MTU somewhere
  Channel(ChannelId id,
          ChannelId remote_id,
          bt::LinkType link_type,
          hci_spec::ConnectionHandle link_handle,
          ChannelInfo info,
          uint16_t max_tx_queued);
  virtual ~Channel() = default;

  // Identifier for this channel's endpoint on this device. It can be prior-
  // specified for fixed channels or allocated for dynamic channels per v5.0,
  // Vol 3, Part A, Section 2.1 "Channel Identifiers." Channels on a link will
  // have unique identifiers to each other.
  ChannelId id() const { return id_; }

  // Identifier for this channel's endpoint on the remote peer. Same value as
  // |id()| for fixed channels and allocated by the remote for dynamic channels.
  ChannelId remote_id() const { return remote_id_; }

  // The type of the logical link this channel operates on.
  bt::LinkType link_type() const { return link_type_; }

  // The connection handle of the underlying logical link.
  hci_spec::ConnectionHandle link_handle() const { return link_handle_; }

  // Returns a value that's unique for any channel connected to this device.
  // If two channels have different unique_ids, they represent different
  // channels even if their ids match.
  using UniqueId = uint32_t;
  UniqueId unique_id() const {
    static_assert(sizeof(UniqueId) >=
                      sizeof(hci_spec::ConnectionHandle) + sizeof(ChannelId),
                  "UniqueId needs to be large enough to make unique IDs");
    return (static_cast<UniqueId>(link_handle())
            << (sizeof(ChannelId) * CHAR_BIT)) |
           id();
  }

  const AnyChannelMode& mode() const { return info().mode; }

  // These accessors define the concept of a Maximum Transmission Unit (MTU) as
  // a maximum inbound (rx) and outbound (tx) packet size for the L2CAP
  // implementation (see v5.2, Vol. 3, Part A 5.1). L2CAP requires that channel
  // MTUs are at least 23 bytes for LE-U links and 48 bytes for ACL-U links. A
  // further requirement is that "the minimum MTU for a channel is the larger
  // of the L2CAP minimum [...] and any MTU explicitly required by the protocols
  // and profiles using that channel." `max_rx_sdu_size` is always determined by
  // the capabilities of the local implementation. For dynamic channels,
  // `max_tx_sdu_size` is determined through a configuration procedure with the
  // peer (v5.2 Vol. 3 Part A 7.1). For fixed channels, this is always the
  // maximum allowable L2CAP packet size, not a protocol-specific MTU.
  uint16_t max_rx_sdu_size() const { return info().max_rx_sdu_size; }
  uint16_t max_tx_sdu_size() const { return info().max_tx_sdu_size; }

  // Returns the current configuration parameters for this channel.
  const ChannelInfo& info() const { return info_; }

  uint16_t max_tx_queued() const { return max_tx_queued_; }
  void set_max_tx_queued(uint16_t count) { max_tx_queued_ = count; }

  // Returns the current link security properties of the underlying link.
  // Returns the lowest security level if the link is closed.
  virtual const sm::SecurityProperties security() = 0;

  // Callback invoked when this channel has been closed without an explicit
  // request from the owner of this instance. For example, this can happen when
  // the remote end closes a dynamically configured channel or when the
  // underlying logical link is terminated through other means.
  using ClosedCallback = fit::closure;

  // Callback invoked when a new packet is received on this channel. Any
  // previously buffered packets will be sent to |rx_cb| right away, provided
  // that |rx_cb| is not empty and the underlying logical link is active.
  using RxCallback = fit::function<void(ByteBufferPtr packet)>;

  // Activates this channel to execute |rx_callback| and |closed_callback|
  // immediately as L2CAP is notified of their underlying events.
  //
  // Any inbound data that has already been buffered for this channel will be
  // drained by calling |rx_callback| repeatedly, before this call returns.
  //
  // Execution of |rx_callback| may block L2CAP data routing, so care should be
  // taken to avoid introducing excessive latency.
  //
  // Each channel can be activated only once.
  //
  // Returns false if the channel's link has been closed.
  //
  // NOTE: Callers shouldn't assume that this method will succeed, as the
  // underlying link can be removed at any time.
  virtual bool Activate(RxCallback rx_callback,
                        ClosedCallback closed_callback) = 0;

  // Deactivates this channel. No more packets can be sent or received after
  // this is called. |rx_callback| may still be called if it has been already
  // dispatched to its task runner.
  //
  // This method is idempotent.
  virtual void Deactivate() = 0;

  // Signals that the underlying link should be disconnected. This should be
  // called when a service layer protocol error requires the connection to be
  // severed.
  //
  // The link error callback (provided to L2CAP::Register* methods) is invoked
  // as a result of this operation. The handler is responsible for actually
  // disconnecting the link.
  //
  // This does not deactivate the channel, though the channel is expected to
  // close when the link gets removed later.
  virtual void SignalLinkError() = 0;

  // Requests to upgrade the security properties of the underlying link to the
  // requested |level| and reports the result via |callback|. Has no effect if
  // the channel is not active.
  virtual void UpgradeSecurity(sm::SecurityLevel level,
                               sm::ResultFunction<> callback) = 0;

  // Queue the given SDU payload for transmission over this channel, taking
  // ownership of |sdu|. Returns true if the SDU was queued successfully, and
  // false otherwise.
  //
  // For reasons why queuing might fail, see the documentation for the relevant
  // TxEngine's QueueSdu() method. Note: a successfully enqueued SDU may still
  // fail to reach the receiver, due to asynchronous local errors, transmission
  // failure, or remote errors.
  virtual bool Send(ByteBufferPtr sdu) = 0;

  // Request that the ACL priority of this channel be changed to |priority|.
  // Calls |callback| with success if the request succeeded, or error otherwise.
  // Requests may fail if the controller does not support changing the ACL
  // priority or the indicated priority conflicts with another channel.
  virtual void RequestAclPriority(
      pw::bluetooth::AclPriority,
      fit::callback<void(fit::result<fit::failed>)> callback) = 0;

  // Sets an automatic flush timeout with duration |flush_timeout|. |callback|
  // will be called with the result of the operation. This is only supported if
  // the link type is kACL (BR/EDR). |flush_timeout| must be in the range [1ms -
  // hci_spec::kMaxAutomaticFlushTimeoutDuration]. A flush timeout of
  // pw::chrono::SystemClock::duration::max() indicates an infinite flush
  // timeout (packets will be marked flushable, but there will be no automatic
  // flush timeout).
  virtual void SetBrEdrAutomaticFlushTimeout(
      pw::chrono::SystemClock::duration flush_timeout,
      hci::ResultCallback<> callback) = 0;

  // Attach this channel as a child node of |parent| with the given |name|.
  virtual void AttachInspect(inspect::Node& parent, std::string name) = 0;

  // Request the start of A2DP source offloading. |callback| will be called with
  // the result of the request. If offloading is already started or is pending,
  // the request will fail and an error will be reported synchronously.
  virtual void StartA2dpOffload(const A2dpOffloadManager::Configuration& config,
                                hci::ResultCallback<> callback) = 0;

  // Request the stop of A2DP source offloading. |callback| will be called with
  // the result of the request. If offloading is already stopped, report
  // success.
  virtual void StopA2dpOffload(hci::ResultCallback<> callback) = 0;

  // The ACL priority that was both requested and accepted by the controller.
  pw::bluetooth::AclPriority requested_acl_priority() const {
    return requested_acl_priority_;
  }

 protected:
  const ChannelId id_;
  const ChannelId remote_id_;
  const bt::LinkType link_type_;
  const hci_spec::ConnectionHandle link_handle_;
  ChannelInfo info_;
  // Maximum number of PDUs in the channel queue
  uint16_t max_tx_queued_;
  // The ACL priority that was requested by a client and accepted by the
  // controller.
  pw::bluetooth::AclPriority requested_acl_priority_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(Channel);
};

namespace internal {

class LogicalLink;
using LogicalLinkWeakPtr = WeakSelf<LogicalLink>::WeakPtr;

// Channel implementation used in production.
class ChannelImpl : public Channel, public TxEngine::TxChannel {
 public:
  // Many core-spec protocols which operate over fixed channels (e.g. v5.2 Vol.
  // 3 Parts F (ATT) and H (SMP)) define service-specific MTU values. Channels
  // created with `CreateFixedChannel` do not check against these
  // service-specific MTUs. Thus `bt-host` local services which operate over
  // fixed channels are required to respect their MTU internally by:
  //   1.) never sending packets larger than their spec-defined MTU.
  //   2.) handling inbound PDUs which are larger than their spec-defined MTU
  //   appropriately.
  static std::unique_ptr<ChannelImpl> CreateFixedChannel(
      pw::async::Dispatcher& dispatcher,
      ChannelId id,
      internal::LogicalLinkWeakPtr link,
      hci::CommandChannel::WeakPtr cmd_channel,
      uint16_t max_acl_payload_size,
      A2dpOffloadManager& a2dp_offload_manager,
      pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider,
      uint16_t max_tx_queued = kDefaultTxMaxQueuedCount);

  static std::unique_ptr<ChannelImpl> CreateDynamicChannel(
      pw::async::Dispatcher& dispatcher,
      ChannelId id,
      ChannelId peer_id,
      internal::LogicalLinkWeakPtr link,
      ChannelInfo info,
      hci::CommandChannel::WeakPtr cmd_channel,
      uint16_t max_acl_payload_size,
      A2dpOffloadManager& a2dp_offload_manager,
      pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider,
      uint16_t max_tx_queued = kDefaultTxMaxQueuedCount);

  ~ChannelImpl() override;

  // Returns the next PDU fragment, or nullptr if none is available.
  // Converts pending transmission PDUs to fragments
  // Fragments of the same PDU must be sent before another channel in the same
  // link can send packets
  std::unique_ptr<hci::ACLDataPacket> GetNextOutboundPacket();

  // Called by |link_| to notify us when the channel can no longer process data.
  void OnClosed();

  // Called by |link_| when a PDU targeting this channel has been received.
  // Contents of |pdu| will be moved.
  void HandleRxPdu(PDU&& pdu);

  bool HasSDUs() const { return !pending_tx_sdus_.empty(); }

  bool HasPDUs() const { return !pending_tx_pdus_.empty(); }

  bool HasFragments() const { return !pending_tx_fragments_.empty(); }

  // Add send credits to the TxEngine. No-op for TxEngines that are creditless.
  void AddCredits(uint16_t credits);

  // Channel overrides:
  const sm::SecurityProperties security() override;
  bool Activate(RxCallback rx_callback,
                ClosedCallback closed_callback) override;
  void Deactivate() override;
  void SignalLinkError() override;
  bool Send(ByteBufferPtr sdu) override;
  void UpgradeSecurity(sm::SecurityLevel level,
                       sm::ResultFunction<> callback) override;
  void RequestAclPriority(
      pw::bluetooth::AclPriority priority,
      fit::callback<void(fit::result<fit::failed>)> callback) override;
  void SetBrEdrAutomaticFlushTimeout(
      pw::chrono::SystemClock::duration flush_timeout,
      hci::ResultCallback<> callback) override;
  void AttachInspect(inspect::Node& parent, std::string name) override;
  void StartA2dpOffload(const A2dpOffloadManager::Configuration& config,
                        hci::ResultCallback<> callback) override;
  void StopA2dpOffload(hci::ResultCallback<> callback) override;

  using WeakPtr = WeakSelf<ChannelImpl>::WeakPtr;
  WeakPtr GetWeakPtr() { return weak_self_.GetWeakPtr(); }

 private:
  static constexpr size_t kMaxPendingRxSdus = 64;

  ChannelImpl(pw::async::Dispatcher& dispatcher,
              ChannelId id,
              ChannelId remote_id,
              internal::LogicalLinkWeakPtr link,
              ChannelInfo info,
              hci::CommandChannel::WeakPtr cmd_channel,
              uint16_t max_acl_payload_size,
              A2dpOffloadManager& a2dp_offload_manager,
              pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider,
              uint16_t max_tx_queued);

  // Common channel closure logic. Called on Deactivate/OnClosed.
  void CleanUp();

  // Called by |tx_engine_| to deliver a PDU to lower layers.
  void SendFrame(ByteBufferPtr pdu) override;

  // Called by |tx_engine_| to get a queued SDU for processing.
  std::optional<ByteBufferPtr> GetNextQueuedSdu() override;

  bool AreQueuesEmpty();

  void TryAcquireWakeLease();

  pw::async::Dispatcher& pw_dispatcher_;

  bool active_;
  RxCallback rx_cb_;
  ClosedCallback closed_cb_;

  // The LogicalLink that this channel is associated with. A channel is always
  // created by a LogicalLink.
  //
  // |link_| is guaranteed to be valid as long as the link is active. This is
  // because when a LogicalLink is torn down, it will notify all of its
  // associated channels by calling OnLinkClosed() which sets |link_| to
  // nullptr.
  internal::LogicalLinkWeakPtr link_;

  // Command channel used to transport A2DP offload configuration of vendor
  // extensions.
  hci::CommandChannel::WeakPtr cmd_channel_;

  // The engine which processes received PDUs, and converts them to SDUs for
  // upper layers.
  std::unique_ptr<RxEngine> rx_engine_;

  // The engine which accepts SDUs, and converts them to PDUs for lower layers.
  std::unique_ptr<TxEngine> tx_engine_;

  // The pending SDUs on this channel. Received PDUs are buffered if |rx_cb_| is
  // currently not set.
  // TODO(armansito): We should avoid STL containers for data packets as they
  // all implicitly allocate. This is a reminder to fix this elsewhere
  // (especially in the HCI layer).
  std::queue<ByteBufferPtr, std::list<ByteBufferPtr>> pending_rx_sdus_;

  // Contains outbound SDUs
  std::queue<ByteBufferPtr> pending_tx_sdus_;

  // Contains outbound PDUs
  std::queue<ByteBufferPtr> pending_tx_pdus_;

  // Contains outbound fragments
  std::list<hci::ACLDataPacketPtr> pending_tx_fragments_;

  // Fragmenter and Recombiner are always accessed on the L2CAP thread.
  const Fragmenter fragmenter_;

  uint64_t dropped_packets_ = 0;

  pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider_;
  std::optional<pw::bluetooth_sapphire::Lease> wake_lease_;

  struct InspectProperties {
    inspect::Node node;
    inspect::StringProperty psm;
    inspect::StringProperty local_id;
    inspect::StringProperty remote_id;
    inspect::UintProperty dropped_packets;
  };
  InspectProperties inspect_;

  A2dpOffloadManager& a2dp_offload_manager_;

  WeakSelf<ChannelImpl> weak_self_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(ChannelImpl);
};

}  // namespace internal
}  // namespace bt::l2cap
