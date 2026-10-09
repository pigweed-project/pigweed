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
#include <type_traits>
#include <utility>
#include <variant>

#include "pw_async2/context.h"
#include "pw_async2/future.h"
#include "pw_async2/poll.h"
#include "pw_async2/try.h"
#include "pw_buf/buf.h"
#include "pw_bytes/span.h"
#include "pw_intrusive_ptr/intrusive_ptr.h"
#include "pw_result/result.h"
#include "pw_rpc2/client.h"
#include "pw_rpc2/internal/call.h"
#include "pw_rpc2/internal/future_base.h"
#include "pw_rpc2/internal/serialize.h"
#include "pw_rpc2/reader.h"
#include "pw_rpc2/write_reservation.h"
#include "pw_rpc2/writer.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"

namespace pw::rpc2 {
namespace internal {

class GeneratedServiceClient;

/// Non-templated base class for `RequestFuture<CallHandler, Request>`.
class RequestFutureBase : public WriteFutureBase {
 protected:
  constexpr RequestFutureBase() = default;

  explicit RequestFutureBase(ReserveWriteFuture&& request_reservation)
      : WriteFutureBase(std::move(request_reservation)) {}

  RequestFutureBase(RequestFutureBase&&) noexcept = default;
  RequestFutureBase& operator=(RequestFutureBase&&) noexcept = default;
  ~RequestFutureBase() = default;

  template <typename CallHandler>
  [[nodiscard]] async2::Poll<Result<CallHandler>> PendAndCreate(
      async2::Context& cx, const void* request, SerializeFn serialize) {
    PW_TRY_READY_ASSIGN(Result<IntrusivePtr<Call>> call,
                        PendWriteAndTakeCall(cx, request, serialize));
    if (!call.ok()) {
      return async2::Ready(Result<CallHandler>(call.status()));
    }
    return async2::Ready(
        Result<CallHandler>(CallAccess::Create<CallHandler>(std::move(*call))));
  }
};

/// Non-templated base class for `UnaryFuture<Request, Response>`.
class UnaryFutureBase : public FutureBase {
 public:
  /// Cancels the call. The future then resolves to `CANCELLED`.
  ///
  /// If the request has not been sent, nothing is sent: the server never
  /// learns of the call. If the request is waiting for transport buffer space,
  /// the future resolves once that wait ends. Otherwise, sends a cancellation
  /// to the server, as `ResponseFuture::Cancel()` does.
  ///
  /// No-op if the call has already ended.
  void Cancel();

 protected:
  constexpr UnaryFutureBase() = default;

  explicit UnaryFutureBase(ReserveWriteFuture&& request_reservation)
      : FutureBase(async2::FutureState::kPending),
        req_fut_(std::move(request_reservation)) {}

  UnaryFutureBase(UnaryFutureBase&&) noexcept = default;
  UnaryFutureBase& operator=(UnaryFutureBase&&) noexcept = default;
  ~UnaryFutureBase() = default;

  /// True until the request has been sent or has failed to send.
  [[nodiscard]] bool request_pending() const { return req_fut_.is_pendable(); }

  [[nodiscard]] async2::Poll<Result<ConstBuf>> PendRaw(async2::Context& cx,
                                                       const void* request,
                                                       SerializeFn serialize);

  void PendAndDeserialize(async2::Context& cx,
                          const void* request,
                          SerializeFn serialize,
                          void* result_out,
                          DeserializeFn deserialize);

 private:
  WriteFutureBase req_fut_;
  RawResponseFuture response_fut_;
};

}  // namespace internal

/// Future that starts a streaming RPC and resolves to its call handle:
/// `Reader` (server streaming), `ClientStreamCall` (client streaming), or
/// `BidiStreamCall` (bidirectional). For server streaming, it also sends the
/// `Request` message.
template <typename CallHandler, typename Request = void>
class RequestFuture : public internal::RequestFutureBase {
 public:
  using value_type = Result<CallHandler>;

  constexpr RequestFuture() = default;

  /// Polls until the RPC is started and returns its call handle.
  ///
  /// @returns
  /// * `OK` with the call handle once the initial write is committed.
  /// * `UNAVAILABLE` if the client is empty or its connection is closed.
  /// * `RESOURCE_EXHAUSTED` if the call could not be allocated or the request
  ///   does not fit in the write buffer.
  /// * Any error returned by the request serializer.
  [[nodiscard]] async2::Poll<value_type> Pend(async2::Context& cx) {
    if constexpr (std::is_void_v<Request>) {
      return PendAndCreate<CallHandler>(cx, nullptr, nullptr);
    } else {
      async2::Poll<value_type> result = PendAndCreate<CallHandler>(
          cx, &request_, &internal::SerializeTypeErased<Request>);
      if constexpr (std::is_same_v<Request, ConstBuf>) {
        // Release an owned request as soon as it has been copied.
        if (result.IsReady()) {
          request_.reset();
        }
      }
      return result;
    }
  }

 private:
  friend class internal::GeneratedServiceClient;

  // Server streaming: sends `request`.
  template <typename R>
  RequestFuture(ReserveWriteFuture&& request_reservation, R&& request)
      : internal::RequestFutureBase(std::move(request_reservation)),
        request_(std::forward<R>(request)) {}

  // Client and bidirectional streaming: starts the call without a request
  // message.
  explicit RequestFuture(ReserveWriteFuture&& request_reservation)
      : internal::RequestFutureBase(std::move(request_reservation)) {
    static_assert(std::is_void_v<Request>);
  }

  [[no_unique_address]] std::conditional_t<std::is_void_v<Request>,
                                           std::monostate,
                                           Request> request_{};
};

/// Future representing a complete unary RPC on a client.
///
/// Sends the `Request` message to the server and awaits the server's single
/// `Response` message, resolving to `Result<Response>`.
///
/// Destroying a `UnaryFuture` before it completes cancels the call. If the
/// request was already sent, the server is notified so it can stop work.
/// `Cancel()` does the same while keeping the future, which then resolves to
/// `CANCELLED`.
template <typename Request = ConstBuf, typename Response = ConstBuf>
class UnaryFuture : public internal::UnaryFutureBase {
 public:
  using value_type = Result<Response>;

  constexpr UnaryFuture() = default;

  /// Polls the request transmission and then the response.
  ///
  /// @returns
  /// * `OK` with the `Response` once the server replies.
  /// * The server's error status (e.g. `NOT_FOUND` for an unknown service or
  ///   method, or `CANCELLED` if the server dropped its `UnaryWriter` without
  ///   responding).
  /// * `FAILED_PRECONDITION` if the server answered as if the method streamed.
  /// * `UNAVAILABLE` if the client is empty or its connection closed.
  /// * `RESOURCE_EXHAUSTED` if the call could not be allocated or the request
  ///   does not fit in the write buffer.
  /// * A serialization or deserialization error (e.g. `DATA_LOSS`).
  [[nodiscard]] async2::Poll<Result<Response>> Pend(async2::Context& cx) {
    async2::Poll<Result<Response>> result = async2::Pending();
    if constexpr (std::is_same_v<Response, ConstBuf>) {
      result = PendRaw(cx, &request_, &internal::SerializeTypeErased<Request>);
    } else {
      PendAndDeserialize(cx,
                         &request_,
                         &internal::SerializeTypeErased<Request>,
                         &result,
                         &internal::DeserializeTypeErased<Response>);
    }
    if constexpr (std::is_same_v<Request, ConstBuf>) {
      // Release an owned request once it has been copied, rather than holding
      // it until the response arrives.
      if (!request_pending()) {
        request_.reset();
      }
    }
    return result;
  }

 private:
  friend class internal::GeneratedServiceClient;

  template <typename R>
  UnaryFuture(ReserveWriteFuture&& request_reservation, R&& request)
      : internal::UnaryFutureBase(std::move(request_reservation)),
        request_(std::forward<R>(request)) {}

  [[no_unique_address]] Request request_{};
};

static_assert(async2::Future<UnaryFuture<ConstBuf, ConstBuf>>);

namespace internal {

/// Shared implementation of `UnaryReservation` and `ServerStreamReservation`.
/// `Commit()` returns `Committed`: the call's `ResponseFuture` or `Reader`.
template <typename Committed>
class RequestReservationBase : public WriteReservationBase {
 public:
  RequestReservationBase(const RequestReservationBase&) = delete;
  RequestReservationBase& operator=(const RequestReservationBase&) = delete;

  /// Sends the first `size_bytes` bytes of the reserved buffer as the request
  /// and returns the future or reader for the server's response(s).
  ///
  /// Consumes the reservation; it cannot be used or committed again.
  /// `size_bytes` must not exceed `size()`.
  ///
  /// @returns
  /// * `OK` with the call's response future or reader once the request is
  ///   queued for transmission.
  /// * `FAILED_PRECONDITION`: this reservation was already committed, dropped,
  ///   or moved from.
  /// * The call's completion status if the call ended while the reservation
  ///   was held (for example, because its connection closed).
  /// * `UNAVAILABLE`: the connection closed before the request could be sent.
  [[nodiscard]] Result<Committed> Commit(size_t size_bytes) {
    Result<IntrusivePtr<Call>> call = CommitAndTakeCall(size_bytes);
    if (!call.ok()) {
      return call.status();
    }
    return CallAccess::Create<Committed>(std::move(*call));
  }

 protected:
  explicit RequestReservationBase(WriteReservation&& reservation)
      : WriteReservationBase(std::move(reservation)) {}

  RequestReservationBase(RequestReservationBase&&) noexcept = default;
  RequestReservationBase& operator=(RequestReservationBase&&) noexcept =
      default;
  ~RequestReservationBase() = default;
};

/// Shared implementation of `RawUnaryReserveFuture` and
/// `RawServerStreamReserveFuture`. Resolves to `ReservationType`.
template <typename ReservationType>
class ReserveRequestFutureBase {
 public:
  using Reservation = ReservationType;
  using value_type = Result<Reservation>;

  ReserveRequestFutureBase(const ReserveRequestFutureBase&) = delete;
  ReserveRequestFutureBase& operator=(const ReserveRequestFutureBase&) = delete;

  [[nodiscard]] bool is_pendable() const { return res_fut_.is_pendable(); }
  [[nodiscard]] bool is_complete() const { return res_fut_.is_complete(); }

  /// Polls for the request write reservation.
  ///
  /// @returns
  /// * `OK` with the reservation once buffer space is available.
  /// * `UNAVAILABLE` if the client is empty or its connection is closed.
  /// * `RESOURCE_EXHAUSTED` if the call could not be allocated or the
  ///   requested size exceeds the maximum write size.
  [[nodiscard]] async2::Poll<value_type> Pend(async2::Context& cx) {
    PW_TRY_READY_ASSIGN(Result<WriteReservation> res, res_fut_.Pend(cx));
    if (!res.ok()) {
      return async2::Ready(value_type(res.status()));
    }
    return async2::Ready(value_type(Reservation(std::move(*res))));
  }

 protected:
  constexpr ReserveRequestFutureBase() = default;

  explicit ReserveRequestFutureBase(ReserveWriteFuture&& res_fut)
      : res_fut_(std::move(res_fut)) {}

  ReserveRequestFutureBase(ReserveRequestFutureBase&&) noexcept = default;
  ReserveRequestFutureBase& operator=(ReserveRequestFutureBase&&) noexcept =
      default;
  ~ReserveRequestFutureBase() = default;

 private:
  ReserveWriteFuture res_fut_;
};

}  // namespace internal

/// A reserved buffer for writing a unary RPC request in place.
///
/// Write the request into the buffer and call `Commit()` with the number of
/// bytes written to send the request and obtain a `ResponseFuture<Response>`.
/// Destroying the reservation without calling `Commit()` (or calling `Drop()`)
/// abandons the call without sending anything.
template <typename Response = ConstBuf>
class UnaryReservation final
    : public internal::RequestReservationBase<ResponseFuture<Response>> {
 private:
  friend class internal::ReserveRequestFutureBase<UnaryReservation>;

  explicit UnaryReservation(WriteReservation&& reservation)
      : internal::RequestReservationBase<ResponseFuture<Response>>(
            std::move(reservation)) {}
};

using RawUnaryReservation = UnaryReservation<>;

/// A reserved buffer for writing a server-streaming RPC request in place.
///
/// Write the request into the buffer and call `Commit()` with the number of
/// bytes written to send the request and obtain a `Reader<Response>`.
/// Destroying the reservation without calling `Commit()` (or calling `Drop()`)
/// abandons the call without sending anything.
template <typename Response = ConstBuf>
class ServerStreamReservation final
    : public internal::RequestReservationBase<Reader<Response>> {
 private:
  friend class internal::ReserveRequestFutureBase<ServerStreamReservation>;

  explicit ServerStreamReservation(WriteReservation&& reservation)
      : internal::RequestReservationBase<Reader<Response>>(
            std::move(reservation)) {}
};

using RawServerStreamReservation = ServerStreamReservation<>;

/// Future that reserves a write buffer for a unary RPC request and resolves to
/// a `UnaryReservation<Response>`. Returned by
/// `client.Method(max_message_size)`.
template <typename Response = ConstBuf>
class UnaryReserveFuture final
    : public internal::ReserveRequestFutureBase<UnaryReservation<Response>> {
 public:
  constexpr UnaryReserveFuture() = default;

 private:
  friend class internal::GeneratedServiceClient;

  explicit UnaryReserveFuture(ReserveWriteFuture&& res_fut)
      : internal::ReserveRequestFutureBase<UnaryReservation<Response>>(
            std::move(res_fut)) {}
};

using RawUnaryReserveFuture = UnaryReserveFuture<>;

/// Future that reserves a write buffer for a server-streaming RPC request and
/// resolves to a `ServerStreamReservation<Response>`. Returned by
/// `client.Method(max_message_size)`.
template <typename Response = ConstBuf>
class ServerStreamReserveFuture final
    : public internal::ReserveRequestFutureBase<
          ServerStreamReservation<Response>> {
 public:
  constexpr ServerStreamReserveFuture() = default;

 private:
  friend class internal::GeneratedServiceClient;

  explicit ServerStreamReserveFuture(ReserveWriteFuture&& res_fut)
      : internal::ReserveRequestFutureBase<ServerStreamReservation<Response>>(
            std::move(res_fut)) {}
};

using RawServerStreamReserveFuture = ServerStreamReserveFuture<>;

static_assert(async2::Future<RawUnaryReserveFuture>);
static_assert(async2::Future<RawServerStreamReserveFuture>);

/// Future that sends the request for a server-streaming RPC and resolves to a
/// `Reader<Response>` for reading the server's response stream.
template <typename Request = ConstBuf, typename Response = ConstBuf>
using ServerStreamFuture = RequestFuture<Reader<Response>, Request>;

/// Client-side handle for an active client-streaming RPC.
///
/// Obtained by awaiting `ClientStreamFuture`. Use `writer()` to stream request
/// messages to the server, call `writer().Finish()` when all requests have been
/// sent, and await `response()` to receive the server's single response.
template <typename Request = ConstBuf, typename Response = ConstBuf>
class ClientStreamCall {
 public:
  using Writer = rpc2::Writer<Request>;
  using ResponseFuture = rpc2::ResponseFuture<Response>;

  constexpr ClientStreamCall() = default;

  [[nodiscard]] Writer& writer() & { return writer_; }
  [[nodiscard]] const Writer& writer() const& { return writer_; }
  [[nodiscard]] Writer&& writer() && { return std::move(writer_); }
  [[nodiscard]] const Writer&& writer() const&& { return std::move(writer_); }

  [[nodiscard]] ResponseFuture& response() & { return response_; }
  [[nodiscard]] const ResponseFuture& response() const& { return response_; }
  [[nodiscard]] ResponseFuture&& response() && { return std::move(response_); }
  [[nodiscard]] const ResponseFuture&& response() const&& {
    return std::move(response_);
  }

 private:
  friend struct internal::CallAccess;

  explicit ClientStreamCall(IntrusivePtr<internal::Call>&& call)
      : writer_(internal::CallAccess::Create<Writer>(call)),
        response_(
            internal::CallAccess::Create<ResponseFuture>(std::move(call))) {}

  Writer writer_;
  ResponseFuture response_;
};

/// Future that opens a client-streaming RPC and resolves to a
/// `ClientStreamCall<Request, Response>`.
template <typename Request = ConstBuf, typename Response = ConstBuf>
using ClientStreamFuture = RequestFuture<ClientStreamCall<Request, Response>>;
using RawClientStreamFuture = ClientStreamFuture<>;

/// Client-side handle for an active bidirectional-streaming RPC.
///
/// Obtained by awaiting `BidiStreamFuture`. Use `writer()` to send request
/// messages (calling `writer().Finish()` once all requests have been sent) and
/// `reader()` to read incoming response messages from the server. `writer()`
/// and `reader()` can be driven concurrently or moved into separate tasks.
template <typename Request = ConstBuf, typename Response = ConstBuf>
class BidiStreamCall {
 public:
  using Writer = rpc2::Writer<Request>;
  using Reader = rpc2::Reader<Response>;

  constexpr BidiStreamCall() = default;

  [[nodiscard]] Writer& writer() & { return writer_; }
  [[nodiscard]] const Writer& writer() const& { return writer_; }
  [[nodiscard]] Writer&& writer() && { return std::move(writer_); }
  [[nodiscard]] const Writer&& writer() const&& { return std::move(writer_); }

  [[nodiscard]] Reader& reader() & { return reader_; }
  [[nodiscard]] const Reader& reader() const& { return reader_; }
  [[nodiscard]] Reader&& reader() && { return std::move(reader_); }
  [[nodiscard]] const Reader&& reader() const&& { return std::move(reader_); }

 private:
  friend struct internal::CallAccess;

  explicit BidiStreamCall(IntrusivePtr<internal::Call>&& call)
      : writer_(internal::CallAccess::Create<Writer>(call)),
        reader_(internal::CallAccess::Create<Reader>(std::move(call))) {}

  Writer writer_;
  Reader reader_;
};

/// Future that opens a bidirectional-streaming RPC and resolves to a
/// `BidiStreamCall<Request, Response>`.
template <typename Request = ConstBuf, typename Response = ConstBuf>
using BidiStreamFuture = RequestFuture<BidiStreamCall<Request, Response>>;
using RawBidiStreamFuture = BidiStreamFuture<>;

static_assert(async2::Future<ServerStreamFuture<ConstBuf, ConstBuf>>);
static_assert(async2::Future<RawClientStreamFuture>);
static_assert(async2::Future<RawBidiStreamFuture>);

/// Base class for generated per-service clients. Binds a `Client` to a service
/// ID.
///
/// @note **Threading.** As with `Client`, a `ServiceClient` and the futures it
/// returns belong to the client's dispatcher thread.
class ServiceClient {
 public:
  const Client& client() const { return client_; }

  [[nodiscard]] bool is_open() const { return client_.is_open(); }

 private:
  friend class internal::GeneratedServiceClient;

  /// Constructs a service client with no connection. Calls on it fail with
  /// `UNAVAILABLE`.
  constexpr ServiceClient() = default;

  /// Binds `client` to the service with ID `service_id`.
  // NOLINTNEXTLINE(modernize-pass-by-value)
  ServiceClient(const Client& client, uint32_t service_id)
      : client_(client), service_id_(service_id) {}

  uint32_t service_id() const { return service_id_; }

  Client client_;
  uint32_t service_id_ = 0;
};

}  // namespace pw::rpc2
