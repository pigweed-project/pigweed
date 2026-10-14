// Copyright 2026 The Pigweed Authors
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

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include "pw_allocator/allocator.h"
#include "pw_assert/check.h"
#include "pw_async2/context.h"
#include "pw_async2/future.h"
#include "pw_async2/poll.h"
#include "pw_async2/task.h"
#include "pw_async2/waker.h"
#include "pw_buf/buf.h"
#include "pw_containers/dynamic_deque.h"
#include "pw_containers/intrusive_forward_list.h"
#include "pw_rpc2/internal/call.h"
#include "pw_rpc2/internal/future_base.h"
#include "pw_rpc2/internal/handshake.h"
#include "pw_rpc2/internal/packet.h"
#include "pw_rpc2/internal/protocol_error.h"
#include "pw_rpc2/status.h"
#include "pw_transport/socket.h"

namespace pw::rpc2::internal {

/// Composite future that asynchronously reserves egress buffer space, formats a
/// payload-free control packet (a client/server error or stream end), and
/// commits it to the transport connection.
///
/// Resolves to `kOk` once the packet is committed, or `kDisconnected` if the
/// transport closed first.
class SendControlPacketFuture : public FutureBase {
 public:
  using value_type = RpcStatus;

  constexpr SendControlPacketFuture() = default;

  SendControlPacketFuture(transport::ReliableDatagramSocket connection,
                          OutboundPacket packet)
      : FutureBase(async2::FutureState::kPending),
        packet_(packet),
        reserve_fut_(connection.ReserveWrite(packet.payload_offset())) {}

  SendControlPacketFuture(SendControlPacketFuture&&) noexcept = default;
  SendControlPacketFuture& operator=(SendControlPacketFuture&&) noexcept =
      default;
  SendControlPacketFuture(const SendControlPacketFuture&) = delete;
  SendControlPacketFuture& operator=(const SendControlPacketFuture&) = delete;

  async2::Poll<RpcStatus> Pend(async2::Context& cx);

 private:
  OutboundPacket packet_;
  transport::ReserveWriteFuture reserve_fut_;
};

static_assert(async2::Future<SendControlPacketFuture>);

class ConnectionTask;

/// Asynchronous task driving I/O, demuxing, and control-packet egress for a
/// single transport connection.
///
/// @note **Threading.** A `ConnectionTask` and everything reachable from it ---
/// its calls, their handles, and their futures --- are bound to the dispatcher
/// that polls it, and may only be touched from that dispatcher's thread. This
/// includes creating and destroying calls, queueing control packets, and
/// closing the connection. See the threading contract in the `pw_rpc2`
/// documentation; there are no thread-safe entry points on this class.
///
/// @note **Head-of-line blocking.** If the destination call cannot accept an
/// ingress message, the packet is stashed and no further packets are read from
/// the connection until it can be delivered. A call that is slow to `Read()`
/// therefore stalls every other call sharing the connection.
class ConnectionTask : public async2::Task {
 public:
  ~ConnectionTask() override;

  void Wake() { waker_.Wake(); }

  /// Queues a protocol error packet for `call_id`. The packet is written
  /// immediately if possible, and otherwise queued for the dispatcher. If the
  /// queue cannot be grown, the connection is closed instead.
  ///
  /// @pre Must be called from this connection's dispatcher thread.
  void QueueError(uint32_t call_id, ProtocolError error) {
    QueueControlPacket(OutboundPacket::Error(role_, call_id, error));
  }

  /// Queues a `ProtocolError::kCancelled` packet for `call_id`, which tells the
  /// peer that application code cancelled the call. Only application cancels
  /// send it; every other condition has its own code.
  ///
  /// @pre Must be called from this connection's dispatcher thread.
  void QueueCancel(uint32_t call_id) {
    QueueError(call_id, ProtocolError::kCancelled);
  }

  /// Queues the packet that finishes this endpoint's stream for `call_id`
  /// normally. Behaves as `QueueError`.
  ///
  /// A client half-closes its stream; a server ends the RPC with `kOk`. See
  /// `OutboundPacket::Finish()`.
  ///
  /// @pre Must be called from this connection's dispatcher thread.
  void QueueFinish(uint32_t call_id) {
    QueueControlPacket(OutboundPacket::Finish(role_, call_id));
  }

  /// Closes the transport connection and completes every call on it with
  /// `reason`, which is one of:
  ///
  /// - `kClosed`: the user closed the `Client` or `Server`.
  /// - `kDisconnected`: the transport closed, the client ran out of call IDs,
  ///   or a control packet could not be queued for lack of memory.
  /// - `kProtocolError`: the peer sent a malformed packet, or the handshake
  ///   failed.
  ///
  /// @pre Must be called from this connection's dispatcher thread, or after
  /// this task is deregistered.
  void CloseConnection(RpcStatus reason);

  /// @pre Must be called from this connection's dispatcher thread.
  transport::ReliableDatagramSocket& connection() { return connection_; }
  Allocator& allocator() { return allocator_; }
  EndpointRole role() const { return role_; }
  bool is_closed() const { return state() == State::kClosed; }
  bool is_handshake_complete() const { return state() == State::kActive; }
  RpcStatus close_status() const { return close_status_; }
  const HandshakeInfo& handshake_info() const { return handshake_info_; }

  // --- Call management, scoped strictly to this connection ---

  /// Adds `call` to this connection's registry.
  ///
  /// @pre Must be called from this connection's dispatcher thread.
  void RegisterCall(Call& call) {
    calls_.push_front(call);
    // A server polls its calls, so it must notice the new one. A client has
    // nothing new to do until the call writes or a packet arrives.
    if (role() == EndpointRole::kServer) {
      Wake();
    }
  }

  /// Removes `call` from this connection's registry.
  ///
  /// @pre Must be called from this connection's dispatcher thread.
  void UnregisterCall(Call& call) { calls_.remove(call); }

 protected:
  enum class State : uint8_t {
    kHandshaking,
    kActive,
    kClosed,
  };

  ConnectionTask(transport::ReliableDatagramSocket connection,
                 Allocator& allocator,
                 EndpointRole role);

  ConnectionTask(EstablishedConnection established_connection,
                 Allocator& allocator,
                 EndpointRole role);

  State state() const { return state_; }
  void set_state(State state) { state_ = state; }
  IntrusiveForwardList<Call>& calls() { return calls_; }
  const IntrusiveForwardList<Call>& calls() const { return calls_; }

  /// Transitions from `State::kHandshaking` to `State::kActive`, publishing the
  /// parameters produced by the handshake.
  void FinishHandshake(HandshakeInfo info);

  /// Deregisters this task from the dispatcher and closes the connection.
  /// Derived destructors must call this before destroying their own state.
  void Teardown() {
    Deregister();
    CloseConnection(RpcStatus::Closed());
  }

  /// Handles up to `kMaxPacketsPerPoll` packets. Returns true if more may be
  /// ready.
  ///
  /// @pre The handshake has completed and the connection is open.
  bool PendPackets(async2::Context& cx);

 private:
  async2::Poll<> DoPend(async2::Context& cx) final;

  /// Polls an open connection. Returns true if more work may be ready.
  virtual bool PollOpenConnection(async2::Context& cx) = 0;

  /// Handles a request to start a new call.
  virtual void HandleIncomingRequest(InboundPacket&& packet) = 0;

  /// Cleans up after the connection closes. Derived destructors also call
  /// this, so it must be idempotent.
  virtual void OnClosed() = 0;

  /// Registers this task's waker for connection activity.
  void StoreWaker(async2::Context& cx);

  static constexpr int kMaxPacketsPerPoll = 4;

  /// Runs one step of outgoing control packet egress, stashed ingress dispatch,
  /// and transport reads (Stages 1--3).
  bool PendPacket(async2::Context& cx);

  bool DispatchPendingIngressPacket(async2::Context& cx);

  void QueueControlPacket(const OutboundPacket& packet);

  bool ProcessOutgoingControlPackets(async2::Context& cx);

  bool ReadPacketFromConnection(async2::Context& cx);

  Call* FindCallById(uint32_t call_id);

  /// Delivers `packet` to `call`, or returns false if the call cannot accept
  /// it yet and the packet must be stashed. In that case `cx` is woken once
  /// the call's reader catches up.
  bool DeliverToCall(Call& call, InboundPacket& packet, async2::Context& cx);

  // The transport connection.
  transport::ReliableDatagramSocket connection_;
  Allocator& allocator_;

  // The connection state machine and fixed endpoint role.
  State state_;
  EndpointRole role_;
  RpcStatus close_status_ = RpcStatus::Closed();

  // Every call active on this connection: the routing registry, keyed by call
  // ID, and for a server connection also the set of calls this task owns and
  // retires. One list suffices because a connection only ever carries calls of
  // one direction.
  IntrusiveForwardList<Call> calls_;

  HandshakeInfo handshake_info_{};

  // Ingress read & stash buffer.
  transport::ReadFuture read_future_;
  InboundPacket pending_dispatch_packet_;

  // Egress control-packet queue & dedicated future. Every queued packet is
  // terminal for its call, so dropping one would strand the peer until the
  // connection closes. The queue therefore grows on demand and only fails
  // under allocator exhaustion, which is handled as a connection error. Once
  // drained, any buffer larger than `kMaxIdleControlPacketCapacity` is
  // released.
  static constexpr size_t kMaxIdleControlPacketCapacity = 4;
  DynamicDeque<OutboundPacket> pending_control_packets_;

  SendControlPacketFuture send_control_future_;

  async2::Waker waker_;
};

}  // namespace pw::rpc2::internal
