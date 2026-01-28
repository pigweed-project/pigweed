// Copyright 2024 The Pigweed Authors
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

#include <optional>

#include "pw_allocator/allocator.h"
#include "pw_bluetooth_proxy/basic_l2cap_channel.h"
#include "pw_bluetooth_proxy/gatt_notify_channel.h"
#include "pw_bluetooth_proxy/internal/acl_data_channel.h"
#include "pw_bluetooth_proxy/internal/l2cap_channel.h"
#include "pw_bluetooth_proxy/internal/l2cap_logical_link.h"
#include "pw_bluetooth_proxy/internal/l2cap_status_tracker.h"
#include "pw_bluetooth_proxy/internal/locked_l2cap_channel.h"
#include "pw_bluetooth_proxy/internal/mutex.h"
#include "pw_bluetooth_proxy/l2cap_channel_common.h"
#include "pw_bluetooth_proxy/l2cap_channel_manager_interface.h"
#include "pw_bluetooth_proxy/l2cap_coc.h"
#include "pw_containers/intrusive_map.h"
#include "pw_function/function.h"
#include "pw_sync/lock_annotations.h"
#include "pw_sync/mutex.h"
#include "pw_sync/thread_notification.h"

// This include is not used but a downstream project transitively depends on it.
#include "pw_containers/flat_map.h"

#if PW_BLUETOOTH_PROXY_ASYNC == 0
#include "pw_bluetooth_proxy/internal/l2cap_channel_manager_sync.h"
#else
#include "pw_bluetooth_proxy/internal/l2cap_channel_manager_async.h"
#endif  // PW_BLUETOOTH_PROXY_ASYNC

#if PW_BLUETOOTH_PROXY_MULTIBUF == PW_BLUETOOTH_PROXY_MULTIBUF_V1
#include "pw_multibuf/simple_allocator.h"  // nogncheck
#endif

namespace pw::bluetooth::proxy {

// `L2capChannelManager` mediates between `ProxyHost` and the L2CAP-based
// channels held by clients of `ProxyHost`, such as L2CAP connection-oriented
// channels, GATT Notify channels, and RFCOMM channels.
//
// ACL packet transmission is subject to data control flow, managed by
// `AclDataChannel`. `L2capChannelManager` handles queueing Tx packets when
// credits are unavailable and sending Tx packets as credits become available,
// dequeueing packets in FIFO order per channel and in round robin fashion
// around channels.
class L2capChannelManager final : public L2capChannelManagerInterface {
 public:
  /// @param[in] allocator - General purpose allocator to use for internal
  /// packet buffers and objects.
  L2capChannelManager(AclDataChannel& acl_data_channel,
                      pw::Allocator& allocator);

  ~L2capChannelManager() override;

  pw::Result<L2capCoc> AcquireL2capCoc(
      MultiBufAllocator& rx_multibuf_allocator,
      uint16_t connection_handle,
      ConnectionOrientedChannelConfig rx_config,
      ConnectionOrientedChannelConfig tx_config,
      Function<void(FlatConstMultiBuf&& payload)>&& receive_fn,
      ChannelEventCallback&& event_fn)
      PW_LOCKS_EXCLUDED(links_mutex_, channels_mutex());

  /// @deprecated Use InterceptBasicModeChannel() instead.
  pw::Result<BasicL2capChannel> AcquireBasicL2capChannel(
      MultiBufAllocator& rx_multibuf_allocator,
      uint16_t connection_handle,
      uint16_t local_cid,
      uint16_t remote_cid,
      AclTransportType transport,
      OptionalPayloadReceiveCallback&& payload_from_controller_fn,
      OptionalPayloadReceiveCallback&& payload_from_host_fn,
      ChannelEventCallback&& event_fn)
      PW_LOCKS_EXCLUDED(links_mutex_, channels_mutex());

  pw::Result<GattNotifyChannel> AcquireGattNotifyChannel(
      uint16_t connection_handle,
      uint16_t attribute_handle,
      ChannelEventCallback&& event_fn)
      PW_LOCKS_EXCLUDED(links_mutex_, channels_mutex());

  // Start proxying L2CAP packets addressed to `channel` arriving from
  // the controller and allow `channel` to send & queue Tx L2CAP packets.
  //
  // @returns
  // * @OK: Channel was registered.
  // * @ALREADY_EXISTS: An active channel with the same connection handle, local
  //                    CID, and remote CID is already registered.
  Status RegisterChannel(L2capChannel& channel)
      PW_LOCKS_EXCLUDED(channels_mutex());

  // Stop proxying L2CAP packets addressed to `channel` and stop sending L2CAP
  // packets queued in `channel`, if `channel` is currently registered.
  void DeregisterChannel(L2capChannel& channel)
      PW_LOCKS_EXCLUDED(channels_mutex());

  // Deregister and close all channels then propagate `event` to clients.
  void DeregisterAndCloseChannels(L2capChannelEvent event)
      PW_LOCKS_EXCLUDED(links_mutex_, channels_mutex());

  // Get an `H4PacketWithH4` backed by a buffer able to hold `size` bytes of
  // data.
  //
  // Returns PW_STATUS_UNAVAILABLE if a buffer could not be allocated.
  pw::Result<H4PacketWithH4> GetAclH4Packet(uint16_t size);

  // Report that new tx packets have been queued or new tx credits have been
  // received since the last DrainChannelQueuesIfNewTx.
  void ReportNewTxPacketsOrCredits() { impl_.ReportNewTxPacketsOrCredits(); }

  // Send L2CAP packets queued in registered channels. Since this function takes
  // the channels_mutex_ lock, it can't be directly called while handling a
  // received packet on a channel. Instead call ReportPacketsMayBeReadyToSend().
  // Rx processing will then call this function when complete.
  void DrainChannelQueuesIfNewTx() { impl_.DrainChannelQueuesIfNewTx(); }

  // Drain channel queues even if no channel explicitly requested it. Should be
  // used for events triggering queue space at the ACL level.
  void ForceDrainChannelQueues() PW_LOCKS_EXCLUDED(channels_mutex());

  std::optional<LockedL2capChannel> FindChannelByLocalCid(
      uint16_t connection_handle, uint16_t local_cid)
      PW_LOCKS_EXCLUDED(channels_mutex());

  std::optional<LockedL2capChannel> FindChannelByRemoteCid(
      uint16_t connection_handle, uint16_t remote_cid)
      PW_LOCKS_EXCLUDED(channels_mutex());

  L2capChannel* FindChannelByLocalCidLocked(uint16_t connection_handle,
                                            uint16_t local_cid)
      PW_EXCLUSIVE_LOCKS_REQUIRED(channels_mutex());

  L2capChannel* FindChannelByRemoteCidLocked(uint16_t connection_handle,
                                             uint16_t remote_cid)
      PW_EXCLUSIVE_LOCKS_REQUIRED(channels_mutex());

  // Register for notifications of connection and disconnection for a
  // particular L2cap service identified by its PSM.
  void RegisterStatusDelegate(L2capStatusDelegate& delegate);

  // Unregister a service delegate.
  void UnregisterStatusDelegate(L2capStatusDelegate& delegate);

  // Called when a l2cap channel connection successfully made.
  void HandleConnectionComplete(const L2capChannelConnectionInfo& info);

  // Called when a l2cap channel configuration is done.
  void HandleConfigurationChanged(const L2capChannelConfigurationInfo& info);

  // Called when an ACL connection is disconnected.
  void HandleAclDisconnectionComplete(uint16_t connection_handle)
      PW_LOCKS_EXCLUDED(links_mutex_, channels_mutex());

  // Called when a l2cap channel connection is disconnected.
  //
  // Must be called under channels_mutex_ but we can't use proper lock
  // annotation here since the call comes via signaling channel.
  // TODO: https://pwbug.dev/390511432 - Figure out way to add annotations to
  // enforce this invariant.
  void HandleDisconnectionCompleteLocked(
      const L2capStatusTracker::DisconnectParams& params);

  // Deliver any pending connection events. Should not be called while holding
  // channels_mutex_.
  void DeliverPendingEvents() PW_LOCKS_EXCLUDED(channels_mutex());

  // Register a logical link with the L2CAP layer.
  //
  // @returns
  // * @OK: The link was successfully added.
  // * @ALREADY_EXISTS: The link is already registered.
  // * @RESOURCE_EXHAUSTED: There is no memory left to allocate state for the
  // link.
  Status AddConnection(uint16_t connection_handle, AclTransportType transport);

  // Send L2CAP_FLOW_CONTROL_CREDIT_IND to indicate local endpoint `cid` is
  // capable of receiving a number of additional K-frames (`credits`).
  //
  // @returns
  // * @OK: `L2CAP_FLOW_CONTROL_CREDIT_IND` was sent.
  // * @UNAVAILABLE: Send could not be queued due to lack of memory in the
  //   client-provided `multibuf_allocator` (transient error).
  // * @FAILED_PRECONDITION: Channel is not `State::kRunning`.
  Status SendFlowControlCreditInd(uint16_t connection_handle,
                                  uint16_t channel_id,
                                  uint16_t credits,
                                  MultiBufAllocator& multibuf_allocator);

  // Returns the max ACL payload size if the Read Buffer Size command complete
  // event was received.
  std::optional<uint16_t> MaxDataPacketLengthForTransport(
      AclTransportType transport) const;

  // Returns the max L2CAP payload size if the Read Buffer Size command complete
  // event was received.
  Result<uint16_t> MaxL2capPayloadSize(AclTransportType transport) const;

  constexpr internal::L2capChannelManagerImpl& impl() { return impl_; }
  constexpr const internal::L2capChannelManagerImpl& impl() const {
    return impl_;
  }

 private:
  using L2capChannelPredicate = pw::Function<bool(L2capChannel&)>;
  using L2capChannelMap = internal::L2capChannelManagerImpl::L2capChannelMap;
  using L2capChannelIterator =
      internal::L2capChannelManagerImpl::L2capChannelIterator;

  static constexpr internal::Mutex& channels_mutex()
      PW_LOCK_RETURNED(internal::L2capChannelManagerImpl::channels_mutex()) {
    return internal::L2capChannelManagerImpl::channels_mutex();
  }

  // Circularly advance `it`, wrapping around to front if `it` reaches the
  // end.
  void Advance(L2capChannelIterator& it)
      PW_EXCLUSIVE_LOCKS_REQUIRED(channels_mutex());

  // RegisterChannel with channels_mutex_ already acquired.
  //
  // @returns
  // * @OK: Channel was registered.
  // * @ALREADY_EXISTS: An active channel with the same connection handle, local
  // CID, and remote CID is already registered.
  Status RegisterChannelLocked(L2capChannel& channel)
      PW_EXCLUSIVE_LOCKS_REQUIRED(channels_mutex());

  // Stop proxying L2CAP packets addressed to `channel` and stop sending L2CAP
  // packets queued in `channel`, if `channel` is currently registered.
  void DeregisterChannelLocked(L2capChannel& channel)
      PW_EXCLUSIVE_LOCKS_REQUIRED(channels_mutex());

  // Delete a channel, which must be deregister and closed before calling.
  void DeleteChannelLocked(L2capChannel& channel)
      PW_EXCLUSIVE_LOCKS_REQUIRED(channels_mutex());

  // Deletes any channels that have been observed to be stale.
  void DeleteStaleChannels() PW_LOCKS_EXCLUDED(channels_mutex());

  void ResetLogicalLinksLocked() PW_EXCLUSIVE_LOCKS_REQUIRED(links_mutex_);

  // L2capChannelManagerInterface override:
  Result<UniquePtr<ChannelProxy>> DoInterceptBasicModeChannel(
      ConnectionHandle connection_handle,
      uint16_t local_channel_id,
      uint16_t remote_channel_id,
      AclTransportType transport,
      BufferReceiveFunction&& payload_from_controller_fn,
      BufferReceiveFunction&& payload_from_host_fn,
      ChannelEventCallback&& event_fn) override
      PW_LOCKS_EXCLUDED(links_mutex_, channels_mutex());
  Result<UniquePtr<ChannelProxy>> DoInterceptCreditBasedFlowControlChannel(
      ConnectionHandle connection_handle,
      ConnectionOrientedChannelConfig rx_config,
      ConnectionOrientedChannelConfig tx_config,
      MultiBufReceiveFunction&& receive_fn,
      ChannelEventCallback&& event_fn) override
      PW_LOCKS_EXCLUDED(links_mutex_, channels_mutex());

  // Reference to the ACL data channel owned by the proxy.
  AclDataChannel& acl_data_channel_;

  // List of registered L2CAP channels.
  L2capChannelMap channels_by_local_cid_ PW_GUARDED_BY(channels_mutex());
  L2capChannelMap channels_by_remote_cid_ PW_GUARDED_BY(channels_mutex());

  // Stale L2CAP channels awaiting deletion.
  L2capChannelMap stale_ PW_GUARDED_BY(channels_mutex());

  // Implementation-specific details that may vary between sync and async modes.
  friend class internal::L2capChannelManagerImpl;
  internal::L2capChannelManagerImpl impl_;

  // Channel connection status tracker and delegate holder.
  L2capStatusTracker status_tracker_;

  // A separate links mutex is required so that the channels owned by the links
  // can be destroyed without deadlock.
  internal::Mutex links_mutex_ PW_ACQUIRED_BEFORE(channels_mutex());
  IntrusiveMap<uint16_t, internal::L2capLogicalLinkInterface> logical_links_
      PW_GUARDED_BY(links_mutex_);

#if PW_BLUETOOTH_PROXY_MULTIBUF == PW_BLUETOOTH_PROXY_MULTIBUF_V1
  // This buffer is small because it is only used with ChannelProxy, which has
  // no MultiBuf clients other than tests, and MultiBuf v1 will soon be removed.
  std::array<std::byte, 200> allocator_buffer_;
  multibuf::SimpleAllocator multibuf_allocator_{allocator_buffer_,
                                                impl_.allocator()};
#elif PW_BLUETOOTH_PROXY_MULTIBUF == PW_BLUETOOTH_PROXY_MULTIBUF_V2
  MultiBufAllocator multibuf_allocator_{impl_.allocator(), impl_.allocator()};
#endif
};

}  // namespace pw::bluetooth::proxy
