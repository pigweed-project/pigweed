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
template <typename>
class Writer;

/// Single-response writer for raw (`ConstBuf`) messages.
using RawUnaryWriter = UnaryWriter<ConstBuf>;

/// Outbound message stream writer for raw (`ConstBuf`) messages.
using RawWriter = Writer<ConstBuf>;

namespace internal {

struct CallAccess;
class UnaryFutureBase;

/// Type-erased serializer: encodes the message at `message` into `dest`.
using SerializeFn = StatusWithSize (*)(const void* message, ByteSpan dest);

template <typename Message>
StatusWithSize SerializeTypeErased(const void* message, ByteSpan dest) {
  return Serialize(*static_cast<const Message*>(message), dest);
}

/// Non-templated base class for `WriteFuture<Message>`.
///
/// Shares the reservation, serialization, and commit logic across all message
/// types.
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
      async2::Context& cx, const void* message, SerializeFn serialize);

  [[nodiscard]] async2::Poll<Status> PendWrite(async2::Context& cx,
                                               const void* message,
                                               SerializeFn serialize);

 private:
  friend class UnaryFutureBase;

  ReserveWriteFuture res_fut_;
};

}  // namespace internal

/// Future representing an outbound write or stream completion.
///
/// Waits for a write reservation, serializes `Message` into the reserved buffer
/// (unless `Message` is `void`, as for `Writer::Finish()`), and commits the
/// write.
///
/// A `ConstBuf` message is copied into the reserved buffer; if it owns its
/// memory, that memory is released as soon as the write completes.
template <typename Message = void>
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
  /// * `OK` once the message or completion has been committed.
  /// * `FAILED_PRECONDITION` if the writer is already closed.
  /// * `RESOURCE_EXHAUSTED` if the encoded message does not fit in the write
  ///   buffer.
  /// * The call's completion status (such as `CANCELLED`) if the call ended
  ///   with an error.
  /// * `UNAVAILABLE` if the connection is closed.
  /// * Any error returned by the message serializer.
  [[nodiscard]] async2::Poll<Status> Pend(async2::Context& cx) {
    if constexpr (std::is_void_v<Message>) {
      return PendWrite(cx, nullptr, nullptr);
    } else {
      async2::Poll<Status> result =
          PendWrite(cx, &message_, &internal::SerializeTypeErased<Message>);
      if constexpr (std::is_same_v<Message, ConstBuf>) {
        if (result.IsReady()) {
          message_.reset();
        }
      }
      return result;
    }
  }

 private:
  template <typename>
  friend class Writer;
  template <typename>
  friend class UnaryWriter;
  friend struct internal::CallAccess;

  // Constructor for message or response writes.
  template <typename M>
  WriteFuture(ReserveWriteFuture&& res_fut, M&& message)
      : internal::WriteFutureBase(std::move(res_fut)),
        message_(std::forward<M>(message)) {}

  // Constructor for stream-end writes (`Writer::Finish()`).
  explicit WriteFuture(ReserveWriteFuture&& res_fut)
      : internal::WriteFutureBase(std::move(res_fut)) {
    static_assert(std::is_void_v<Message>);
  }

  [[no_unique_address]] std::conditional_t<std::is_void_v<Message>,
                                           std::monostate,
                                           Message> message_{};
};

static_assert(async2::Future<WriteFuture<>>);
static_assert(async2::Future<WriteFuture<ConstBuf>>);

namespace internal {

/// Non-templated base class for outbound RPC handles (`Writer` and
/// `UnaryWriter`).
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
  /// This becomes `true` once `Finish()` (or `ReserveFinish()`) is called,
  /// `Cancel()` is called, the peer cancels or finishes the call, or the
  /// connection closes. Server handlers can check `is_closed()` between async
  /// steps to stop work early if the client has disconnected or cancelled.
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
      size_t max_message_size) const {
    auto reservation = ReserveOutbound(sizeof(PacketHeader), max_message_size);
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
      size_t max_message_size) const {
    auto reservation = ReserveOutbound(sizeof(PacketHeader), max_message_size);
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

/// Outbound stream handle for sending zero or more messages to the peer.
///
/// * **Server side** (server- and bidirectional-streaming RPCs): streams
///   response messages to the client. `Finish()` (or `~Writer()`) completes the
///   RPC with `OK`.
/// * **Client side** (client- and bidirectional-streaming RPCs): streams
///   request messages to the server. `Finish()` (or `~Writer()`) closes the
///   request stream while keeping the call open for responses. Call `Cancel()`
///   to abort the call instead.
///
/// Every `Writer` can copy a `ConstBuf` with `WriteCopy()` or reserve a buffer
/// with `ReserveWrite()`. Typed writers (such as `Writer<pwpb::Msg>`) also
/// serialize messages with `Write()`.
///
/// Messages are sent in the order their writes complete or their reservations
/// are committed, not the order `Write()`, `WriteCopy()`, `ReserveWrite()`, or
/// `Finish()` was called. Await each write (or commit its reservation) before
/// starting the next write or `Finish()`.
///
/// On a server, if the method's future or coroutine completes before its
/// `Writer` has finished the stream (for example, if the `Writer` was moved to
/// a subtask that was not joined before returning), the call is terminated with
/// `CANCELLED` and any remaining writes fail.
template <typename Message>
class Writer : public internal::WriterBase {
 public:
  constexpr Writer() = default;

  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;
  Writer(Writer&&) noexcept = default;
  Writer& operator=(Writer&&) noexcept = default;
  ~Writer() = default;

  /// Serializes and sends `message`. Not available on `RawWriter`; use
  /// `WriteCopy()` instead.
  ///
  /// @note `message` is copied into the returned future. Copying a
  /// `pw_protobuf` message drops its callback fields, so move messages with
  /// callbacks into `Write(Message&&)` instead.
  template <typename M = Message,
            typename = std::enable_if_t<std::is_copy_constructible_v<M> &&
                                        !std::is_same_v<M, ConstBuf>>>
  [[nodiscard]] WriteFuture<Message> Write(const Message& message) {
    return WriteFuture<Message>(
        ReserveWrite(internal::ReservationSize(message, payload_limit())),
        message);
  }

  /// Serializes and sends `message`, moving it into the returned future. Not
  /// available on `RawWriter`; use `WriteCopy()` instead.
  template <typename M = Message,
            typename = std::enable_if_t<!std::is_same_v<M, ConstBuf>>>
  [[nodiscard]] WriteFuture<Message> Write(Message&& message) {
    return WriteFuture<Message>(
        ReserveWrite(internal::ReservationSize(message, payload_limit())),
        std::move(message));
  }

  /// Copies `message` into the outbound stream.
  ///
  /// Pass `ConstBuf::Unowned(bytes)` to copy from bytes that outlive the
  /// returned future, or move in an owned `ConstBuf`, which is released once
  /// copied.
  [[nodiscard]] WriteFuture<ConstBuf> WriteCopy(ConstBuf&& message) {
    return WriteFuture<ConstBuf>(ReserveWrite(message.size()),
                                 std::move(message));
  }

  /// Reserves a buffer of at least `max_message_size` bytes for writing a
  /// message in place.
  [[nodiscard]] ReserveWriteFuture ReserveWrite(size_t max_message_size) {
    return ReserveMessage(max_message_size);
  }

  /// Completes the outbound message stream normally.
  ///
  /// The peer's `Reader::Read()` resolves to `Status::OutOfRange()` after all
  /// prior messages have been read. On a server, this completes the RPC with
  /// `OK`; on a client, it closes the request stream while keeping the inbound
  /// side open.
  ///
  /// Destroying a `Writer` without calling `Finish()` or `Cancel()` finishes
  /// the stream on a best-effort basis. Await `Finish()` to wait for buffer
  /// space and observe whether completion was committed.
  ///
  /// @warning On a server, committing `Finish()` retires the call and destroys
  /// the method's future or coroutine on the connection's next poll. Perform
  /// all per-call work before calling `Finish()`.
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

/// Single-response writer for server unary and client-streaming RPCs.
///
/// Sends a single response and completes the RPC with `OK` via `FinishCopy()`
/// (raw bytes), `Finish()` (typed writers only), or `ReserveFinish()` (in-place
/// write).
///
/// Destroying a `UnaryWriter` (or returning from the method's future or
/// coroutine) before a response is committed cancels the call
/// (`Status::Cancelled()`). If a `Finish()` future or `ReserveFinish()`
/// reservation is still in flight when the `UnaryWriter` is destroyed, the call
/// is cancelled only if that future or reservation is dropped without
/// committing.
template <typename Message>
class UnaryWriter : public internal::WriterBase {
 public:
  constexpr UnaryWriter() = default;

  UnaryWriter(const UnaryWriter&) = delete;
  UnaryWriter& operator=(const UnaryWriter&) = delete;
  UnaryWriter(UnaryWriter&&) noexcept = default;
  UnaryWriter& operator=(UnaryWriter&&) noexcept = default;
  ~UnaryWriter() = default;

  /// Serializes `response`, sends it, and completes the RPC with `OK`. Not
  /// available on `RawUnaryWriter`; use `FinishCopy()` instead.
  ///
  /// @note `response` is copied into the returned future. Copying a
  /// `pw_protobuf` message drops its callback fields, so move responses with
  /// callbacks into `Finish(Message&&)` instead.
  ///
  /// @warning Committing the response retires the call and destroys the
  /// method's future or coroutine on the connection's next poll. Perform all
  /// per-call work before calling `Finish()`.
  template <typename M = Message,
            typename = std::enable_if_t<std::is_copy_constructible_v<M> &&
                                        !std::is_same_v<M, ConstBuf>>>
  [[nodiscard]] WriteFuture<Message> Finish(const Message& response) {
    return WriteFuture<Message>(
        ReserveFinish(internal::ReservationSize(response, payload_limit())),
        response);
  }

  /// Serializes `response`, sends it, and completes the RPC with `OK`, moving
  /// `response` into the returned future. Not available on `RawUnaryWriter`;
  /// use `FinishCopy()` instead.
  ///
  /// @warning Committing the response retires the call and destroys the
  /// method's future or coroutine on the connection's next poll. Perform all
  /// per-call work before calling `Finish()`.
  template <typename M = Message,
            typename = std::enable_if_t<!std::is_same_v<M, ConstBuf>>>
  [[nodiscard]] WriteFuture<Message> Finish(Message&& response) {
    return WriteFuture<Message>(
        ReserveFinish(internal::ReservationSize(response, payload_limit())),
        std::move(response));
  }

  /// Copies `response` as the single response and completes the RPC with `OK`.
  ///
  /// Pass `ConstBuf::Unowned(bytes)` to copy from bytes that outlive the
  /// returned future, or move in an owned `ConstBuf`, which is released once
  /// copied.
  ///
  /// @warning Committing the response retires the call and destroys the
  /// method's future or coroutine on the connection's next poll. Perform all
  /// per-call work before calling `FinishCopy()`.
  [[nodiscard]] WriteFuture<ConstBuf> FinishCopy(ConstBuf&& response) {
    return WriteFuture<ConstBuf>(ReserveFinish(response.size()),
                                 std::move(response));
  }

  /// Reserves a buffer of at least `max_message_size` bytes for writing the
  /// response in place.
  ///
  /// Committing the resulting `WriteReservation` sends the response and
  /// completes the RPC with `OK`, retiring the call and destroying the
  /// method's future or coroutine. Dropping the reservation without committing
  /// reopens the `UnaryWriter` (or cancels the call if the `UnaryWriter` was
  /// already destroyed).
  [[nodiscard]] ReserveWriteFuture ReserveFinish(size_t max_message_size) {
    return ReserveResponse(max_message_size);
  }

 private:
  friend struct internal::CallAccess;

  explicit UnaryWriter(const IntrusivePtr<internal::Call>& call)
      : internal::WriterBase(call, kCancelOnDestroy) {}
  explicit UnaryWriter(IntrusivePtr<internal::Call>&& call)
      : internal::WriterBase(std::move(call), kCancelOnDestroy) {}
};

static_assert(sizeof(RawWriter) == sizeof(internal::CallHandle));
static_assert(sizeof(RawUnaryWriter) == sizeof(internal::CallHandle));

}  // namespace pw::rpc2
