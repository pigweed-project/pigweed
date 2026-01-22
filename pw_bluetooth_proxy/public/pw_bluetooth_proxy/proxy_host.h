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

#include "pw_bluetooth_proxy/gatt_notify_channel.h"
#include "pw_bluetooth_proxy/internal/acl_data_channel.h"
#include "pw_bluetooth_proxy/internal/hci_transport.h"
#include "pw_bluetooth_proxy/internal/l2cap_channel_manager.h"
#include "pw_bluetooth_proxy/internal/multibuf.h"
#include "pw_bluetooth_proxy/l2cap_channel_common.h"
#include "pw_bluetooth_proxy/l2cap_coc.h"
#include "pw_bluetooth_proxy/l2cap_status_delegate.h"
#include "pw_function/function.h"
#include "pw_status/status.h"

#if PW_BLUETOOTH_PROXY_ASYNC == 0
#include "pw_bluetooth_proxy/internal/proxy_host_sync.h"

// TODO: b/472522742 - Forward-declare the dispatcher until downstream support
// for pw_async2 is resolved.
namespace pw::async2 {
class Dispatcher;
}  // namespace pw::async2
#else
#include "pw_async2/dispatcher.h"
#include "pw_bluetooth_proxy/internal/proxy_host_async.h"
#endif  // PW_BLUETOOTH_PROXY_ASYNC

/// Lightweight proxy for augmenting Bluetooth functionality
namespace pw::bluetooth::proxy {

/// @module{pw_bluetooth_proxy}

/// `ProxyHost` acts as the main coordinator for proxy functionality. After
/// creation, the container then passes packets through the proxy.
class ProxyHost : public L2capChannelManagerInterface {
 public:
  /// Creates an `ProxyHost` that will process HCI packets.
  /// @param[in] send_to_host_fn Callback that will be called when proxy wants
  /// to send HCI packet towards the host.
  /// @param[in] send_to_controller_fn - Callback that will be called when
  /// proxy wants to send HCI packet towards the controller.
  /// @param[in] le_acl_credits_to_reserve - How many buffers to reserve for the
  /// proxy out of any LE ACL buffers received from controller.
  /// @param[in] br_edr_acl_credits_to_reserve - How many buffers to reserve for
  /// the proxy out of any BR/EDR ACL buffers received from controller.
  /// @param[in] allocator - General purpose allocator to use for internal
  /// packet buffers and objects. On multi-threaded systems this should be a
  /// SynchronizedAllocator. Must not be null.
  ProxyHost(pw::Function<void(H4PacketWithHci&& packet)>&& send_to_host_fn,
            pw::Function<void(H4PacketWithH4&& packet)>&& send_to_controller_fn,
            uint16_t le_acl_credits_to_reserve,
            uint16_t br_edr_acl_credits_to_reserve,
            pw::Allocator* allocator);

  ProxyHost() = delete;
  ProxyHost(const ProxyHost&) = delete;
  ProxyHost& operator=(const ProxyHost&) = delete;
  ProxyHost(ProxyHost&&) = delete;
  ProxyHost& operator=(ProxyHost&&) = delete;
  /// Deregisters all channels, and if any channels are not yet closed, closes
  /// them and sends `L2capChannelEvent::kChannelClosedByOther`.
  ~ProxyHost() override;

  // ##### Container API
  // Containers are expected to call these functions (in addition to ctor).

  /// Called by container to ask proxy to handle a H4 HCI packet sent from the
  /// host side towards the controller side. Proxy will in turn call the
  /// `send_to_controller_fn` provided during construction to pass the packet
  /// on to the controller. Some packets may be modified, added, or removed.
  ///
  /// The proxy host currently does not require any from-host packets to support
  /// its current functionality. It will pass on all packets, so containers can
  /// choose to just pass all from-host packets through it.
  ///
  /// Container is required to call this function synchronously (one packet at a
  /// time).
  ///
  /// If using async mode, this function must only be called from the thread
  /// that the dispatcher is running on.
  void HandleH4HciFromHost(H4PacketWithH4&& h4_packet);

  /// Called by container to ask proxy to handle a H4 packet sent from the
  /// controller side towards the host side. Proxy will in turn call the
  /// `send_to_host_fn` provided during construction to pass the packet on to
  /// the host. Some packets may be modified, added, or removed.
  ///
  /// To support all of its current functionality, the proxy host needs at least
  /// the following from-controller packets passed through it. It will pass on
  /// all other packets, so containers can choose to just pass all
  /// from-controller packets through the proxy host.
  ///
  /// All packets of this type:
  /// - L2CAP over ACL packets (specifically those addressed to channels managed
  ///   by the proxy host, including signaling packets)
  ///
  /// HCI_Command_Complete events (7.7.14) containing return parameters for
  /// these commands:
  /// - HCI_LE_Read_Buffer_Size [v1] command (7.8.2)
  /// - HCI_LE_Read_Buffer_Size [v2] command (7.8.2)
  ///
  /// These HCI event packets:
  /// - HCI_Number_Of_Completed_Packets event (7.7.19)
  /// - HCI_Disconnection_Complete event (7.7.5)
  ///
  /// Container is required to call this function synchronously (one packet at a
  /// time).
  ///
  /// If using async mode, this function must only be called from the thread
  /// that the dispatcher is running on.
  void HandleH4HciFromController(H4PacketWithHci&& h4_packet);

  /// Called when an HCI_Reset Command packet is observed. Proxy resets its
  /// internal state. Deregisters all channels, and if any channels are not yet
  /// closed, closes them and sends `L2capChannelEvent::kReset`.
  ///
  /// May also be called by container to notify proxy that the Bluetooth system
  /// is being otherwise reset.
  ///
  /// Warning: Outstanding H4 packets are not invalidated upon reset. If they
  /// are destructed post-reset, packets generated post-reset are liable to be
  /// overwritten prematurely.
  void Reset();

  // ##### Container async API

  /// Sets dispatcher to use to run asynchronous tasks.
  ///
  /// The dispatcher must outlive the ProxyHost and any clients. This method
  /// must called from the thread the dispatcher will run on.
  ///
  /// @returns
  /// * @OK: Dispatcher is ready to run tasks.
  /// * @FAILED_PRECONDITION: Dispatcher is already set.
  /// * @UNIMPLEMENTED: PW_BLUETOOTH_PROXY_ASYNC is not enabled.
  Status SetDispatcher(async2::Dispatcher& dispatcher);

  // ##### Client APIs

  /// Register for notifications of connection and disconnection for a
  /// particular L2cap service identified by its PSM.
  ///
  /// @param[in] delegate   A delegate that will be notified when a successful
  ///                       L2cap connection is made on its PSM. Note: This
  ///                       must outlive the ProxyHost.
  void RegisterL2capStatusDelegate(L2capStatusDelegate& delegate);

  /// Unregister a service delegate.
  ///
  /// @param[in] delegate   The delegate to unregister. Must have been
  ///                       previously registered.
  void UnregisterL2capStatusDelegate(L2capStatusDelegate& delegate);

  /// Returns an L2CAP connection-oriented channel that supports writing to and
  /// reading from a remote peer.
  ///
  /// @param[in] rx_multibuf_allocator  Provides the allocator the channel will
  ///                                   use for its Rx buffers (for both
  ///                                   queueing and returning to the client).
  ///
  /// @param[in] connection_handle      The connection handle of the remote
  ///                                   peer.
  ///
  /// @param[in] rx_config              Parameters applying to reading packets.
  ///                                   See `l2cap_coc.h` for details.
  ///
  /// @param[in] tx_config              Parameters applying to writing packets.
  ///                                   See `l2cap_coc.h` for details.
  ///
  /// @param[in] receive_fn             Read callback to be invoked on Rx SDUs.
  ///
  /// @param[in] event_fn               Handle asynchronous events such as
  ///                                   errors and flow control events
  ///                                   encountered by the channel. See
  ///                                   `l2cap_channel_common.h`.
  ///                                   Must outlive the channel and remain
  ///                                   valid until the channel destructor
  ///                                   returns.
  ///
  /// @returns @Result{the channel}
  /// * @INVALID_ARGUMENT: Arguments are invalid. Check the logs.
  /// * @UNAVAILABLE: A channel could not be created because no memory was
  ///   available to accommodate an additional ACL connection.
  pw::Result<L2capCoc> AcquireL2capCoc(
      MultiBufAllocator& rx_multibuf_allocator,
      uint16_t connection_handle,
      L2capCoc::CocConfig rx_config,
      L2capCoc::CocConfig tx_config,
      Function<void(FlatConstMultiBuf&& payload)>&& receive_fn,
      ChannelEventCallback&& event_fn);

  /// Returns an L2CAP channel operating in basic mode that supports writing to
  /// and reading from a remote peer.
  ///
  /// @param[in] rx_multibuf_allocator      Provides the allocator the channel
  ///                                       will use for its Rx buffers (for
  ///                                       both queueing and  returning to the
  ///                                       client).
  ///
  /// @param[in] connection_handle          The connection handle of the remote
  ///                                       peer.
  ///
  /// @param[in] local_cid                  L2CAP channel ID of the local
  ///                                       endpoint.
  ///
  /// @param[in] remote_cid                 L2CAP channel ID of the remote
  ///                                       endpoint.
  ///
  /// @param[in] transport                  Logical link transport type.
  ///
  /// @param[in] payload_from_controller_fn Read callback to be invoked on Rx
  ///                                       SDUs. If a multibuf is returned by
  ///                                       the callback, it is copied into the
  ///                                       payload to be forwarded to the host.
  ///                                       Optional null return indicates
  ///                                       packet was handled and no forwarding
  ///                                       is required.
  ///
  /// @param[in] payload_from_host_fn       Read callback to be invoked on Tx
  ///                                       SDUs. If a multibuf is returned by
  ///                                       the callback, it is copied into the
  ///                                       payload to be forwarded to the
  ///                                       controller. Optional null return
  ///                                       indicates packet was handled and no
  ///                                       forwarding is required.
  ///
  /// @param[in] event_fn                   Handle asynchronous events such as
  ///                                       errors and flow control events
  ///                                       encountered by the channel. See
  ///                                       `l2cap_channel_common.h`.
  ///                                       Must outlive the channel and remain
  ///                                       valid until the channel destructor
  ///                                       returns.
  ///
  /// @returns @Result{the channel}
  /// * @INVALID_ARGUMENT: Arguments are invalid. Check the logs.
  /// * @UNAVAILABLE: A channel could not be created because no memory was
  ///   available to accommodate an additional ACL connection.
  pw::Result<BasicL2capChannel> AcquireBasicL2capChannel(
      MultiBufAllocator& rx_multibuf_allocator,
      uint16_t connection_handle,
      uint16_t local_cid,
      uint16_t remote_cid,
      AclTransportType transport,
      OptionalPayloadReceiveCallback&& payload_from_controller_fn,
      OptionalPayloadReceiveCallback&& payload_from_host_fn,
      ChannelEventCallback&& event_fn);

  /// Returns a GATT Notify channel channel that supports sending notifications
  /// to a particular connection handle and attribute.
  ///
  /// @param[in] connection_handle  The connection handle of the peer to notify.
  ///                               Maximum valid connection handle is 0x0EFF.
  ///
  /// @param[in] attribute_handle   The attribute handle the notify should be
  ///                               sent on. Cannot be 0.
  ///
  /// @param[in] event_fn           Handle asynchronous events such as errors
  ///                               and flow control events encountered by the
  ///                               channel. See `l2cap_channel_common.h`. Must
  ///                               outlive the channel and remain valid until
  ///                               the channel destructor returns.
  ///
  /// @returns @Result{the channel}
  /// * @INVALID_ARGUMENT: Arguments are invalid. Check the logs.
  /// * @UNAVAILABLE: A channel could not be created because no memory was
  ///   available to accommodate an additional ACL connection.
  pw::Result<GattNotifyChannel> AcquireGattNotifyChannel(
      int16_t connection_handle,
      uint16_t attribute_handle,
      ChannelEventCallback&& event_fn);

  /// Indicates whether the proxy has the capability of sending LE ACL packets.
  /// Note that this indicates intention, so it can be true even if the proxy
  /// has not yet or has been unable to reserve credits from the host.
  bool HasSendLeAclCapability() const;

  /// Indicates whether the proxy has the capability of sending BR/EDR ACL
  /// packets. Note that this indicates intention, so it can be true even if the
  /// proxy has not yet or has been unable to reserve credits from the host.
  bool HasSendBrEdrAclCapability() const;

  /// Returns the number of available LE ACL send credits for the proxy.
  /// Can be zero if the controller has not yet been initialized by the host.
  uint16_t GetNumFreeLeAclPackets() const;

  /// Returns the number of available BR/EDR ACL send credits for the proxy.
  /// Can be zero if the controller has not yet been initialized by the host.
  uint16_t GetNumFreeBrEdrAclPackets() const;

  /// Returns the max number of simultaneous LE ACL connections supported.
  static constexpr size_t GetMaxNumAclConnections() {
    return AclDataChannel::GetMaxNumAclConnections();
  }

 private:
  friend class internal::ProxyHostImpl;

  /// @copydoc ProxyHost::HandleH4HciFromHost
  void DoHandleH4HciFromHost(H4PacketWithH4&& h4_packet);

  /// @copydoc ProxyHost::HandleH4HciFromController
  void DoHandleH4HciFromController(H4PacketWithHci&& h4_packet);

  /// @copydoc ProxyHost::Reset
  void DoReset();

  /// @copydoc ProxyHost::RegisterL2capStatusDelegate
  void DoRegisterL2capStatusDelegate(L2capStatusDelegate& delegate);

  /// @copydoc ProxyHost::UnregisterL2capStatusDelegate
  void DoUnregisterL2capStatusDelegate(L2capStatusDelegate& delegate);

  /// @copydoc ProxyHost::AcquireL2capCoc
  pw::Result<L2capCoc> DoAcquireL2capCoc(
      MultiBufAllocator& rx_multibuf_allocator,
      uint16_t connection_handle,
      L2capCoc::CocConfig rx_config,
      L2capCoc::CocConfig tx_config,
      Function<void(FlatConstMultiBuf&& payload)>&& receive_fn,
      ChannelEventCallback&& event_fn);

  /// @copydoc ProxyHost::AcquireBasicL2capChannel
  pw::Result<BasicL2capChannel> DoAcquireBasicL2capChannel(
      MultiBufAllocator& rx_multibuf_allocator,
      uint16_t connection_handle,
      uint16_t local_cid,
      uint16_t remote_cid,
      AclTransportType transport,
      OptionalPayloadReceiveCallback&& payload_from_controller_fn,
      OptionalPayloadReceiveCallback&& payload_from_host_fn,
      ChannelEventCallback&& event_fn);

  /// @copydoc ProxyHost::AcquireGattNotifyChannel
  pw::Result<GattNotifyChannel> DoAcquireGattNotifyChannel(
      int16_t connection_handle,
      uint16_t attribute_handle,
      ChannelEventCallback&& event_fn);

  /// @copydoc ProxyHost::HasSendLeAclCapability
  bool DoHasSendLeAclCapability() const;

  /// @copydoc ProxyHost::HasSendBrEdrAclCapability
  bool DoHasSendBrEdrAclCapability() const;

  /// @copydoc ProxyHost::GetNumFreeLeAclPackets
  uint16_t DoGetNumFreeLeAclPackets() const;

  /// @copydoc ProxyHost::GetNumFreeBrEdrAclPackets
  uint16_t DoGetNumFreeBrEdrAclPackets() const;

  // Handle HCI Event packet from the controller.
  void HandleEventFromController(H4PacketWithHci&& h4_packet);

  // Handle HCI Event packet from the host.
  void HandleEventFromHost(H4PacketWithH4&& h4_packet);

  // Handle HCI ACL data packet from the controller.
  void HandleAclFromController(H4PacketWithHci&& h4_packet);

  // Process an LE_META_EVENT
  void HandleLeMetaEvent(H4PacketWithHci&& h4_packet);

  // Process a Command_Complete event.
  void HandleCommandCompleteEvent(H4PacketWithHci&& h4_packet);

  // Handle HCI Command packet from the host.
  void HandleCommandFromHost(H4PacketWithH4&& h4_packet);

  // Handle HCI ACL data packet from the host.
  void HandleAclFromHost(H4PacketWithH4&& h4_packet);

  // Called when any type of connection complete event is received.
  void OnConnectionCompleteSuccess(uint16_t connection_handle,
                                   AclTransportType transport);

  // AclDataChannel callback for when new ACL TX credits are received and more
  // L2CAP packets can be sent.
  void OnAclTxCredits();

  // L2capChannelManagerInterface override:
  Result<UniquePtr<ChannelProxy>> DoInterceptBasicModeChannel(
      ConnectionHandle connection_handle,
      uint16_t local_channel_id,
      uint16_t remote_channel_id,
      AclTransportType transport,
      BufferReceiveFunction&& payload_from_controller_fn,
      BufferReceiveFunction&& payload_from_host_fn,
      ChannelEventCallback&& event_fn) override;

  Result<UniquePtr<ChannelProxy>> InternalDoInterceptBasicModeChannel(
      ConnectionHandle connection_handle,
      uint16_t local_channel_id,
      uint16_t remote_channel_id,
      AclTransportType transport,
      BufferReceiveFunction&& payload_from_controller_fn,
      BufferReceiveFunction&& payload_from_host_fn,
      ChannelEventCallback&& event_fn);

  // Implementation-specific details that may vary between sync and async modes.
  internal::ProxyHostImpl impl_;

  // For sending non-ACL data to the host and controller. ACL traffic shall be
  // sent through the `acl_data_channel_`.
  HciTransport hci_transport_;

  // Owns management of the LE ACL data channel.
  AclDataChannel acl_data_channel_;

  // Keeps track of the L2CAP-based channels managed by the proxy.
  L2capChannelManager l2cap_channel_manager_;
};

}  // namespace pw::bluetooth::proxy
