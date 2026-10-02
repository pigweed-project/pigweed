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

class ServiceClient;
template <typename, typename>
class UnaryCallFuture;
template <typename, typename>
class ClientStreamCall;
template <typename>
class Reader;

namespace internal {

struct CallAccess;

/// Non-templated base class for `ReadFuture<Payload>`.
///
/// Keeping `ReadFutureBase` non-templated ensures that the read-claim lifecycle
/// (construction, move-assignment, destruction) and the channel polling logic
/// in `PendRaw()` / `PendAndDeserialize()` are compiled once and shared across
/// `ReadFuture<Payload>` for all payload types.
class ReadFutureBase : public CallHandle, public FutureBase {
 public:
  ReadFutureBase(const ReadFutureBase&) = delete;
  ReadFutureBase& operator=(const ReadFutureBase&) = delete;

 protected:
  /// Whether the call will read anything after this future.
  ///
  /// `Reader::Read()` reads one message of many, and the `Reader` closes the
  /// call's read side when it is destroyed. The single response to a unary or
  /// client-streaming RPC has no `Reader` behind it, so that future must close
  /// the read side itself once it completes or is dropped.
  enum ReadKind : bool {
    kStreamRead = false,
    kSoleRead = true,
  };

  using DeserializeFn = void (*)(void* result_out, Result<ConstByteSpan> raw);

  constexpr ReadFutureBase() = default;

  ReadFutureBase(const IntrusivePtr<Call>& call, ReadKind kind)
      : CallHandle(call) {
    ClaimReadIfHasCall(kind);
  }

  ReadFutureBase(IntrusivePtr<Call>&& call, ReadKind kind)
      : CallHandle(std::move(call)) {
    ClaimReadIfHasCall(kind);
  }

  ~ReadFutureBase() { ReleaseRead(); }

  ReadFutureBase(ReadFutureBase&& other) noexcept = default;

  ReadFutureBase& operator=(ReadFutureBase&& other) noexcept {
    if (this != &other) {
      // Release this future's own hold before adopting `other`'s call.
      ReleaseRead();
      CallHandle::operator=(static_cast<CallHandle&&>(other));
      FutureBase::operator=(static_cast<FutureBase&&>(other));
      receive_fut_ = std::move(other.receive_fut_);
    }
    return *this;
  }

  [[nodiscard]] async2::Poll<Result<ConstBuf>> PendRaw(async2::Context& cx);

  void PendAndDeserialize(async2::Context& cx,
                          void* result_out,
                          DeserializeFn deserialize);

 private:
  void ClaimReadIfHasCall(ReadKind kind) {
    if (!has_call()) {
      return;
    }
    receive_fut_ = call().ClaimRead();
    mark_pending();
    if (kind == kSoleRead) {
      // The read is already claimed, so this only flags the call; the read
      // side closes when the claim is released.
      call().CloseReadOnReaderDestroy();
    }
  }

  // Releases this future's hold on the call's read stream, if it is active.
  void ReleaseRead() {
    if (is_pendable()) {
      call().ReleaseRead();
    }
  }

  async2::ReceiveFuture<ConstBuf> receive_fut_;
};

}  // namespace internal

/// Future that resolves to a single inbound RPC message (`Result<Payload>`).
///
/// Returned by `Reader::Read()` to receive the next message in a stream, and
/// used in `ClientStreamCall` / `RawUnaryCall` to await the server's single
/// response.
///
/// At most one `ReadFuture` may be active on a call at a time: starting a
/// second read while an earlier `ReadFuture` is still pending fails with an
/// assertion rather than silently splitting the incoming message stream. As
/// soon as `Pend()` resolves, the read claim is released so the next read can
/// be started even if the completed `ReadFuture` remains in scope.
///
/// A `ReadFuture` holds a shared reference to the underlying call, so it stays
/// valid even if the `Reader` or call struct that produced it is destroyed
/// first.
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
          cx, &result, [](void* out, Result<ConstByteSpan> bytes) {
            *static_cast<async2::Poll<Result<Payload>>*>(out) =
                bytes.ok() ? internal::Deserialize<Payload>(*bytes)
                           : Result<Payload>(bytes.status());
          });
      return result;
    }
  }

 private:
  template <typename>
  friend class ReadFuture;
  template <typename>
  friend class Reader;
  friend class ServiceClient;
  template <typename, typename>
  friend class UnaryCallFuture;
  template <typename, typename>
  friend class ClientStreamCall;
  friend struct internal::CallAccess;

  static ReadFuture StreamRead(const IntrusivePtr<internal::Call>& call) {
    return ReadFuture(call, kStreamRead);
  }
  static ReadFuture StreamRead(IntrusivePtr<internal::Call>&& call) {
    return ReadFuture(std::move(call), kStreamRead);
  }
  static ReadFuture SoleRead(IntrusivePtr<internal::Call>&& call) {
    return ReadFuture(std::move(call), kSoleRead);
  }

  using internal::ReadFutureBase::ReadFutureBase;
};

static_assert(pw::async2::Future<ReadFuture<pw::ConstBuf>>);

using RawReadFuture = ReadFuture<pw::ConstBuf>;

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
    return ReadFuture<Payload>::StreamRead(share_call());
  }

 private:
  template <typename>
  friend class Reader;
  friend class ServiceClient;
  friend struct internal::CallAccess;

  using internal::ReaderBase::ReaderBase;
};

using RawReader = Reader<ConstBuf>;

}  // namespace pw::rpc2
