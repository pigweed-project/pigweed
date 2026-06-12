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
#include <variant>

#include "pw_allocator/allocator.h"
#include "pw_bluetooth/hci_data.emb.h"
#include "pw_bluetooth/hci_events.emb.h"
#include "pw_bluetooth_proxy/direction.h"
#include "pw_bluetooth_proxy/internal/hci_transport.h"
#include "pw_bluetooth_proxy/internal/logical_transport.h"
#include "pw_bluetooth_proxy/internal/mutex.h"
#include "pw_containers/dynamic_map.h"
#include "pw_containers/dynamic_queue.h"
#include "pw_containers/vector.h"
#include "pw_function/function.h"
#include "pw_multibuf/multibuf.h"
#include "pw_status/status.h"
#include "pw_sync/lock_annotations.h"

namespace pw::bluetooth::proxy {

// Represents the Bluetooth ACL Data channel and tracks the Host<->Controller
// ACL data flow control.
//
// This currently only supports LE Packet-based Data Flow Control as defined in
// Core Spec v5.0, Vol 2, Part E, Section 4.1.1. Does not support sharing BR/EDR
// buffers.
class AclDataChannel {
 public:
  /// The delegate interface for an ACL connection. This is implemented by
  /// L2capLogicalLink.
  class ConnectionDelegate {
   public:
    struct HandleAclDataReturn {
      /// True if the packet was consumed and should not be forwarded.
      bool handled;
      /// If this ACL packet was the last fragment of a fragmented PDU, an ACL
      /// packet containing the recombined PDU should be returned if it was not
      /// consumed and needs to be forwarded.
      std::optional<multibuf::MultiBuf> recombined_buffer;
    };

    virtual ~ConnectionDelegate() = default;

    /// Called by AclDataChannel when an ACL packet for this connection is sent
    /// from the host or received from the controller.
    virtual HandleAclDataReturn HandleAclData(
        Direction direction, emboss::AclDataFrameWriter& acl) = 0;
  };

  // Used to `SendAcl` packets.
  class SendCredit {
   public:
    friend class AclDataChannel;

    SendCredit(const SendCredit&) = delete;
    SendCredit& operator=(const SendCredit&) = delete;
    // Move-only
    SendCredit(SendCredit&& other);
    SendCredit& operator=(SendCredit&& other);

    ~SendCredit();

   private:
    // Dispensed via `AclDataChannel::ReserveSendCredit()`.
    SendCredit(AclTransportType transport,
               Function<void(AclTransportType transport)>&& relinquish_fn);

    // Indicate that credits has been used for Tx.
    void MarkUsed();

    AclTransportType transport_;
    // If `this` was not used or moved and is destructed, `relinquish_fn_` is
    // called to replenish the credit that was subtracted from `AclDataChannel`.
    Function<void(AclTransportType transport)> relinquish_fn_;
  };

  /// When using this constructor, both host and proxy share the single pool of
  /// available credits from the controller. Packets are queued when credits
  /// are exhausted.
  AclDataChannel(HciTransport& hci_transport,
                 pw::Allocator& allocator,
                 Closure on_tx_credits_fn)
      : hci_transport_(hci_transport),
        le_credits_(allocator),
        br_edr_credits_(allocator),
        connection_delegates_(allocator),
        allocator_(allocator),
        on_tx_credits_fn_(std::move(on_tx_credits_fn)) {}

  /// When using this constructor, a fixed number of credits are reserved for
  /// the proxy, and the rest are passed to the host.
  AclDataChannel(HciTransport& hci_transport,
                 uint16_t le_acl_credits_to_reserve,
                 uint16_t br_edr_acl_credits_to_reserve,
                 pw::Allocator& allocator,
                 Closure on_tx_credits_fn)
      : hci_transport_(hci_transport),
        le_credits_(le_acl_credits_to_reserve),
        br_edr_credits_(br_edr_acl_credits_to_reserve),
        connection_delegates_(allocator),
        allocator_(allocator),
        on_tx_credits_fn_(std::move(on_tx_credits_fn)) {}

  AclDataChannel(const AclDataChannel&) = delete;
  AclDataChannel& operator=(const AclDataChannel&) = delete;
  AclDataChannel(AclDataChannel&&) = delete;
  AclDataChannel& operator=(AclDataChannel&&) = delete;

  // Handle HCI ACL data packet from the controller.
  void HandleAclFromController(H4PacketWithHci&& h4_packet);

  // Handle HCI ACL data packet from the host.
  void HandleAclFromHost(H4PacketWithH4&& h4_packet);

  // Returns the max number of active ACL connections supported.
  static constexpr size_t GetMaxNumAclConnections() { return kMaxConnections; }

  // Revert to uninitialized state, clearing credit reservation and connections,
  // but not the number of credits to reserve nor HCI transport.
  void Reset();

  /// Registers a connection delegate.
  /// @returns `OkStatus()` on success. On failure returns:
  /// * @ALREADY_EXISTS: A delegate is already registered for the connection.
  /// * @RESOURCE_EXHAUSTED: A memory allocation failed.
  Status RegisterConnection(uint16_t handle, ConnectionDelegate& delegate);

  /// Unregisters a connection delegate.
  /// @returns `OkStatus()` on success. On failure returns:
  /// * @NOT_FOUND: The delegate was not found.
  Status UnregisterConnection(uint16_t handle);

  void ProcessReadBufferSizeCommandCompleteEvent(
      emboss::ReadBufferSizeCommandCompleteEventWriter read_buffer_event);

  // Acquires LE ACL credits for proxy host use by removing the amount needed
  // from the amount that is passed to the host.
  void ProcessLEReadBufferSizeCommandCompleteEvent(
      emboss::LEReadBufferSizeV1CommandCompleteEventWriter read_buffer_event) {
    ProcessSpecificLEReadBufferSizeCommandCompleteEvent(read_buffer_event);
  }

  void ProcessLEReadBufferSizeCommandCompleteEvent(
      emboss::LEReadBufferSizeV2CommandCompleteEventWriter read_buffer_event) {
    ProcessSpecificLEReadBufferSizeCommandCompleteEvent(read_buffer_event);
  }

  // Remove completed packets from `nocp_event` as necessary to reclaim LE ACL
  // credits that are associated with our credit-allocated connections.
  bool HandleNumberOfCompletedPacketsEvent(H4PacketWithHci& h4_packet);

  // Drains the ACL queue for the given transport using fair round-robin
  // dynamic sharing between the host and proxy.
  void DrainDynamicQuota(AclTransportType transport);

  // Reclaim any credits we have associated with the removed connection.
  void ProcessDisconnectionCompleteEvent(uint16_t connection_handle,
                                         emboss::StatusCode reason);

  // Create new tracked connection.
  void HandleConnectionCompleteEvent(uint16_t connection_handle,
                                     AclTransportType transport);

  /// Indicates whether the proxy has the capability of sending ACL packets.
  /// Note that this indicates intention, so it can be true even if the proxy
  /// has not yet or has been unable to reserve credits from the host.
  bool HasSendAclCapability(AclTransportType transport) const;

  /// @deprecated Use HasSendAclCapability with transport parameter instead.
  bool HasSendAclCapability() const {
    return HasSendAclCapability(AclTransportType::kLe);
  }

  // Returns the number of available ACL send credits for the proxy.
  // Can be zero if the controller has not yet been initialized by the host.
  uint16_t GetNumFreeAclPackets(AclTransportType transport) const;

  // In order to `SendAcl`, a `SendCredit` for the desired transport must be
  // provided.
  //
  // Returns std::nullopt if no credits are available for the desired transport.
  std::optional<SendCredit> ReserveSendCredit(AclTransportType transport);

  // Send an ACL data packet contained in an H4 packet to the controller.
  // Requires a reserved `SendCredit` that matches the transport of the
  // connection on which `h4_packet` is to be sent.
  //
  // Returns PW_STATUS_UNAVAILABLE if no ACL send credits were available.
  // Returns PW_STATUS_INVALID_ARGUMENT if ACL packet was ill-formed or `credit`
  // was provided for the wrong transport. See logs.
  // Returns PW_NOT_FOUND if ACL connection does not exist.
  pw::Status SendAcl(H4PacketWithH4&& h4_packet, SendCredit&& credit);

  // Register a new logical link on ACL logical transport.
  //
  // Returns PW_STATUS_OK if a connection was added.
  // Returns PW_STATUS_ALREADY EXISTS if a connection already exists.
  // Returns PW_STATUS_RESOURCE_EXHAUSTED if no space for additional connection.
  pw::Status CreateAclConnection(uint16_t connection_handle,
                                 AclTransportType transport);

  // Returns the max ACL payload size if the Read Buffer Size command complete
  // event was received.
  std::optional<uint16_t> MaxDataPacketLengthForTransport(
      AclTransportType transport) const;

  // Returns true if an ACL connection exists with the handle
  // `connection_handle`.
  bool HasAclConnection(uint16_t connection_handle);

 private:
  /// Source of an ACL packet sent to the controller.
  enum class PacketSource : uint8_t {
    /// Packet was sent from the host (and passed through by proxy).
    kHost,
    /// Packet was sent by the proxy.
    kProxy,
  };

  // An active logical link on ACL logical transport.
  class AclConnection {
   public:
    AclConnection(AclTransportType transport,
                  uint16_t connection_handle,
                  pw::Allocator& allocator);

    AclConnection(const AclConnection&) = delete;
    AclConnection& operator=(const AclConnection&) = delete;
    AclConnection(AclConnection&&) = delete;
    AclConnection& operator=(AclConnection&&) = default;

    uint16_t connection_handle() const { return connection_handle_; }

    uint16_t num_proxy_pending_packets() const {
      return num_proxy_pending_packets_;
    }

    uint16_t num_host_pending_packets() const {
      return num_host_pending_packets_;
    }

    AclTransportType transport() const { return transport_; }

    /// Record a packet being sent to the controller.
    void RecordPacket(PacketSource source);

    /// Reclaim a credit for a packet that has completed.
    /// This will decide whether to give the credit to the host or proxy.
    PacketSource ReclaimCredit();

    pw::DynamicQueue<H4PacketWithH4>& queue() { return queue_; }
    const pw::DynamicQueue<H4PacketWithH4>& queue() const { return queue_; }

   private:
    AclTransportType transport_;
    uint16_t connection_handle_;
    uint16_t num_proxy_pending_packets_ = 0;
    uint16_t num_host_pending_packets_ = 0;
    PacketSource last_reclaimed_ = PacketSource::kProxy;
    pw::DynamicQueue<H4PacketWithH4> queue_;
  };

  class Credits {
   public:
    struct Static {
      const uint16_t to_reserve;
      // The local number of HCI ACL Data packets that we have reserved for
      // this proxy host to use.
      uint16_t proxy_max = 0;
    };

    struct Dynamic {
      PacketSource drain_turn = PacketSource::kProxy;
      uint16_t total_queued_packets = 0;
    };

    explicit Credits(uint16_t to_reserve)
        : data_(std::in_place_type<Static>, Static{to_reserve}) {}

    explicit Credits(pw::Allocator&)
        : data_(std::in_place_type<Dynamic>, Dynamic{}) {}

    void Reset();

    // Reserves credits from the controllers max and returns how many the host
    // can use.
    uint16_t Reserve(uint16_t controller_max);

    // Mark `num_credits` as pending.
    //
    // Returns Status::ResourceExhausted() if there aren't enough available
    // credits.
    Status MarkPending(uint16_t num_credits, PacketSource source);

    // Return `num_credits` to available pool.
    void MarkCompleted(uint16_t num_credits);

    uint16_t Remaining() const {
      if (std::holds_alternative<Dynamic>(data_)) {
        if (pending_ >= controller_max_packets_) {
          return 0;
        }
        return controller_max_packets_ - pending_;
      }
      return std::get<Static>(data_).proxy_max - pending_;
    }
    bool Available() const { return Remaining() > 0; }

    // If this class was initialized with some number of credits to reserve or
    // if dynamic sharing is enabled.
    bool HasSendCapability() const {
      if (std::holds_alternative<Dynamic>(data_)) {
        return true;
      }
      return std::get<Static>(data_).to_reserve > 0;
    }

    // If this class has already had credits reserved from the controller.
    bool Initialized() const { return controller_max_packets_ > 0; }

    uint16_t controller_max_packets() const { return controller_max_packets_; }

    bool dynamic_sharing() const {
      return std::holds_alternative<Dynamic>(data_);
    }

    uint16_t total_queued_packets() const {
      if (const auto* dynamic = std::get_if<Dynamic>(&data_)) {
        return dynamic->total_queued_packets;
      }
      return 0;
    }

    void IncrementTotalQueuedPackets() {
      if (auto* dynamic = std::get_if<Dynamic>(&data_)) {
        dynamic->total_queued_packets++;
      }
    }

    void DecrementTotalQueuedPackets(uint16_t count) {
      if (auto* dynamic = std::get_if<Dynamic>(&data_)) {
        if (count > dynamic->total_queued_packets) {
          dynamic->total_queued_packets = 0;
        } else {
          dynamic->total_queued_packets -= count;
        }
      }
    }

    uint16_t pending() const { return pending_; }

    PacketSource drain_turn() const {
      if (auto* dynamic = std::get_if<Dynamic>(&data_)) {
        return dynamic->drain_turn;
      }
      return PacketSource::kProxy;
    }

    void PassTurn() {
      if (auto* dynamic = std::get_if<Dynamic>(&data_)) {
        dynamic->drain_turn = (dynamic->drain_turn == PacketSource::kProxy)
                                  ? PacketSource::kHost
                                  : PacketSource::kProxy;
      }
    }

   private:
    std::variant<Static, Dynamic> data_;
    // The number of HCI ACL Data packets that we have sent to the controller
    // and have not yet completed (for proxy only in static mode, or host +
    // proxy in dynamic mode).
    uint16_t pending_ = 0;
    // The maximum number of HCI ACL Data packets the controller can hold.
    uint16_t controller_max_packets_ = 0;
  };

  // Handles an ACL Data frame.
  // Returns true if the frame was handled and is consumed by the proxy.
  // Returns false if the frame should be passed on to the other side.
  bool HandleAclData(Direction direction, pw::span<uint8_t> buffer);

  void HandleAclFromHostLocked(H4PacketWithH4&& h4_packet,
                               AclConnection* connection_ptr,
                               std::optional<H4PacketWithH4>& packet_to_send,
                               std::optional<H4PacketWithH4>& packet_to_drop)
      PW_EXCLUSIVE_LOCKS_REQUIRED(connection_mutex_);

  // Guards interactions with ACL connection objects.
  mutable internal::Mutex connection_mutex_ PW_ACQUIRED_BEFORE(credit_mutex_);

  // Returns pointer to AclConnection with provided `connection_handle` in
  // `acl_connections_`. Returns nullptr if no such connection exists.
  //
  // Pointer is assured valid only as long as `mutex_` is held.
  AclConnection* FindAclConnection(uint16_t connection_handle)
      PW_EXCLUSIVE_LOCKS_REQUIRED(connection_mutex_);

  AclConnection* FindConnectionToDrain(AclTransportType transport)
      PW_EXCLUSIVE_LOCKS_REQUIRED(connection_mutex_);

  std::optional<H4PacketWithH4> DequeueHostPacket(AclConnection* connection,
                                                  Credits& credits)
      PW_EXCLUSIVE_LOCKS_REQUIRED(connection_mutex_, credit_mutex_);

  Credits& LookupCredits(AclTransportType transport)
      PW_EXCLUSIVE_LOCKS_REQUIRED(credit_mutex_);

  const Credits& LookupCredits(AclTransportType transport) const
      PW_EXCLUSIVE_LOCKS_REQUIRED(credit_mutex_);

  // Data members

  // Maximum number of active ACL connections supported.
  // TODO: https://pwbug.dev/349700888 - Make size configurable.
  static constexpr size_t kMaxConnections = 10;

  // Reference to the transport owned by the host.
  HciTransport& hci_transport_;

  // Credit allocation will happen inside a mutex since it crosses thread
  // boundaries.
  mutable internal::Mutex credit_mutex_ PW_ACQUIRED_AFTER(connection_mutex_);

  Credits le_credits_ PW_GUARDED_BY(credit_mutex_);
  Credits br_edr_credits_ PW_GUARDED_BY(credit_mutex_);

  std::optional<uint16_t> max_acl_data_packet_length_
      PW_GUARDED_BY(credit_mutex_);
  std::optional<uint16_t> max_le_acl_data_packet_length_
      PW_GUARDED_BY(credit_mutex_);

  // List of credit-allocated ACL connections.
  pw::Vector<AclConnection, kMaxConnections> acl_connections_
      PW_GUARDED_BY(connection_mutex_);

  // This separate mutex is required because the delegate may call SendAcl() and
  // acquire the connection_mutex_ inside of delegate callbacks.
  internal::Mutex delegates_mutex_ PW_ACQUIRED_BEFORE(connection_mutex_);

  // Guards the draining process to serialize SendToController calls.
  mutable internal::Mutex draining_mutex_ PW_ACQUIRED_BEFORE(connection_mutex_);

  DynamicMap<uint16_t, ConnectionDelegate*> connection_delegates_
      PW_GUARDED_BY(delegates_mutex_);

  pw::Allocator& allocator_;

  // Called after ACL TX credits are received.
  const Closure on_tx_credits_fn_;

  // Instantiated in acl_data_channel.cc for
  // `emboss::LEReadBufferSizeV1CommandCompleteEventWriter` and
  // `emboss::LEReadBufferSizeV1CommandCompleteEventWriter`.
  template <class EventT>
  void ProcessSpecificLEReadBufferSizeCommandCompleteEvent(
      EventT read_buffer_event);
};

}  // namespace pw::bluetooth::proxy
