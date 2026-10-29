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
#include <limits>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

#include "pw_allocator/allocator.h"
#include "pw_assert/assert.h"
#include "pw_assert/check.h"
#include "pw_async2/context.h"
#include "pw_async2/poll.h"
#include "pw_async2/waker.h"
#include "pw_buf/buf.h"
#include "pw_containers/intrusive_forward_list.h"
#include "pw_enum/traits.h"
#include "pw_intrusive_ptr/intrusive_ptr.h"
#include "pw_intrusive_ptr/recyclable.h"
#include "pw_rpc2/internal/packet.h"
#include "pw_rpc2/internal/protocol_error.h"
#include "pw_rpc2/status.h"
#include "pw_status/status.h"
#include "pw_transport/socket.h"

namespace pw::rpc2 {
class Client;
}  // namespace pw::rpc2

namespace pw::rpc2::internal {

class ConnectionTask;
class Call;

/// State shared by the two ends of a single RPC invocation.
///
/// A `Call` owns the single-slot ingress buffer for one call ID, tracks
/// whether each direction of the call is still open, and provides the egress
/// reservation entry point used by `Writer` and `UnaryWriter`.
///
/// @note **Threading.** A `Call` belongs to its connection's dispatcher.
/// Creating it, polling it, handing packets to it, and destroying it all
/// happen on that dispatcher's thread, as does every operation on the public
/// handles that wrap it. It holds no lock and needs none. See the threading
/// contract in the `pw_rpc2` documentation.
///
/// @note **Ownership.** Every `Call` is intrusively reference-counted via
/// `IntrusivePtr<Call>` and `Recyclable<Call>`, and every handle holds a
/// reference, so a handle can never outlive the call it refers to. While
/// registered in its connection's call list, a call refers back to its
/// `ConnectionTask`. Retiring the call or tearing the connection down detaches
/// it (replacing the connection pointer with the connection's `Allocator*` for
/// eventual deallocation), after which its outbound operations are no-ops and
/// `ReserveWrite()` fails. A completed call reports why it completed (see
/// `ReserveWrite()`); a detached call that did not complete reports
/// `kWriteClosed`.
class Call : public IntrusiveForwardList<Call>::Item, public Recyclable<Call> {
 public:
  virtual ~Call();

  /// True while this call is registered with a connection. A detached call's
  /// outbound operations are no-ops.
  bool is_attached() const { return HasReadFlag<kAttached>(); }

  /// @pre The call must not have been detached from its connection.
  ConnectionTask& connection_task() const {
    PW_DASSERT(is_attached());
    return *connection_task_;
  }
  uint32_t call_id() const { return call_id_; }
  EndpointRole role() const {
    return HasWriteFlag<kServerCall>() ? EndpointRole::kServer
                                       : EndpointRole::kClient;
  }

  /// Queues a protocol error packet for this call on its connection task. The
  /// packet is written immediately if the transport has room, and otherwise
  /// queued until it does. If the queue cannot grow, the connection is closed.
  /// No-op if the call is detached.
  void QueueError(ProtocolError error);

  /// Queues the packet that finishes this call's outbound stream normally on
  /// its connection task: a client stream end, or a server's successful end of
  /// the RPC. Delivered as `QueueError`.
  void QueueFinish();

  bool is_read_closed() const { return HasReadFlag<kIngressClosed>(); }
  bool is_write_closed() const { return HasWriteFlag<kClosed>(); }

  /// Closes the write side of the call.
  ///
  /// A server always ends the RPC when it closes its outbound stream: the
  /// terminal packet it sends (`CloseMode::kOkTerminal` or `kErrorTerminal`)
  /// completes the call on the client. A server call therefore stops accepting
  /// inbound messages here too, so that it closes immediately rather than
  /// waiting for a client stream end that will never come. Messages already
  /// queued remain readable.
  void CloseWrite();
  [[nodiscard]] bool is_closed() const {
    return (is_read_closed() && is_write_closed()) || is_completed();
  }

  /// Closes the read side of the call, dropping any buffered inbound message
  /// and waking the connection if it was waiting for a receive slot.
  ///
  /// Cancels a client call that has not completed, since nothing will observe
  /// its outcome.
  void CloseRead();

  /// Marks that the call's `Reader` (or single-message read handle) has been
  /// destroyed, closing the read channel immediately unless a `ReadFuture` is
  /// still in flight (in which case `ReleaseRead()` closes it when done).
  void CloseReadOnReaderDestroy();

  /// True if the peer cleanly half-closed its outbound stream
  /// (`CloseMode::kStreamEnd`), after which it sends no more messages.
  bool peer_ended_stream() const { return HasReadFlag<kPeerClosed>(); }

  /// Marks a client call as expecting exactly one response (unary and
  /// client-streaming RPCs). Every non-error packet the server sends for such
  /// a call must carry the response and terminate the RPC; anything else means
  /// the server disagrees about the method's type.
  void ExpectSingleResponse() {
    PW_DASSERT(role() == EndpointRole::kClient);
    SetReadFlag<kSingleResponse>();
  }
  bool expects_single_response() const {
    return HasReadFlag<kSingleResponse>();
  }

  /// Marks that a terminal write reservation has been initiated for this call.
  void BeginTerminalWrite() {
    PW_DASSERT(!is_write_closed() && !has_pending_terminal_write());
    SetWriteFlag<kTerminalWritePending>();
  }

  /// Marks that a pending terminal write reservation was committed, closing the
  /// write side of the call.
  void CommitTerminalWrite() { CloseWrite(); }

  /// Marks that a pending terminal write reservation was dropped without being
  /// committed. Executes any deferred destructor fallback if the owning handle
  /// was already destroyed.
  void AbandonTerminalWrite();

  bool has_pending_terminal_write() const {
    return HasWriteFlag<kTerminalWritePending>();
  }

  /// Marks that this client call's start packet was committed. Until then,
  /// cancelling sends nothing.
  void MarkStarted() { SetWriteFlag<kStarted>(); }

  /// Sends `ProtocolError::kCancelled` to the peer and completes the call
  /// locally with `RpcCode::kCancelled`. In-flight write reservations fail to
  /// commit.
  ///
  /// No-op if the call completed, or if it is a server call that already sent
  /// its terminal packet.
  void Cancel();

  /// Ends a retired server call whose write side is still open because a
  /// `Writer` or `UnaryWriter` escaped its method. Sends
  /// `ProtocolError::kMethodReturnedWithoutFinishing` and closes the write
  /// side. No-op if the write side is closed.
  void CloseWriteOnRetire();

  void set_error_on_writer_drop(bool error) {
    if (error) {
      SetWriteFlag<kErrorOnWriterDrop>();
    } else {
      ClearWriteFlag<kErrorOnWriterDrop>();
    }
  }

  /// Runs the RAII destructor fallback for `Writer` and `UnaryWriter`. Queues
  /// `ProtocolError::kDroppedWithoutResponse` (if `kErrorOnWriterDrop` is
  /// set, as for `UnaryWriter`) or a finish packet (as for `Writer`:
  /// `OK_TERMINAL` from a server, `STREAM_END` from a client), deferring the
  /// packet if a terminal write or initial method invocation is still in
  /// progress.
  void CloseOnWriterDestroy();

  /// Marks this call as undergoing server method invocation or polling,
  /// deferring responder/writer destructor packets until `FinishInvocation()`.
  void BeginInvocation() {
    PW_DASSERT(HasWriteFlag<kServerCall>());
    SetWriteFlag<kInvoking>();
  }

  /// Completes server method invocation or polling, flushing any RAII
  /// destructor action triggered while `kInvoking` was active.
  void FinishInvocation();

  /// True once the call has completed.
  [[nodiscard]] bool is_completed() const {
    return completion_code_ != kUncompleted;
  }

  /// The status the call completed with.
  ///
  /// - `kOk`: the server ended the RPC successfully.
  /// - `kCancelled`: application code on either end ended the call. This end
  ///   cancelled it, or the peer sent `kCancelled`, `kDroppedWithoutResponse`,
  ///   or `kMethodReturnedWithoutFinishing`.
  /// - `kUnavailable`, `kAborted`, `kUnimplemented`, or `kDataError`: the peer
  ///   ended the call with an error code that maps to it (see
  ///   `PeerErrorToRpcStatus()`).
  /// - `kProtocolError`: an endpoint violated the protocol.
  /// - `kClosed`: the call was closed by the user.
  /// - `kDisconnected`: the underlying socket disconnected.
  ///
  /// @pre `is_completed()`.
  RpcStatus completion_status() const {
    PW_DASSERT(is_completed());
    return RpcStatus(static_cast<RpcCode>(completion_code_));
  }

  /// The status to report when an operation on this call fails because its
  /// socket closed. If the call already ended with an error, such as the
  /// `kProtocolError` that this end closed the connection with, that error is
  /// the cause. Otherwise, the socket closed on its own: `kDisconnected`.
  RpcStatus ConnectionClosedStatus() const {
    if (is_completed() && !completion_status().ok()) {
      return completion_status();
    }
    return RpcStatus::Disconnected();
  }

  /// Reserves the call's single message slot for a following `OnMessage()`.
  ///
  /// Returns true if the slot is empty, or if the read side is closed (in
  /// which case `OnMessage()` discards the payload). Returns false if a
  /// previously delivered message has not yet been consumed by the reader. In
  /// that case, this registers the connection to be woken once the slot drains
  /// or the read side closes, and the caller must ask again.
  [[nodiscard]] bool ReserveMessageSlot() {
    if (is_read_closed() || !HasReadFlag<kHasMessage>()) {
      return true;
    }
    SetReadFlag<kSenderBlocked>();
    return false;
  }

  /// True if an inbound message is buffered and waiting for the reader.
  bool has_buffered_message() const { return HasReadFlag<kHasMessage>(); }

  /// Delivers an ingress message payload to the call's buffer, or discards it
  /// if the read side is closed.
  ///
  /// @pre `ReserveMessageSlot()` returned true (a newly created call's slot is
  /// always empty).
  void OnMessage(ConstBuf payload);

  /// Handles the peer half-closing its outbound stream
  /// (`CloseMode::kStreamEnd`). The peer sends no more messages afterwards.
  ///
  /// When a client half-closes, a server call's reader observes the end of the
  /// stream once it drains any queued message.
  ///
  /// When a server half-closes, the RPC continues: the server still ends it
  /// with a terminal packet, which carries the call's final status. A client
  /// call therefore keeps its reader open until that packet arrives, so that
  /// the reader reports the actual outcome of the RPC rather than a premature
  /// end of stream.
  void OnPeerStreamEnd();
  void OnError(RpcStatus status);

  /// Completes the call with `status`, closing both directions and waking the
  /// server connection (or detaching a client call). The first completion wins,
  /// so a call that already failed keeps its original status even if the
  /// connection later closes underneath it.
  void Complete(RpcStatus status);

  /// Unregisters this call from its connection's routing registry and drops its
  /// pointer to the connection, after which all outbound operations on this
  /// call become no-ops and `ReserveWrite()` fails.
  ///
  /// Idempotent.
  void DetachFromConnection();

  /// Clears this call's pointer to its connection, replacing it with the
  /// connection's allocator so the call can still deallocate itself when its
  /// last handle is released.
  ///
  /// @pre The connection has already removed this call from its list.
  void ClearConnectionTask();

  /// Claims this call's read stream.
  ///
  /// `ReadFuture` is the call's only reader and holds the claim until the read
  /// resolves, so that a second concurrent read fails loudly instead of
  /// silently splitting the message stream between the two readers.
  void ClaimRead();

  /// Polls for the next inbound message or end-of-stream status.
  ///
  /// @pre `ClaimRead()` has been called.
  async2::Poll<RpcResult<ConstBuf>> PendRead(async2::Context& cx);

  /// Releases the claim taken by `ClaimRead()`.
  void ReleaseRead();

  /// Begins an egress reservation for `Writer` / `WriteFuture`.
  ///
  /// Fails immediately, without consulting the transport, when the call cannot
  /// write at all:
  ///
  /// - The call's completion status, if the call completed with an error: for
  ///   example `kCancelled` if the peer cancelled it, or `kDisconnected` if
  ///   the connection closed. See `is_completed()`.
  /// - `kWriteClosed`: the write side is already closed, because a terminal
  ///   packet was sent or is pending, the writer was dropped, the call
  ///   completed successfully, or the call was detached from its connection
  ///   without completing.
  /// - `kMessageTooLarge`: a packet with a `header_size`-byte header and a
  ///   `max_payload_size`-byte payload exceeds the largest datagram the
  ///   transport can write.
  ///
  /// Otherwise returns a future for the transport's reservation of
  /// `header_size + max_payload_size` bytes.
  RpcResult<transport::ReserveWriteFuture> ReserveWrite(
      size_t header_size, size_t max_payload_size);

  /// The largest packet, header included, that this call can currently
  /// reserve, or 0 if the call is detached from its connection.
  size_t max_write_size_bytes() const;

  /// The largest payload that fits in a packet with a `header_size`-byte
  /// header, or 0 if the call is detached from its connection.
  size_t max_payload_size(size_t header_size) const {
    const size_t max_packet = max_write_size_bytes();
    return max_packet > header_size ? max_packet - header_size : 0;
  }

 protected:
  /// Constructs a call attached to `connection_task` and allocated from
  /// `connection_task.allocator()`.
  Call(ConnectionTask& connection_task, uint32_t call_id);

 private:
  template <typename U>
  friend class ::pw::IntrusivePtr;
  friend class Recyclable<Call>;
  friend class ServerCall;

  static constexpr uint8_t kUncompleted = 0xff;
  static_assert(!IsValidEnum<RpcCode>(kUncompleted));

  // Use custom AddRef/ReleaseRef instead of RefCounted, since the count doesn't
  // need to be atomic. Also, a uint8_t is sufficient since the ref count peaks
  // at 5 in normal use (ServerCall, Reader, Writer, ReadFuture, WriteFuture).
  void AddRef() const {
    PW_ASSERT(ref_count_ < std::numeric_limits<uint8_t>::max());
    ++ref_count_;
  }

  [[nodiscard]] bool ReleaseRef() const { return --ref_count_ == 0; }

  [[nodiscard]] int32_t ref_count() const { return ref_count_; }

  void pw_recycle();

  enum WriteFlag : uint8_t {
    // Write side is closed (terminal packet committed/queued, or call closed).
    kClosed = 1 << 0,

    // ServerCall is inside Method::Invoke() before SetFuture(). Writer
    // destructor packets are deferred until FinishInvocation().
    kInvoking = 1 << 1,

    // A terminal reservation (UnaryWriter::Finish / ReserveFinish or
    // Writer::Finish) is in flight. Further writes are rejected, and writer
    // destructor packets are deferred until the reservation commits or is
    // abandoned.
    kTerminalWritePending = 1 << 2,

    // The call's Writer or UnaryWriter was destroyed without finishing.
    kWriterDropped = 1 << 3,

    // Dropping the writer ends the call with an error (UnaryWriter) instead of
    // sending a stream-end packet (Writer).
    kErrorOnWriterDrop = 1 << 4,

    // This call belongs to a ServerConnectionTask (EndpointRole::kServer).
    kServerCall = 1 << 5,

    // The packet that starts this client call has been committed, so the
    // server knows about the call. Always set for a server call.
    kStarted = 1 << 6,

    // The server call's method task has finished and is ready to be retired.
    kServerFinished = 1 << 7,
  };

  enum ReadFlag : uint8_t {
    // A ReadFuture currently holds the read stream via ClaimRead().
    kClaimed = 1 << 0,

    // The local Reader (or single-response handle) was destroyed or
    // CloseRead() was called. If kClaimed is set, ReleaseRead() will finish
    // closing the read channel once the in-flight ReadFuture resolves.
    kReaderDropped = 1 << 1,

    // The peer cleanly half-closed its outbound stream (CloseMode::kStreamEnd).
    kPeerClosed = 1 << 2,

    // Client call for a unary or client-streaming RPC, which accepts only a
    // single terminal response from the server.
    kSingleResponse = 1 << 3,

    // True while registered with `connection_task_` (`connection_task_` is the
    // active union member). False once detached (`allocator_` is the active
    // union member).
    kAttached = 1 << 4,

    // `ConnectionTask::DeliverToCall()` tried to deliver a message while the
    // call's single-slot buffer was full; wake `connection_task_` when the
    // slot is drained or closed.
    kSenderBlocked = 1 << 5,

    // No more inbound messages may be queued for this call. Any message already
    // buffered in `pending_message_` (`kHasMessage`) can still be drained
    // unless `CloseRead()` discarded it.
    kIngressClosed = 1 << 6,

    // True when `pending_message_` (`ConstBuf`) is the active member of the
    // ingress union; false when `read_waker_` (`async2::Waker`) is active.
    kHasMessage = 1 << 7,
  };

  template <WriteFlag kFlag>
  bool HasWriteFlag() const {
    return (write_flags_ & kFlag) != 0;
  }

  template <WriteFlag kFlag>
  void SetWriteFlag() {
    write_flags_ |= kFlag;
  }

  template <WriteFlag kFlag>
  void ClearWriteFlag() {
    write_flags_ &= ~kFlag;
  }

  template <ReadFlag kFlag>
  bool HasReadFlag() const {
    return (read_flags_ & kFlag) != 0;
  }

  template <ReadFlag kFlag>
  void SetReadFlag() {
    read_flags_ |= kFlag;
  }

  template <ReadFlag kFlag>
  void ClearReadFlag() {
    read_flags_ &= ~kFlag;
  }

  bool is_read_claimed() const { return HasReadFlag<kClaimed>(); }
  bool is_reader_dropped() const { return HasReadFlag<kReaderDropped>(); }

  void StorePendingMessage(ConstBuf&& payload) {
    std::move(read_waker_).Wake();
    read_waker_.~Waker();
    ::new (&pending_message_) ConstBuf(std::move(payload));
    SetReadFlag<kHasMessage>();
  }

  ConstBuf TakePendingMessage() {
    ConstBuf msg = std::move(pending_message_);
    pending_message_.~ConstBuf();
    ::new (&read_waker_) async2::Waker();
    ClearReadFlag<kHasMessage>();
    return msg;
  }

  void DiscardPendingMessage() {
    if (HasReadFlag<kHasMessage>()) {
      TakePendingMessage();
    }
  }

  void CloseIngress();
  void QueueWriterDropPacket();
  void FlushDeferredWriterDrop();

  // Detaches a client call or wakes the server connection when the call is
  // observed to be closed.
  void NotifyIfClosed();

  uint32_t call_id_;
  uint8_t write_flags_ = 0;
  uint8_t read_flags_ = kAttached;
  // Set by the first `Complete()`; `kUncompleted` until then.
  uint8_t completion_code_ = kUncompleted;
  mutable uint8_t ref_count_ = 0;

  // While `kAttached` is set, points to the owning `ConnectionTask`. When
  // detached (`ClearConnectionTask()`), replaced with
  // `&connection_task_->allocator()` so `pw_recycle()` can deallocate `this`
  // even after the connection is gone.
  union {
    ConnectionTask* connection_task_;
    Allocator* allocator_;
  };

  // Single-slot ingress state. A reader only parks `read_waker_` when no
  // message is buffered, and delivering a message immediately wakes and
  // consumes `read_waker_`, so the two have disjoint lifetimes:
  // - `!HasReadFlag<kHasMessage>()`: `read_waker_` is the active member.
  // - `HasReadFlag<kHasMessage>()`: `pending_message_` is the active member.
  union {
    async2::Waker read_waker_;
    ConstBuf pending_message_;
  };
};

/// Client-side RPC call state.
///
/// Detaches itself from its `ConnectionTask` as soon as the call closes. A
/// write reservation still in flight at that point is rejected when it
/// commits.
class ClientCall final : public Call {
 public:
  /// Allocates and constructs a heap-allocated `ClientCall` from
  /// `connection_task.allocator()`, managed by `IntrusivePtr<Call>`.
  static IntrusivePtr<Call> Create(ConnectionTask& connection_task,
                                   uint32_t call_id);

 private:
  friend class ::pw::Allocator;

  ClientCall(ConnectionTask& connection_task, uint32_t call_id)
      : Call(connection_task, call_id) {}
};

/// Internal access helper used to construct public RPC handles and futures
/// from their internal parts (an `internal::Call`, a `ReserveWriteFuture`)
/// without exposing those constructors publicly.
struct CallAccess {
  template <typename HandleType, typename... Args>
  static HandleType Create(Args&&... args) {
    return HandleType(std::forward<Args>(args)...);
  }

  // Constructs a client from an already-established connection, bypassing the
  // handshake. Used by test fixtures that drive a mock connection;
  // `ClientType` is always `pw::rpc2::Client`.
  template <typename ClientType, typename... Args>
  static ClientType CreateClient(Args&&... args) {
    return ClientType(std::forward<Args>(args)...);
  }

  // Allocates a bare call on a client's connection. `client` may be a `Client`
  // or anything convertible to one, such as a test fixture that wraps a
  // client. `FullClient` is a template parameter only so that `Client` need not
  // be complete here.
  template <typename ClientType, typename FullClient = Client>
  static auto CreateCall(const ClientType& client) {
    return static_cast<const FullClient&>(client).CreateCall();
  }

  // Call IDs are an internal detail that the public handles deliberately do
  // not expose. This reads the ID of the call a handle refers to.
  template <typename HandleType>
  static uint32_t call_id(const HandleType& handle) {
    return handle.call().call_id();
  }

  template <typename HandleType>
  static Call& call(const HandleType& handle) {
    return handle.call();
  }
};

}  // namespace pw::rpc2::internal
