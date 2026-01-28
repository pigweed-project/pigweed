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

#include <cstdint>

#include "pw_allocator/unique_ptr.h"
#include "pw_assert/assert.h"
#include "pw_bluetooth_proxy/connection_handle.h"
#include "pw_bluetooth_proxy/direction.h"
#include "pw_bluetooth_proxy/h4_packet.h"
#include "pw_bluetooth_proxy/internal/basic_mode_rx_engine.h"
#include "pw_bluetooth_proxy/internal/basic_mode_tx_engine.h"
#include "pw_bluetooth_proxy/internal/credit_based_flow_control_rx_engine.h"
#include "pw_bluetooth_proxy/internal/credit_based_flow_control_tx_engine.h"
#include "pw_bluetooth_proxy/internal/gatt_notify_rx_engine.h"
#include "pw_bluetooth_proxy/internal/gatt_notify_tx_engine.h"
#include "pw_bluetooth_proxy/internal/logical_transport.h"
#include "pw_bluetooth_proxy/internal/multibuf.h"
#include "pw_bluetooth_proxy/internal/mutex.h"
#include "pw_bluetooth_proxy/l2cap_channel_common.h"
#include "pw_containers/intrusive_map.h"
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_status/try.h"
#include "pw_sync/lock_annotations.h"
#include "pw_sync/mutex.h"
#include "pw_sync/thread_notification.h"

#if PW_BLUETOOTH_PROXY_ASYNC == 0
#include "pw_bluetooth_proxy/internal/l2cap_channel_sync.h"
#else
#include "pw_bluetooth_proxy/internal/l2cap_channel_async.h"
#endif  // PW_BLUETOOTH_PROXY_ASYNC

namespace pw::bluetooth::proxy {

class L2capChannelManager;

namespace internal {

class GenericL2capChannel;
class GenericL2capChannelImpl;
class L2capChannelManagerImpl;

}  // namespace internal

// Base class for peer-to-peer L2CAP-based channels.
//
// Protocol-dependent information that is fixed per channel, such as addresses,
// flags, handles, etc. should be provided at construction to derived channels.
class L2capChannel final : public internal::TxEngine::Delegate {
 public:
  static constexpr uint16_t kMaxValidConnectionHandle = 0x0EFF;

  // Return true if the PDU was consumed by the channel. Otherwise, return false
  // and the PDU will be forwarded by `ProxyHost` on to the Bluetooth
  // controller.
  using PayloadSpanReceiveCallback = Function<bool(pw::span<uint8_t>)>;
  using SpanReceiveFunction = Function<bool(ConstByteSpan,
                                            ConnectionHandle connection_handle,
                                            uint16_t local_channel_id,
                                            uint16_t remote_channel_id)>;

  using FromControllerFn = std::variant<std::monostate,
                                        OptionalPayloadReceiveCallback,
                                        OptionalBufferReceiveFunction,
                                        PayloadSpanReceiveCallback,
                                        SpanReceiveFunction,
                                        MultiBufReceiveFunction>;

  using FromHostFn = std::variant<std::monostate,
                                  OptionalPayloadReceiveCallback,
                                  OptionalBufferReceiveFunction,
                                  PayloadSpanReceiveCallback,
                                  SpanReceiveFunction>;

  enum class State {
    // Channel is new.
    kNew,
    kRunning,
    // Channel is stopped, but the L2CAP connection has not been closed.
    kStopped,
    // L2CAP connection has been closed as the result of an
    // HCI_Disconnection_Complete event, L2CAP_DISCONNECTION_RSP packet, or
    // HCI_Reset Command packet; or `ProxyHost` dtor has been called.
    kClosed,
  };

  // Precondition: AreValidParameters() is true
  explicit L2capChannel(L2capChannelManager& l2cap_channel_manager,
                        MultiBufAllocator* rx_multibuf_allocator,
                        uint16_t connection_handle,
                        AclTransportType transport,
                        uint16_t local_cid,
                        uint16_t remote_cid,
                        ChannelEventCallback&& event_fn);

  ~L2capChannel() override;

  // Returns whether or not ACL connection handle & L2CAP channel identifiers
  // are valid parameters for a packet.
  [[nodiscard]] static bool AreValidParameters(uint16_t connection_handle,
                                               uint16_t local_cid,
                                               uint16_t remote_cid);

  // Initialize the channel for Basic mode. Must be called before `Start`.
  Status InitBasic(FromControllerFn&& from_controller_fn,
                   FromHostFn&& from_host_fn);

  // Initialize the channel for Credit Based Flow Control mode. Must be called
  // before `Start`.
  Status InitCreditBasedFlowControl(ConnectionOrientedChannelConfig rx_config,
                                    ConnectionOrientedChannelConfig tx_config,
                                    MultiBufReceiveFunction&& receive_fn);

  // Initialize the channel for GATT Notify mode. Must be called before `Start`.
  Status InitGattNotify(uint16_t attribute_handle);

  // Registers the channel. Must be called for channel to be active.
  Status Start();

  //-------------
  //  Status (internal public)
  //-------------

  // Helper since these operations should typically be coupled.
  void StopAndSendEvent(L2capChannelEvent event) {
    Stop();
    impl_.SendEvent(event);
  }

  // Enter `State::kStopped`. This means
  //   - Queue is cleared so pending sends will not complete.
  //   - Calls to `QueuePacket()` will return PW_STATUS_FAILED_PRECONDITION, so
  //     derived channels should not accept client writes.
  //   - Rx packets will be dropped & trigger `kRxWhileStopped` events.
  //   - Container is responsible for closing L2CAP connection & destructing
  //     the channel object to free its resources.
  void Stop();

  // Enter `State::kClosed` and disconnects the client. This has all the same
  // effects as stopping the channel and sends `event` unless the client has
  // disconnected. No-op if channel is already `State::kClosed`.
  void Close(
      L2capChannelEvent event = L2capChannelEvent::kChannelClosedByOther);

  //-------------
  //  Tx (public)
  //-------------

  // TODO: https://pwbug.dev/388082771 - Most of these APIs should be in a
  // public header. Will probably do that as part of moving them to
  // ClientChannel.

  /// Send a payload to the remote peer.
  ///
  /// @param[in] payload The client payload to be sent. Payload will be
  /// destroyed once its data has been used.
  ///
  /// @returns A `StatusWithMultiBuf` with one of the statuses below. If status
  /// is not @OK then payload is also returned in `StatusWithMultiBuf`.
  /// * @OK: Packet was successfully queued for send.
  /// * @UNAVAILABLE: Channel could not acquire the resources to queue
  ///   the send at this time (transient error). If an `event_fn` has been
  ///   provided it will be called with `L2capChannelEvent::kWriteAvailable`
  ///   when there is queue space available again.
  /// * @INVALID_ARGUMENT: Payload is too large.
  /// * @FAILED_PRECONDITION: Channel is not `State::kRunning`.
  /// * @UNIMPLEMENTED: Channel does not support `Write(MultiBuf)`.
  // TODO: https://pwbug.dev/388082771 - Plan to eventually move this to
  // ClientChannel.
  StatusWithMultiBuf Write(FlatConstMultiBuf&& payload);

  /// Channels that need to send a payload during handling a received packet
  /// directly (for instance to replenish credits) should use this function
  /// which does not take the L2capChannelManager channels lock.
  StatusWithMultiBuf WriteDuringRx(FlatConstMultiBuf&& payload) {
    return impl_.Write(std::move(payload));
  }

  // Max number of Tx L2CAP packets that can be waiting to send.
  static constexpr size_t QueueCapacity() {
    return internal::L2capChannelImpl::kQueueCapacity;
  }

  //-------------
  //  Rx (public)
  //-------------

  // Handle a Tx L2CAP PDU.
  //
  // Implementations should call `SendPayloadFromHostToClient` after processing
  // the PDU.
  //
  // Return true if the PDU was consumed by the channel. Otherwise, return false
  // and the PDU will be forwarded by `ProxyHost` on to the Bluetooth
  // controller.
  [[nodiscard]] bool HandlePduFromHost(span<uint8_t> l2cap_pdu);

  // Called when an L2CAP PDU is received on this channel. If channel is
  // `kRunning`, returns `HandlePduFromController(l2cap_pdu)`. If channel is not
  // `State::kRunning`, sends `kRxWhileStopped` event to client and drops PDU.
  // This function will call DoHandlePduFromController on its subclass.
  [[nodiscard]] bool HandlePduFromController(pw::span<uint8_t> l2cap_pdu);

  //--------------
  //  Accessors:
  //--------------

  State state() const;

  constexpr uint16_t local_cid() const { return local_handle_.channel_id(); }

  constexpr uint16_t remote_cid() const { return remote_handle_.channel_id(); }

  constexpr uint16_t connection_handle() const {
    return local_handle_.connection_handle();
  }

  constexpr AclTransportType transport() const { return transport_; }

  // Returns the maximum size supported for Tx L2CAP PDU payloads with a Basic
  // header.
  //
  // Returns std::nullopt if the ACL data packet length is not yet known.
  std::optional<uint16_t> MaxL2capPayloadSize() override;

  Status AddTxCredits(uint16_t credits);

 private:
  friend class L2capChannelManager;
  friend class internal::GenericL2capChannel;
  friend class internal::GenericL2capChannelImpl;
  friend class internal::L2capChannelImpl;
  friend class internal::L2capChannelManagerImpl;

  // TODO: https://pwbug.dev/349700888 - Make capacity configurable.
  static constexpr size_t kQueueCapacity = 5;

  L2capChannelManager& channel_manager() const {
    return l2cap_channel_manager_;
  }

  // Returns false if payload should be forwarded to host instead.
  // Allows client to modify the payload to be forwarded.
  using SendPayloadToClientCallback =
      std::variant<OptionalPayloadReceiveCallback*,
                   OptionalBufferReceiveFunction*>;
  bool SendPayloadToClient(span<uint8_t> payload,
                           SendPayloadToClientCallback callback);

  // Reserve an L2CAP packet over ACL over H4 packet.
  pw::Result<H4PacketWithH4> PopulateL2capPacket(uint16_t data_length);

  // TxEngine::Delegate overrides:
  Result<H4PacketWithH4> AllocateH4(uint16_t length) override;

  //--------------
  //  Tx (private)
  //--------------

  // Reserve an L2CAP over ACL over H4 packet, with those three headers
  // populated for an L2CAP PDU payload of `data_length` bytes addressed to
  // `connection_handle_`.
  //
  // Returns PW_STATUS_INVALID_ARGUMENT if payload is too large for a buffer.
  // Returns PW_STATUS_UNAVAILABLE if all buffers are currently occupied.
  pw::Result<H4PacketWithH4> PopulateTxL2capPacket(uint16_t data_length);

  // Alert `L2capChannelManager` that queued packets may be ready to send.
  void ReportNewTxPacketsOrCredits();

  // Tell `L2capChannelManager` to try and send all available queued
  // packets. When calling this method, ensure no locks are held that are
  // also acquired in `Dequeue()` overrides, and that the channels lock is
  // not held either.
  void DrainChannelQueuesIfNewTx();

  // Returns the next Tx H4 based on the given `payload`, and sets
  // `keep_payload` to false if payload should be discard, e.g. if the returned
  // PDU completes the transmission of a payload. If no Tx H4s can be generated,
  // either due to a lack of payload or available H4 packets, returns
  // std::nullopt.
  //
  // Subclasses should override to generate correct H4 packet from their
  // payload.
  std::optional<H4PacketWithH4> GenerateNextTxPacket(
      const FlatConstMultiBuf& payload, bool& keep_payload)
      PW_EXCLUSIVE_LOCKS_REQUIRED(impl_.mutex_);

  Status SendAdditionalRxCredits(uint16_t additional_rx_credits);

  //--------------
  //  Rx (private)
  //--------------

  constexpr MultiBufAllocator* rx_multibuf_allocator() const {
    return rx_multibuf_allocator_;
  }

  Status ReplenishRxCredits(uint16_t credits);

  //-------------
  //  Rx recombine - private, used by friend Recombiner only
  //-------------
  // TODO: https://pwbug.dev/404094475 - This section (and underlying data
  // member) could eventually move up into a AclChannel class.

  // The Rx combine functions private and only used by friend Recombiner.
  friend class Recombiner;

  // Create MultiBuf that Recombiner can use to store in-progress payload when
  // being recombined.
  // Allocate extra_header_size at front of the multibuf for possible use in
  // headers if needed. That extra is discarded so it will not be part of
  // initial MultiBuf view returned by TakeBuf.
  pw::Status StartRecombinationBuf(Direction direction,
                                   size_t payload_size,
                                   size_t extra_header_size);

  // Returns true if this channel has a recombination MultiBuf (which means
  // recombination is active for this channel).
  bool HasRecombinationBuf(Direction direction) {
    return GetRecombinationBufOptRef(direction).has_value();
  }

  // Return the recombination buf to the caller.
  //
  // Channel no longer has buf after this call.
  MultiBufInstance TakeRecombinationBuf(Direction direction) {
    PW_ASSERT(GetRecombinationBufOptRef(direction).has_value());
    return std::exchange(GetRecombinationBufOptRef(direction), std::nullopt)
        .value();
  }

  // Copy the passed span to the recombination buf.
  //
  // Precondition: `HasCombinationBuf()`.
  pw::Status CopyToRecombinationBuf(Direction direction,
                                    ConstByteSpan data,
                                    uint16_t write_offset) {
    PW_ASSERT(HasRecombinationBuf(direction));
    auto& bufopt_ref = GetRecombinationBufOptRef(direction);
    MultiBuf& mbuf = MultiBufAdapter::Unwrap(bufopt_ref.value());
    size_t copied = MultiBufAdapter::Copy(mbuf, write_offset, as_bytes(data));
    return copied < data.size() ? Status::ResourceExhausted() : OkStatus();
  }

  // Return reference to the recombination MultiBuf.
  //
  // Intended for use just within L2capChannel functions.
  std::optional<MultiBufInstance>& GetRecombinationBufOptRef(
      Direction direction) {
    return recombination_mbufs_[cpp23::to_underlying(direction)];
  }

  // Destroy the recombination MultiBuf.
  void EndRecombinationBuf(Direction direction);

  //-------------
  // Link - private, used by friends only
  //-------------

  [[nodiscard]] bool IsStale() const { return impl_.IsStale(); }

  //-------------
  // Handles and keys for maps
  //-------------

  // Mappable handle to the internal channel.
  //
  // An intrusively mapped item can only be in one map at a time. Since channels
  // are mapped by keys derived from both local and remote CIDs, the channels
  // are not mappable items theselves, but have a handle for each map.
  class Handle : public IntrusiveMap<uint32_t, Handle>::Pair {
   public:
    constexpr Handle(L2capChannel& channel, uint32_t key)
        : IntrusiveMap<uint32_t, Handle>::Pair(key), channel_(channel) {}

    constexpr uint16_t connection_handle() const {
      return static_cast<uint16_t>(key() >> 16);
    }

    constexpr uint16_t channel_id() const {
      return static_cast<uint16_t>(key() & 0xFFFF);
    }

    constexpr L2capChannel* get() { return &channel_; }
    constexpr const L2capChannel* get() const { return &channel_; }

    constexpr L2capChannel& operator*() { return *get(); }
    constexpr const L2capChannel& operator*() const { return *get(); }

    constexpr L2capChannel* operator->() { return get(); }
    constexpr const L2capChannel* operator->() const { return get(); }

   private:
    L2capChannel& channel_;
  };

  // Produces a key from a connection handle and local or remote CID that can be
  // used to look up a channel handle in the manager's local or remote channel
  // map, respectively.
  static constexpr uint32_t MakeKey(uint16_t connection_handle, uint16_t cid) {
    return (uint32_t(connection_handle) << 16) | cid;
  }

  Handle& local_handle() { return local_handle_; }
  Handle& remote_handle() { return remote_handle_; }

  internal::L2capChannelImpl& impl() { return impl_; }

  //--------------
  //  Data members
  //--------------

  internal::RxEngine& rx_engine() PW_EXCLUSIVE_LOCKS_REQUIRED(rx_mutex_);
  internal::TxEngine& tx_engine() PW_EXCLUSIVE_LOCKS_REQUIRED(impl_.mutex_);

  L2capChannelManager& l2cap_channel_manager_;

  State state_ PW_GUARDED_BY(impl_.mutex_) = State::kNew;

  const AclTransportType transport_;

  // L2CAP channel handle using the ID of local endpoint.
  Handle local_handle_;

  // L2CAP channel handle using the ID of remote endpoint.
  Handle remote_handle_;

  // Notify clients of asynchronous events encountered such as errors.
  const ChannelEventCallback event_fn_;

  // Optional client-provided allocator for MultiBufs.
  MultiBufAllocator* rx_multibuf_allocator_ = nullptr;

  // Client-provided controller read callback.
  FromControllerFn from_controller_fn_;

  // Client-provided host read callback.
  FromHostFn from_host_fn_;

  std::variant<std::monostate,
               internal::BasicModeTxEngine,
               internal::CreditBasedFlowControlTxEngine,
               internal::GattNotifyTxEngine>
      tx_engine_ PW_GUARDED_BY(impl_.mutex_);

  internal::Mutex rx_mutex_ PW_ACQUIRED_BEFORE(impl_.mutex_);
  std::variant<std::monostate,
               internal::BasicModeRxEngine,
               internal::CreditBasedFlowControlRxEngine,
               internal::GattNotifyRxEngine>
      rx_engine_ PW_GUARDED_BY(rx_mutex_);

  // Recombination MultiBufs used by Recombiner to store in-progress
  // payloads when they are being recombined.
  // They are stored here so that they can be allocated with the channel's
  // allocator and also properly destroyed with the channel.
  std::array<std::optional<MultiBufInstance>, kNumDirections>
      recombination_mbufs_{};

  // Implementation-specific details that may vary between sync and async modes.
  internal::L2capChannelImpl impl_;
};

}  // namespace pw::bluetooth::proxy
