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
#include <type_traits>
#include <utility>
#include <variant>

#include "pw_assert/assert.h"
#include "pw_async2/await.h"
#include "pw_async2/context.h"
#include "pw_async2/poll.h"
#include "pw_buf/buf.h"
#include "pw_result/result.h"
#include "pw_rpc2/internal/call.h"
#include "pw_rpc2/internal/call_handle.h"
#include "pw_rpc2/internal/future_base.h"
#include "pw_rpc2/internal/packet.h"
#include "pw_rpc2/internal/serialize.h"
#include "pw_rpc2/write_reservation.h"
#include "pw_status/status.h"
#include "pw_transport/socket.h"

namespace pw::rpc2 {

template <typename>
class UnaryWriter;
class RawUnaryWriter;
template <typename>
class Writer;
class RawWriter;

namespace internal {

struct CallAccess;
class UnaryFutureBase;

/// Type-erased serializer: encodes the payload at `payload` into `dest`.
using SerializeFn = StatusWithSize (*)(const void* payload, ByteSpan dest);

template <typename Payload>
StatusWithSize SerializeTypeErased(const void* payload, ByteSpan dest) {
  return Serialize(*static_cast<const Payload*>(payload), dest);
}

/// Non-templated base class for `WriteFuture<Payload>`.
///
/// Keeping `WriteFutureBase` non-templated ensures that awaiting the write
/// reservation, handling reservation or serialization errors, and committing
/// the packet are compiled once and shared across `WriteFuture<Payload>` for
/// all payload types.
class WriteFutureBase {
 public:
  using value_type = Status;

  WriteFutureBase(const WriteFutureBase&) = delete;
  WriteFutureBase& operator=(const WriteFutureBase&) = delete;

  [[nodiscard]] bool is_pendable() const { return res_fut_.is_pendable(); }
  [[nodiscard]] bool is_complete() const { return res_fut_.is_complete(); }

 protected:
  constexpr WriteFutureBase() = default;

  explicit WriteFutureBase(ReserveWriteFuture&& res_fut)
      : res_fut_(std::move(res_fut)) {}

  WriteFutureBase(WriteFutureBase&&) noexcept = default;
  WriteFutureBase& operator=(WriteFutureBase&&) noexcept = default;
  ~WriteFutureBase() = default;

  [[nodiscard]] async2::Poll<Result<IntrusivePtr<Call>>> PendWriteAndTakeCall(
      async2::Context& cx, const void* payload, SerializeFn serialize);

  [[nodiscard]] async2::Poll<Status> PendWrite(async2::Context& cx,
                                               const void* payload,
                                               SerializeFn serialize);

 private:
  friend class UnaryFutureBase;

  ReserveWriteFuture res_fut_;
};

}  // namespace internal

/// Future representing an outbound RPC transmission (`Writer::Write()`,
/// `RawWriter::WriteCopy()`, `Writer::Finish()`, `UnaryWriter::Finish()`, or
/// `RawUnaryWriter::FinishCopy()`).
///
/// When polled, `WriteFuture` waits for a write reservation, serializes
/// `Payload` into the reserved buffer (unless `Payload` is `void`, as for
/// `Writer::Finish()` returning `WriteFuture<>`), and commits the write.
template <typename Payload = void>
class WriteFuture : public internal::WriteFutureBase {
 public:
  constexpr WriteFuture() = default;

  WriteFuture(const WriteFuture&) = delete;
  WriteFuture& operator=(const WriteFuture&) = delete;
  WriteFuture(WriteFuture&& other) noexcept = default;
  WriteFuture& operator=(WriteFuture&& other) noexcept = default;

  /// Polls the outbound write until it is committed.
  ///
  /// @returns
  /// * `OK` once the message or completion has been serialized and committed
  ///   for transmission.
  /// * `FAILED_PRECONDITION` if the writer was already closed (for example,
  ///   after `Finish()` or `Cancel()`).
  /// * `UNAVAILABLE` if the peer cancelled or finished the call, or the
  ///   connection closed before the write could be sent.
  /// * `RESOURCE_EXHAUSTED` if the serialized payload does not fit in the
  ///   write buffer.
  /// * Any serialization error returned by the message serializer.
  [[nodiscard]] async2::Poll<Status> Pend(async2::Context& cx) {
    if constexpr (std::is_void_v<Payload>) {
      return PendWrite(cx, nullptr, nullptr);
    } else {
      return PendWrite(cx, &payload_, &internal::SerializeTypeErased<Payload>);
    }
  }

 private:
  template <typename>
  friend class Writer;
  friend class RawWriter;
  template <typename>
  friend class UnaryWriter;
  friend class RawUnaryWriter;
  friend struct internal::CallAccess;

  // Constructor for typed message or response writes.
  template <typename P = Payload,
            typename = std::enable_if_t<!std::is_void_v<P>>>
  WriteFuture(ReserveWriteFuture&& res_fut, P&& payload)
      : internal::WriteFutureBase(std::move(res_fut)),
        payload_(std::forward<P>(payload)) {}

  // Constructor for zero-payload completion packets (`Writer::Finish()`).
  template <typename P = Payload,
            typename = std::enable_if_t<std::is_void_v<P>>>
  explicit WriteFuture(ReserveWriteFuture&& res_fut)
      : internal::WriteFutureBase(std::move(res_fut)) {}

  [[no_unique_address]] std::conditional_t<std::is_void_v<Payload>,
                                           std::monostate,
                                           Payload> payload_{};
};

static_assert(async2::Future<WriteFuture<>>);
static_assert(async2::Future<WriteFuture<ConstByteSpan>>);

namespace internal {

/// Non-templated base class for outbound RPC handles (`Writer`, `RawWriter`,
/// `UnaryWriter`, and `RawUnaryWriter`).
///
/// Keeping `WriterBase` non-templated ensures that `is_closed()`, `Cancel()`,
/// move-assignment, destruction, and packet reservation helpers are compiled
/// once and shared across all writer types.
class WriterBase : public CallHandle {
 public:
  WriterBase(const WriterBase&) = delete;
  WriterBase& operator=(const WriterBase&) = delete;

  /// Returns `true` if no further writes can be initiated through this writer.
  ///
  /// This becomes `true` once `Finish()` (or `FinishCopy()` /
  /// `ReserveFinish()`) is called, `Cancel()` is called, the peer cancels or
  /// finishes the call, or the connection closes. Server handlers can check
  /// `is_closed()` between async steps to stop work early if the client has
  /// disconnected or cancelled.
  [[nodiscard]] bool is_closed() const {
    return !has_call() || call().is_write_closed() ||
           call().has_pending_terminal_write();
  }

  /// Aborts the call immediately, closing this writer and sending
  /// `Status::Cancelled()` to the peer.
  ///
  /// Unlike `Finish()`, which indicates normal completion of the stream or RPC,
  /// `Cancel()` terminates the RPC in both directions. The peer's pending or
  /// next read fails with `CANCELLED`, and so does this end's: the call
  /// completes locally with `Status::Cancelled()`, so a client's `Reader` or
  /// response future resolves instead of waiting for a server that will not
  /// answer. A write still in flight is rejected rather than sent.
  void Cancel() {
    if (has_call()) {
      call().Cancel();
    }
  }

 protected:
  /// What destroying a writer before it is finished does to the call.
  ///
  /// `Writer` ends its stream normally, since a stream may legitimately be
  /// empty. `UnaryWriter` cancels, since the peer is owed exactly one
  /// response.
  enum OnDestroy : bool {
    kFinishOnDestroy = false,
    kCancelOnDestroy = true,
  };

  constexpr WriterBase() = default;

  WriterBase(const IntrusivePtr<Call>& call, OnDestroy on_destroy)
      : CallHandle(call) {
    if (has_call()) {
      this->call().set_cancel_on_writer_drop(on_destroy == kCancelOnDestroy);
    }
  }

  WriterBase(IntrusivePtr<Call>&& call, OnDestroy on_destroy)
      : CallHandle(std::move(call)) {
    if (has_call()) {
      this->call().set_cancel_on_writer_drop(on_destroy == kCancelOnDestroy);
    }
  }

  WriterBase(WriterBase&&) noexcept = default;

  WriterBase& operator=(WriterBase&& other) noexcept {
    if (this != &other) {
      CleanupIfUnfinished();
      CallHandle::operator=(std::move(other));
    }
    return *this;
  }

  ~WriterBase() { CleanupIfUnfinished(); }

  /// The largest payload a message or response packet on this call can carry,
  /// or 0 if the call cannot write.
  [[nodiscard]] size_t payload_limit() const {
    return has_call() ? call().max_payload_size(sizeof(PacketHeader)) : 0;
  }

  [[nodiscard]] ReserveWriteFuture ReserveMessage(
      size_t max_payload_size) const {
    auto reservation = ReserveOutbound(sizeof(PacketHeader), max_payload_size);
    return ReserveWriteFuture::Message(std::move(reservation.future),
                                       reservation.role,
                                       reservation.call_id,
                                       std::move(reservation.call));
  }

  [[nodiscard]] ReserveWriteFuture ReserveStreamEnd() const {
    auto reservation = ReserveOutbound(sizeof(PacketHeader), 0);
    return ReserveWriteFuture::Finish(std::move(reservation.future),
                                      reservation.role,
                                      reservation.call_id,
                                      std::move(reservation.call));
  }

  [[nodiscard]] ReserveWriteFuture ReserveResponse(
      size_t max_payload_size) const {
    auto reservation = ReserveOutbound(sizeof(PacketHeader), max_payload_size);
    return ReserveWriteFuture::Response(std::move(reservation.future),
                                        reservation.call_id,
                                        std::move(reservation.call));
  }

 private:
  void CleanupIfUnfinished() {
    if (has_call()) {
      call().CloseOnWriterDestroy();
    }
  }
};

}  // namespace internal

/// Outbound message stream handle for sending zero or more typed messages to
/// the peer.
///
/// * **Server side** (server-streaming and bidirectional-streaming RPCs):
///   `Writer<Response>` streams response messages to the client. Calling
///   `Finish()` (or destroying the `Writer`) completes the RPC with `OK`.
/// * **Client side** (client-streaming and bidirectional-streaming RPCs):
///   `Writer<Request>` streams request messages to the server. Calling
///   `Finish()` (or destroying the `Writer`) ends the request stream while
///   keeping the call open to receive the server's response(s). To abort an
///   in-progress call instead of finishing the request stream, call `Cancel()`
///   before dropping the `Writer`.
///
/// Messages are sent in the order their reservations are committed via
/// `WriteReservation::Commit()` (or when `WriteFuture` resolves), not the
/// order `Write()`, `ReserveWrite()`, or `Finish()` was called. Await each
/// write (or commit its reservation) before starting the next write or
/// `Finish()`.
///
/// On a server, if the method's future or coroutine completes before its
/// `Writer` has finished the stream (for example, if the `Writer` was moved to
/// a subtask that was not joined before returning), the call is terminated with
/// `CANCELLED` and any remaining writes fail.
template <typename Payload>
class Writer : public internal::WriterBase {
 public:
  static_assert(!std::is_same_v<Payload, ConstBuf>,
                "Use RawWriter instead of Writer<ConstBuf>.");

  constexpr Writer() = default;

  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;
  Writer(Writer&&) noexcept = default;
  Writer& operator=(Writer&&) noexcept = default;
  ~Writer() = default;

  /// Serializes and sends a single message on the outbound stream.
  [[nodiscard]] WriteFuture<Payload> Write(const Payload& payload) {
    return WriteFuture<Payload>(
        ReserveMessage(internal::ReservationSize(payload, payload_limit())),
        payload);
  }

  /// Serializes and sends a single message on the outbound stream.
  [[nodiscard]] WriteFuture<Payload> Write(Payload&& payload) {
    return WriteFuture<Payload>(
        ReserveMessage(internal::ReservationSize(payload, payload_limit())),
        std::move(payload));
  }

  /// Reserves a buffer of at least `max_payload_size` bytes for writing a
  /// message in place.
  [[nodiscard]] ReserveWriteFuture ReserveWrite(size_t max_payload_size) {
    return ReserveMessage(max_payload_size);
  }

  /// Completes the outbound message stream normally.
  ///
  /// Sends a stream-end packet so the peer's `Reader::Read()` resolves to
  /// `Status::OutOfRange()` after all prior messages have been read. On a
  /// server, this also completes the RPC with `OK`; on a client, it half-closes
  /// the request stream while keeping the inbound side open.
  ///
  /// If a `Writer` is destroyed without calling `Finish()` or `Cancel()`, the
  /// stream is finished automatically on a best-effort basis. Explicitly
  /// awaiting `Finish()` waits for buffer space and reports whether the
  /// completion was committed (`Status`).
  ///
  /// @warning On a server, committing the `Finish()` packet retires the call
  /// and destroys the method's future or coroutine on the connection's next
  /// poll. Perform all per-call work before calling `Finish()`.
  [[nodiscard]] WriteFuture<> Finish() {
    return WriteFuture<>(ReserveStreamEnd());
  }

 private:
  friend struct internal::CallAccess;

  explicit Writer(const IntrusivePtr<internal::Call>& call)
      : internal::WriterBase(call, kFinishOnDestroy) {}
  explicit Writer(IntrusivePtr<internal::Call>&& call)
      : internal::WriterBase(std::move(call), kFinishOnDestroy) {}
};

/// Outbound message stream handle for sending zero or more raw byte messages to
/// the peer.
///
/// Provides copy-based message writing (`WriteCopy()`) and zero-copy write
/// reservation (`ReserveWrite()`). Lifecycle and completion semantics match
/// `Writer<Payload>`.
class RawWriter : public internal::WriterBase {
 public:
  constexpr RawWriter() = default;

  RawWriter(const RawWriter&) = delete;
  RawWriter& operator=(const RawWriter&) = delete;
  RawWriter(RawWriter&&) noexcept = default;
  RawWriter& operator=(RawWriter&&) noexcept = default;
  ~RawWriter() = default;

  /// Reserves a write buffer of `payload.size()` bytes and copies `payload`
  /// into it when the reservation resolves.
  ///
  /// The bytes referenced by `payload` must remain valid until the returned
  /// `WriteFuture` completes or is destroyed.
  [[nodiscard]] WriteFuture<ConstByteSpan> WriteCopy(ConstByteSpan payload) {
    return WriteFuture<ConstByteSpan>(ReserveMessage(payload.size()), payload);
  }

  /// Reserves a buffer of at least `max_payload_size` bytes for writing a
  /// message in place.
  [[nodiscard]] ReserveWriteFuture ReserveWrite(size_t max_payload_size) {
    return ReserveMessage(max_payload_size);
  }

  /// Completes the outbound message stream normally. See `Writer::Finish()`.
  [[nodiscard]] WriteFuture<> Finish() {
    return WriteFuture<>(ReserveStreamEnd());
  }

 private:
  friend struct internal::CallAccess;

  explicit RawWriter(const IntrusivePtr<internal::Call>& call)
      : internal::WriterBase(call, kFinishOnDestroy) {}
  explicit RawWriter(IntrusivePtr<internal::Call>&& call)
      : internal::WriterBase(std::move(call), kFinishOnDestroy) {}
};

/// Single-response outbound writer for server unary and client-streaming RPCs.
///
/// Unlike `Writer<Payload>`, which streams zero or more messages and finishes
/// with a zero-argument `Finish()`, `UnaryWriter<Payload>` sends a single
/// terminal response via `Finish(Payload)` (or `ReserveFinish()` for raw
/// buffers), delivering the response payload and completing the RPC in one
/// step.
///
/// Destroying a `UnaryWriter` before a response is committed cancels the call
/// (`Status::Cancelled()`) so the client is not left waiting for a response. If
/// a `Finish()` future or `ReserveFinish()` reservation is still in flight when
/// the `UnaryWriter` is destroyed, cancellation is deferred and sent only if
/// that future or reservation is dropped without committing. Similarly, if the
/// method's future or coroutine finishes before a response is committed, the
/// call is cancelled and any remaining handle is detached.
template <typename Payload>
class UnaryWriter : public internal::WriterBase {
 public:
  static_assert(!std::is_same_v<Payload, ConstBuf>,
                "Use RawUnaryWriter instead of UnaryWriter<ConstBuf>.");

  constexpr UnaryWriter() = default;

  UnaryWriter(const UnaryWriter&) = delete;
  UnaryWriter& operator=(const UnaryWriter&) = delete;
  UnaryWriter(UnaryWriter&&) noexcept = default;
  UnaryWriter& operator=(UnaryWriter&&) noexcept = default;
  ~UnaryWriter() = default;

  /// Serializes `payload`, sends it as the single response, and completes the
  /// RPC with `OK`.
  ///
  /// @warning Committing the response retires the call and destroys the
  /// method's future or coroutine on the connection's next poll. Perform all
  /// per-call work before calling `Finish()`.
  [[nodiscard]] WriteFuture<Payload> Finish(const Payload& payload) {
    return WriteFuture<Payload>(
        ReserveResponse(internal::ReservationSize(payload, payload_limit())),
        payload);
  }

  /// Serializes `payload`, sends it as the single response, and completes the
  /// RPC with `OK`.
  ///
  /// @warning Committing the response retires the call and destroys the
  /// method's future or coroutine on the connection's next poll. Perform all
  /// per-call work before calling `Finish()`.
  [[nodiscard]] WriteFuture<Payload> Finish(Payload&& payload) {
    return WriteFuture<Payload>(
        ReserveResponse(internal::ReservationSize(payload, payload_limit())),
        std::move(payload));
  }

  /// Reserves a buffer of at least `max_payload_size` bytes for writing the
  /// single terminal response in place.
  ///
  /// Committing the resulting `WriteReservation` transmits the response and
  /// completes the RPC with `OK`, which retires the call and destroys the
  /// method's future or coroutine. Cancelling or dropping the reservation
  /// without committing reopens the `UnaryWriter` (or cancels the call if the
  /// `UnaryWriter` was already destroyed).
  [[nodiscard]] ReserveWriteFuture ReserveFinish(size_t max_payload_size) {
    return ReserveResponse(max_payload_size);
  }

 private:
  friend struct internal::CallAccess;

  explicit UnaryWriter(const IntrusivePtr<internal::Call>& call)
      : internal::WriterBase(call, kCancelOnDestroy) {}
  explicit UnaryWriter(IntrusivePtr<internal::Call>&& call)
      : internal::WriterBase(std::move(call), kCancelOnDestroy) {}
};

/// Single-response outbound writer for raw server unary and client-streaming
/// RPCs.
///
/// Provides copy-based response writing (`FinishCopy()`) and zero-copy response
/// reservation (`ReserveFinish()`). Lifecycle and cancellation semantics match
/// `UnaryWriter<Payload>`.
class RawUnaryWriter : public internal::WriterBase {
 public:
  constexpr RawUnaryWriter() = default;

  RawUnaryWriter(const RawUnaryWriter&) = delete;
  RawUnaryWriter& operator=(const RawUnaryWriter&) = delete;
  RawUnaryWriter(RawUnaryWriter&&) noexcept = default;
  RawUnaryWriter& operator=(RawUnaryWriter&&) noexcept = default;
  ~RawUnaryWriter() = default;

  /// Reserves a write buffer of `payload.size()` bytes, copies `payload` into
  /// it when the reservation resolves, and completes the RPC with `OK`.
  ///
  /// The bytes referenced by `payload` must remain valid until the returned
  /// `WriteFuture` completes or is destroyed.
  ///
  /// @warning Committing the response retires the call and destroys the
  /// method's future or coroutine on the connection's next poll. Perform all
  /// per-call work before calling `FinishCopy()`.
  [[nodiscard]] WriteFuture<ConstByteSpan> FinishCopy(ConstByteSpan payload) {
    return WriteFuture<ConstByteSpan>(ReserveResponse(payload.size()), payload);
  }

  /// Reserves a buffer of at least `max_payload_size` bytes for writing the
  /// single terminal response in place. See `UnaryWriter::ReserveFinish()`.
  [[nodiscard]] ReserveWriteFuture ReserveFinish(size_t max_payload_size) {
    return ReserveResponse(max_payload_size);
  }

 private:
  friend struct internal::CallAccess;

  explicit RawUnaryWriter(const IntrusivePtr<internal::Call>& call)
      : internal::WriterBase(call, kCancelOnDestroy) {}
  explicit RawUnaryWriter(IntrusivePtr<internal::Call>&& call)
      : internal::WriterBase(std::move(call), kCancelOnDestroy) {}
};

static_assert(sizeof(RawWriter) == sizeof(internal::CallHandle));
static_assert(sizeof(RawUnaryWriter) == sizeof(internal::CallHandle));

}  // namespace pw::rpc2
