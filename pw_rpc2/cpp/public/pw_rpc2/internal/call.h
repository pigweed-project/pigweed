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
#include <optional>
#include <type_traits>
#include <utility>

#include "pw_allocator/allocator.h"
#include "pw_assert/assert.h"
#include "pw_assert/check.h"
#include "pw_async2/channel.h"
#include "pw_async2/context.h"
#include "pw_async2/future_or_value.h"
#include "pw_async2/waker.h"
#include "pw_buf/buf.h"
#include "pw_containers/intrusive_forward_list.h"
#include "pw_intrusive_ptr/intrusive_ptr.h"
#include "pw_intrusive_ptr/recyclable.h"
#include "pw_intrusive_ptr/ref_counted.h"
#include "pw_result/result.h"
#include "pw_rpc2/internal/packet.h"
#include "pw_rpc2/internal/protocol_status.h"
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
/// A `Call` owns the ingress queue for one call ID, tracks whether each
/// direction of the call is still open, and provides the egress reservation
/// entry point used by `Writer` and `UnaryWriter`.
///
/// @note **Threading.** A `Call` belongs to its connection's dispatcher.
/// Creating it, polling it, handing packets to it, and destroying it all
/// happen on that dispatcher's thread, as does every operation on the public
/// handles that wrap it. It holds no lock and needs none. See the threading
/// contract in the `pw_rpc2` documentation.
///
/// @note **Size.** A `Call` is not small: it embeds an inline SPSC channel and
/// its storage, an egress reservation future, and a transport connection
/// handle, and one is allocated per in-flight RPC.
/// TODO: hepler@ - Audit `sizeof(Call)` and shrink it. The reservation
/// state is the most likely candidate for sharing across calls.
///
/// @note **Ownership.** Every `Call` is intrusively reference-counted via
/// `RefCounted<Call>`, `IntrusivePtr<Call>`, and `Recyclable<Call>`,
/// and every handle holds a reference, so a handle can never outlive the call
/// it refers to. A call refers back to its `ConnectionTask` via a raw pointer
/// that is non-null if and only if the call is currently registered in that
/// connection's call list. Retiring the call or tearing the connection down
/// detaches it, after which its outbound operations are no-ops and
/// `ReserveWrite()` fails. A completed call reports why it completed (see
/// `ReserveWrite()`); a detached call that did not complete reports
/// `UNAVAILABLE`.
class Call : public IntrusiveForwardList<Call>::Item,
             public RefCounted<Call>,
             public Recyclable<Call> {
 public:
  virtual ~Call();

  /// True while this call is registered with a connection. A detached call's
  /// outbound operations are no-ops.
  bool is_attached() const { return connection_task_ != nullptr; }

  /// @pre The call must not have been detached from its connection.
  ConnectionTask& connection_task() const {
    PW_DASSERT(connection_task_ != nullptr);
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
  void QueueError(ProtocolStatus error);

  /// Queues the packet that finishes this call's outbound stream normally on
  /// its connection task: a client stream end, or a server's successful end of
  /// the RPC. Delivered as `QueueError`.
  void QueueFinish();

  bool is_read_closed() const { return !sender_.is_open(); }
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
    return (!sender_.is_open() && is_write_closed()) || is_completed();
  }

  /// Closes the read side of the call (both sender and receiver), dropping any
  /// buffered inbound message and waking the connection if it was waiting for a
  /// receive slot.
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

  /// Sends `kCancelled` to the peer and completes the call locally with
  /// `Status::Cancelled()`. In-flight write reservations fail to commit.
  ///
  /// No-op if the call completed, or if it is a server call that already sent
  /// its terminal packet.
  void Cancel();

  /// Ends a retired server call whose write side is still open because a
  /// `Writer` or `UnaryWriter` escaped its method. Sends
  /// `kDroppedWithoutResponse` (single response) or `kCancelled` (streaming)
  /// and closes the write side. No-op if the write side is closed.
  void CloseWriteOnRetire();

  void set_cancel_on_writer_drop(bool cancel) {
    if (cancel) {
      SetWriteFlag<kCancelOnWriterDrop>();
    } else {
      ClearWriteFlag<kCancelOnWriterDrop>();
    }
  }

  /// Runs the RAII destructor fallback for `Writer` and `UnaryWriter`. Queues
  /// `CANCELLED` (if `kCancelOnWriterDrop` is set, as for `UnaryWriter`) or
  /// `STREAM_END` (as for `Writer`), deferring the packet if a terminal write
  /// or initial method invocation is still in progress.
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

  /// Calls complete with one of the following known statuses:
  ///
  /// - `OkStatus()`: the server ended the RPC successfully with a
  ///   `CloseMode::kOkTerminal` packet.
  /// - `Status::Cancelled()`: `ProtocolStatus::kCancelled`,
  ///   `ProtocolStatus::kDroppedWithoutResponse`,
  ///   `ProtocolStatus::kServiceUnregistered`, local/peer connection closed,
  ///   or service unregistered.
  /// - `Status::NotFound()`: `ProtocolStatus::kUnknownService` or
  ///   `ProtocolStatus::kUnknownMethod`.
  /// - `Status::DataLoss()`: `ProtocolStatus::kInvalidRequestPayload` or
  ///   response payload deserialization failure.
  /// - `Status::ResourceExhausted()`: `ProtocolStatus::kFailedToAllocateCall`,
  ///   `ProtocolStatus::kFailedToAllocateCallResourcesWhileRunning`, or local
  ///   outbound buffer allocation failure.
  /// - `Status::Unimplemented()`:
  ///   `ProtocolStatus::kReceivedPacketForWrongEndpoint`.
  /// - `Status::FailedPrecondition()`: `ProtocolStatus::kMethodTypeMismatch`,
  ///   sent by whichever side detected that the client and server disagree
  ///   about the method's type.
  /// - `Status::Internal()`: `ProtocolStatus::kInternal`.
  /// - `Status::Unavailable()`: invoked on a closed or disconnected client, or
  ///   outbound write reservation failed due to closed transport.
  /// - `Status::Unknown()`: the peer sent an unrecognized error code.
  [[nodiscard]] bool is_completed() const { return HasReadFlag<kCompleted>(); }

  /// The status the call completed with.
  ///
  /// @pre `is_completed()`.
  Status completion_status() const { return completion_status_; }

  /// Acquires a slot in the call's ingress queue, in preparation for a call to
  /// `OnMessage()`.
  ///
  /// Returns true once a slot is held, or if the read side is closed, in which
  /// case `OnMessage()` discards the payload instead of queuing it. Returns
  /// false if the reader has not consumed the previous message yet; `cx` is
  /// woken once it has, and the caller must ask again.
  [[nodiscard]] bool ReserveMessageSlot(async2::Context& cx);

  /// Delivers an ingress message payload to the call's SPSC queue.
  /// Requires a successful `ReserveMessageSlot()`.
  void OnMessage(ConstBuf payload);

  /// Queues the message carried by the packet that started this call.
  ///
  /// A newly created call's queue is always empty, so unlike `OnMessage()`
  /// this needs no prior `ReserveMessageSlot()`.
  void DeliverFirstPayloadForStream(ConstBuf payload) {
    const Status status = sender_.TrySend(std::move(payload));
    PW_DASSERT(status.ok());
  }

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
  void OnError(Status status);

  /// Completes the call with `status`, closing both directions and waking the
  /// server connection (or detaching a client call). The first completion wins,
  /// so a call that already failed keeps its original status even if the
  /// connection later closes underneath it.
  void Complete(Status status);

  /// Unregisters this call from its connection's routing registry and drops its
  /// pointer to the connection, after which all outbound operations on this
  /// call become no-ops and `ReserveWrite()` fails with `UNAVAILABLE`.
  ///
  /// Called when a server call is retired, when a client call closes, when a
  /// connection is torn down, or when the call is destroyed. Idempotent.
  void DetachFromConnection();

  /// Claims this call's read stream and starts a receive on it.
  ///
  /// `ReadFuture` is the call's only reader and holds the claim until the read
  /// resolves, so that a second concurrent read fails loudly instead of
  /// silently splitting the message stream between the two readers.
  async2::ReceiveFuture<ConstBuf> ClaimRead();

  /// Releases the claim taken by `ClaimRead()`.
  void ReleaseRead();

  /// Begins an egress reservation for `Writer` / `WriteFuture`.
  ///
  /// Fails immediately, without consulting the transport, when the call cannot
  /// write at all:
  ///
  /// - The call's completion status, if the call completed with an error: for
  ///   example `CANCELLED` if the peer cancelled it or the connection closed.
  /// - `FAILED_PRECONDITION`: the write side is already closed, because a
  ///   terminal packet was sent or the call completed successfully.
  /// - `UNAVAILABLE`: the call has been detached from its connection without
  ///   completing.
  /// - `RESOURCE_EXHAUSTED`: a packet with a `header_size`-byte header and a
  ///   `max_payload_size`-byte payload exceeds the largest datagram the
  ///   transport can write.
  ///
  /// Otherwise returns a future for the transport's reservation of
  /// `header_size + max_payload_size` bytes.
  Result<transport::ReserveWriteFuture> ReserveWrite(size_t header_size,
                                                     size_t max_payload_size);

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
  friend class Recyclable<Call>;

  /// Constructs a call attached to `connection_task`. If `allocator` is
  /// non-null, the call will destroy and deallocate itself through `allocator`
  /// when its last `IntrusivePtr` reference is released.
  Call(ConnectionTask& connection_task,
       uint32_t call_id,
       Allocator* allocator = nullptr);

  void ClearConnectionTask() { connection_task_ = nullptr; }

  void pw_recycle() {
    if (allocator_ != nullptr) {
      Allocator* alloc = allocator_;
      this->~Call();
      alloc->Deallocate(this);
    }
  }

 private:
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

    // Dropping the writer cancels the call (UnaryWriter) instead of sending
    // a stream-end packet (Writer).
    kCancelOnWriterDrop = 1 << 4,

    // This call belongs to a ServerConnectionTask (EndpointRole::kServer).
    kServerCall = 1 << 5,

    // The packet that starts this client call has been committed, so the
    // server knows about the call. Always set for a server call.
    kStarted = 1 << 6,
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

    // The call has completed; `completion_status_` holds its status.
    kCompleted = 1 << 4,

    // `NotifyIfClosed()` has run, so it does not run again.
    kClosedNotified = 1 << 5,
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

  void InitChannel();
  void QueueWriterDropPacket();
  void FlushDeferredWriterDrop();

  // Detaches a client call or wakes the server connection the first time the
  // call is observed to be closed.
  void NotifyIfClosed();

  // Non-null while registered in `connection_task_->calls_`; cleared by
  // DetachFromConnection().
  ConnectionTask* connection_task_ = nullptr;
  Allocator* allocator_ = nullptr;

  uint32_t call_id_;

  // Inline SPSC channel storage.
  async2::ChannelStorage<ConstBuf, 1> storage_;

  // Ingress SPSC byte channel & read state.
  async2::Sender<ConstBuf> sender_;
  async2::Receiver<ConstBuf> receiver_;

  // Slot in the ingress queue, acquired by `ReserveMessageSlot()` and consumed
  // by `OnMessage()`. Empty whenever no packet is being delivered; it holds a
  // pending reservation future only while the reader is behind, so that the
  // connection task's wakeup survives until the queue drains.
  async2::FutureOrValue<async2::ReserveSendFuture<ConstBuf>> send_state_;

  uint8_t write_flags_ = 0;
  uint8_t read_flags_ = 0;

  // Set by the first `Complete()`; later completions are ignored.
  Status completion_status_;
};

/// Client-side RPC call state.
///
/// Detaches itself from its `ConnectionTask` as soon as the call closes. A
/// write reservation still in flight at that point is rejected when it
/// commits.
class ClientCall final : public Call {
 public:
  ClientCall(ConnectionTask& connection_task,
             uint32_t call_id,
             Allocator* allocator = nullptr)
      : Call(connection_task, call_id, allocator) {}

  /// Allocates and constructs a heap-allocated `ClientCall` managed by
  /// `IntrusivePtr<Call>`.
  static IntrusivePtr<Call> Create(ConnectionTask& connection_task,
                                   uint32_t call_id,
                                   Allocator& allocator) {
    return IntrusivePtr<Call>(
        allocator.New<ClientCall>(connection_task, call_id, &allocator));
  }
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
};

}  // namespace pw::rpc2::internal
