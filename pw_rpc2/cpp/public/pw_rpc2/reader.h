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

#include "pw_assert/assert.h"
#include "pw_async2/await.h"
#include "pw_async2/context.h"
#include "pw_async2/poll.h"
#include "pw_buf/buf.h"
#include "pw_bytes/span.h"
#include "pw_result/result.h"
#include "pw_rpc2/internal/call.h"
#include "pw_rpc2/internal/call_handle.h"
#include "pw_rpc2/internal/future_base.h"
#include "pw_rpc2/internal/serialize.h"
#include "pw_status/status.h"

namespace pw::rpc2 {

template <typename>
class Reader;

namespace internal {

struct CallAccess;

/// Type-erased deserializer: writes `bytes` decoded as a payload, or their
/// error, to the `async2::Poll<Result<Payload>>` at `result_out`.
using DeserializeFn = void (*)(void* result_out, Result<ConstByteSpan> bytes);

template <typename Payload>
void DeserializeTypeErased(void* result_out, Result<ConstByteSpan> bytes) {
  *static_cast<async2::Poll<Result<Payload>>*>(result_out) =
      bytes.ok() ? Deserialize<Payload>(*bytes)
                 : Result<Payload>(bytes.status());
}

/// Non-templated base class for `ReadFuture<Payload>` and
/// `ResponseFuture<Response>`.
///
/// Keeping `ReadFutureBase` non-templated ensures that the read-claim lifecycle
/// (construction, move-assignment, destruction) and the channel polling logic
/// in `PendRaw()` / `PendAndDeserialize()` are compiled once and shared across
/// all payload types.
class ReadFutureBase : public CallHandle, public FutureBase {
 public:
  ReadFutureBase(const ReadFutureBase&) = delete;
  ReadFutureBase& operator=(const ReadFutureBase&) = delete;

 protected:
  constexpr ReadFutureBase() = default;

  /// Claims the call's read. The read side stays open after this future
  /// resolves; closing it is up to whatever owns the read side: a `Reader`, or
  /// a `ResponseFuture`, which owns it itself.
  explicit ReadFutureBase(const IntrusivePtr<Call>& call) : CallHandle(call) {
    ClaimReadIfHasCall();
  }

  explicit ReadFutureBase(IntrusivePtr<Call>&& call)
      : CallHandle(std::move(call)) {
    ClaimReadIfHasCall();
  }

  ~ReadFutureBase() { ReleaseRead(); }

  ReadFutureBase(ReadFutureBase&& other) noexcept = default;

  ReadFutureBase& operator=(ReadFutureBase&& other) noexcept {
    if (this != &other) {
      // Release this future's own hold before adopting `other`'s call.
      ReleaseRead();
      // `receive_fut_` refers to the call's channel, so replace it before the
      // call reference, which may be the last one.
      receive_fut_ = std::move(other.receive_fut_);
      CallHandle::operator=(static_cast<CallHandle&&>(other));
      FutureBase::operator=(static_cast<FutureBase&&>(other));
    }
    return *this;
  }

  [[nodiscard]] async2::Poll<Result<ConstBuf>> PendRaw(async2::Context& cx);

  void PendAndDeserialize(async2::Context& cx,
                          void* result_out,
                          DeserializeFn deserialize);

 private:
  void ClaimReadIfHasCall() {
    if (!has_call()) {
      return;
    }
    receive_fut_ = call().ClaimRead();
    mark_pending();
  }

  // Releases this future's hold on the call's read stream, if it is active.
  void ReleaseRead() {
    if (is_pendable()) {
      call().ReleaseRead();
    }
  }

  async2::ReceiveFuture<ConstBuf> receive_fut_;
};

/// Non-templated base class for `ResponseFuture<Response>`.
class ResponseFutureBase : public ReadFutureBase {
 public:
  /// Cancels the call: sends a cancellation to the server and completes the
  /// call locally with `Status::Cancelled()`. A pending or later `Pend()`
  /// resolves to `CANCELLED` unless the response has already arrived. No-op if
  /// the call has already ended or this future has no call.
  void Cancel() {
    if (has_call()) {
      call().Cancel();
    }
  }

 protected:
  constexpr ResponseFutureBase() = default;

  // The read is claimed first, so this only flags the call; the read side
  // closes when the claim is released.
  explicit ResponseFutureBase(IntrusivePtr<Call>&& call)
      : ReadFutureBase(std::move(call)) {
    if (has_call()) {
      this->call().CloseReadOnReaderDestroy();
    }
  }

  ResponseFutureBase(ResponseFutureBase&&) noexcept = default;
  ResponseFutureBase& operator=(ResponseFutureBase&&) noexcept = default;
  ~ResponseFutureBase() = default;
};

}  // namespace internal

/// Future that resolves to the next inbound message of a stream
/// (`Result<Payload>`). Returned by `Reader::Read()`.
///
/// At most one `ReadFuture` may be active on a call at a time: starting a
/// second read while an earlier `ReadFuture` is still pending fails with an
/// assertion rather than silently splitting the incoming message stream. As
/// soon as `Pend()` resolves, the read claim is released so the next read can
/// be started even if the completed `ReadFuture` remains in scope.
///
/// A `ReadFuture` holds a shared reference to the underlying call, so it stays
/// valid even if the `Reader` that produced it is destroyed first.
template <typename Payload = ConstBuf>
class ReadFuture : public internal::ReadFutureBase {
 public:
  using value_type = Result<Payload>;

  constexpr ReadFuture() = default;

  ReadFuture(const ReadFuture&) = delete;
  ReadFuture& operator=(const ReadFuture&) = delete;
  ReadFuture(ReadFuture&&) noexcept = default;
  ReadFuture& operator=(ReadFuture&&) noexcept = default;
  ~ReadFuture() = default;

  /// Polls for the inbound message.
  ///
  /// @returns
  /// * `OK` with the decoded `Payload` (or raw `ConstBuf`) when a message
  ///   arrives.
  /// * `OUT_OF_RANGE` when the peer has finished its outbound stream
  ///   (`Finish()` or `~Writer()`) and all sent messages have been read. This
  ///   is the normal end-of-stream condition for a `Reader` loop, not an error.
  /// * The error status that ended the call (such as `CANCELLED` if the peer
  ///   cancelled the RPC or the connection closed).
  /// * A deserialization error (such as `DATA_LOSS`) if the incoming bytes
  ///   could not be decoded as `Payload`.
  [[nodiscard]] async2::Poll<Result<Payload>> Pend(async2::Context& cx) {
    if constexpr (std::is_same_v<Payload, ConstBuf>) {
      return PendRaw(cx);
    } else {
      async2::Poll<Result<Payload>> result = async2::Pending();
      PendAndDeserialize(
          cx, &result, &internal::DeserializeTypeErased<Payload>);
      return result;
    }
  }

 private:
  template <typename>
  friend class ReadFuture;
  template <typename>
  friend class Reader;
  friend struct internal::CallAccess;

  using internal::ReadFutureBase::ReadFutureBase;
};

static_assert(async2::Future<ReadFuture<ConstBuf>>);

using RawReadFuture = ReadFuture<ConstBuf>;

/// Future that resolves to the server's single response to a client unary or
/// client-streaming RPC (`Result<Response>`).
///
/// Returned by `RawUnaryReservation::Commit()` and
/// `ClientStreamCall::response()`. `UnaryFuture` uses one internally.
///
/// A `ResponseFuture` is its call's only reader. Destroying it before it
/// resolves cancels the call (`Status::Cancelled()`) so the server can stop
/// work. `Cancel()` does the same while keeping the future, which then
/// resolves to `CANCELLED`.
template <typename Response = ConstBuf>
class ResponseFuture : public internal::ResponseFutureBase {
 public:
  using value_type = Result<Response>;

  constexpr ResponseFuture() = default;

  ResponseFuture(const ResponseFuture&) = delete;
  ResponseFuture& operator=(const ResponseFuture&) = delete;
  ResponseFuture(ResponseFuture&&) noexcept = default;
  ResponseFuture& operator=(ResponseFuture&&) noexcept = default;
  ~ResponseFuture() = default;

  /// Polls for the server's response.
  ///
  /// @returns
  /// * `OK` with the decoded `Response` (or raw `ConstBuf`) once the server
  ///   replies.
  /// * The error status that ended the call (such as `CANCELLED` if the call
  ///   was cancelled or the connection closed, or `NOT_FOUND` for an unknown
  ///   service or method).
  /// * A deserialization error (such as `DATA_LOSS`) if the response could not
  ///   be decoded as `Response`.
  [[nodiscard]] async2::Poll<Result<Response>> Pend(async2::Context& cx) {
    if constexpr (std::is_same_v<Response, ConstBuf>) {
      return PendRaw(cx);
    } else {
      async2::Poll<Result<Response>> result = async2::Pending();
      PendAndDeserialize(
          cx, &result, &internal::DeserializeTypeErased<Response>);
      return result;
    }
  }

 private:
  friend struct internal::CallAccess;

  explicit ResponseFuture(IntrusivePtr<internal::Call>&& call)
      : internal::ResponseFutureBase(std::move(call)) {}
};

static_assert(async2::Future<ResponseFuture<ConstBuf>>);

using RawResponseFuture = ResponseFuture<ConstBuf>;

namespace internal {

/// Non-templated base class for `Reader<Payload>` that closes the call's read
/// side when destroyed so that any unread or future inbound messages are
/// immediately discarded without blocking the connection task.
class ReaderBase : public CallHandle {
 public:
  ReaderBase(const ReaderBase&) = delete;
  ReaderBase& operator=(const ReaderBase&) = delete;

  /// Aborts the call immediately in both directions.
  ///
  /// Sends a cancellation to the peer and completes the call locally with
  /// `Status::Cancelled()`: once any message already received has been read,
  /// a pending or later `Read()` on this end fails with `CANCELLED`, as does
  /// any write through the call's `Writer` or `UnaryWriter`. No-op if the call
  /// has already ended. On a server, this is equivalent to cancelling through
  /// the call's writer.
  void Cancel() {
    if (has_call()) {
      call().Cancel();
    }
  }

 protected:
  constexpr ReaderBase() = default;
  using CallHandle::CallHandle;

  ~ReaderBase() { CloseReadIfHasCall(); }

  ReaderBase(ReaderBase&&) noexcept = default;

  ReaderBase& operator=(ReaderBase&& other) noexcept {
    if (this != &other) {
      CloseReadIfHasCall();
      CallHandle::operator=(static_cast<CallHandle&&>(other));
    }
    return *this;
  }

 private:
  void CloseReadIfHasCall() {
    if (has_call()) {
      call().CloseReadOnReaderDestroy();
    }
  }
};

}  // namespace internal

/// Inbound message stream handle for reading a sequence of messages from the
/// peer.
///
/// * **Server side** (client-streaming and bidirectional-streaming RPCs):
///   `Reader<Request>` reads the stream of requests sent by the client.
/// * **Client side** (server-streaming and bidirectional-streaming RPCs):
///   `Reader<Response>` reads the stream of responses sent by the server.
///
/// Call `Read()` in a loop until the returned `ReadFuture` resolves to
/// `Status::OutOfRange()`, which indicates that the peer called `Finish()` (or
/// dropped its `Writer`) and no more messages will arrive.
///
/// Destroying a `Reader` before the stream ends stops reading incoming
/// messages (any unread or subsequent messages from the peer are discarded).
///
/// * **Server side:** it does not cancel the call or affect the outbound
///   `Writer` or `UnaryWriter`.
/// * **Client side:** if the RPC has not completed yet, it cancels the call,
///   as `Cancel()` does, since nothing is left to observe its outcome. This
///   keeps the server from running an abandoned RPC, such as an unbounded
///   server stream, until the connection closes.
template <typename Payload = ConstBuf>
class Reader : public internal::ReaderBase {
 public:
  constexpr Reader() = default;

  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;
  Reader(Reader&&) noexcept = default;
  Reader& operator=(Reader&&) noexcept = default;
  ~Reader() = default;

  /// Returns a future that resolves to the next inbound message, or
  /// `Status::OutOfRange()` once the peer has finished the stream.
  ///
  /// Only one `ReadFuture` may be pending at a time; await or destroy the
  /// returned future before calling `Read()` again.
  [[nodiscard]] ReadFuture<Payload> Read() {
    return ReadFuture<Payload>(share_call());
  }

 private:
  template <typename>
  friend class Reader;
  friend struct internal::CallAccess;

  using internal::ReaderBase::ReaderBase;
};

using RawReader = Reader<ConstBuf>;

static_assert(sizeof(RawReader) == sizeof(internal::CallHandle));

}  // namespace pw::rpc2
