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

#include "pw_assert/assert.h"
#include "pw_buf/buf.h"
#include "pw_rpc2/client.h"
#include "pw_rpc2/internal/serialize.h"
#include "pw_rpc2/method_type.h"
#include "pw_rpc2/service_client.h"
#include "pw_rpc2/write_reservation.h"

namespace pw::rpc2::internal {

/// Restricts `client.Method(max_message_size)` to integers other than `bool`.
///
/// Being a template also prevents braced initializers (`client.Method({})`)
/// from matching, so they select the message overload instead.
template <typename Size>
using EnableIfMessageSize =
    std::enable_if_t<std::is_integral_v<Size> && !std::is_same_v<Size, bool>>;

/// Converts the argument of `client.Method(max_message_size)` to `size_t`.
/// Negative sizes are a programming error, so they are caught in debug builds
/// rather than wrapping around to a huge reservation.
template <typename Size>
size_t MessageSize(Size max_message_size) {
  static_assert(sizeof(Size) <= sizeof(size_t),
                "max_message_size cannot be larger than size_t");
  if constexpr (std::is_signed_v<Size>) {
    PW_DASSERT(max_message_size >= 0);
  }
  return static_cast<size_t>(max_message_size);
}

/// Base class for generated per-service clients. Provides the call-initiation
/// helpers that generated code wraps in typed, per-method functions.
class GeneratedServiceClient : public ServiceClient {
 protected:
  /// Constructs a service client with no connection. Calls on it fail with
  /// `UNAVAILABLE`.
  constexpr GeneratedServiceClient() = default;

  /// Binds `client` to the service with ID `service_id`.
  GeneratedServiceClient(const Client& client, uint32_t service_id)
      : ServiceClient(client, service_id) {}

  // To limit code size, these templates do only message-type-dependent work.
  // Everything else is in `StartCall()`. Calls are allocated immediately, but
  // nothing is sent until the returned future is polled.

  /// Starts a unary call that sends `request`.
  template <typename Request, typename Response, typename R>
  [[nodiscard]] UnaryFuture<Request, Response> CallUnary(uint32_t method_id,
                                                         R&& request) const {
    static_assert(
        std::is_same_v<std::remove_cv_t<std::remove_reference_t<R>>, Request>);
    ReserveWriteFuture req;
    if constexpr (std::is_same_v<Request, ConstBuf>) {
      req = StartReservedCall(method_id, MethodType::kUnary, request.size());
    } else {
      req = StartCall(
          method_id, MethodType::kUnary, &request, &RequestSize<Request>, 0);
    }
    return UnaryFuture<Request, Response>(std::move(req),
                                          std::forward<R>(request));
  }

  /// Starts a unary call that reserves up to `max_message_size` bytes for the
  /// request.
  template <typename Response>
  [[nodiscard]] UnaryReserveFuture<Response> ReserveUnary(
      uint32_t method_id, size_t max_message_size) const {
    return UnaryReserveFuture<Response>(
        StartReservedCall(method_id, MethodType::kUnary, max_message_size));
  }

  /// Starts a server-streaming call that sends `request`.
  template <typename Request, typename Response, typename R>
  [[nodiscard]] ServerStreamFuture<Request, Response> CallServerStream(
      uint32_t method_id, R&& request) const {
    static_assert(
        std::is_same_v<std::remove_cv_t<std::remove_reference_t<R>>, Request>);
    ReserveWriteFuture req;
    if constexpr (std::is_same_v<Request, ConstBuf>) {
      req = StartReservedCall(
          method_id, MethodType::kServerStreaming, request.size());
    } else {
      req = StartCall(method_id,
                      MethodType::kServerStreaming,
                      &request,
                      &RequestSize<Request>,
                      0);
    }
    return ServerStreamFuture<Request, Response>(std::move(req),
                                                 std::forward<R>(request));
  }

  /// Starts a server-streaming call that reserves up to `max_message_size`
  /// bytes for the request.
  template <typename Response>
  [[nodiscard]] ServerStreamReserveFuture<Response> ReserveServerStream(
      uint32_t method_id, size_t max_message_size) const {
    return ServerStreamReserveFuture<Response>(StartReservedCall(
        method_id, MethodType::kServerStreaming, max_message_size));
  }

  /// Starts a client-streaming call.
  template <typename Request, typename Response>
  [[nodiscard]] ClientStreamFuture<Request, Response> CallClientStream(
      uint32_t method_id) const {
    return ClientStreamFuture<Request, Response>(
        StartReservedCall(method_id, MethodType::kClientStreaming, 0));
  }

  /// Starts a bidirectional-streaming call.
  template <typename Request, typename Response>
  [[nodiscard]] BidiStreamFuture<Request, Response> CallBidiStream(
      uint32_t method_id) const {
    return BidiStreamFuture<Request, Response>(
        StartReservedCall(method_id, MethodType::kBidirectionalStreaming, 0));
  }

 private:
  template <typename, uint32_t, MethodType, typename>
  friend class CopyMethod;

  /// Returns the number of bytes to reserve for `request`, given the largest
  /// message a start packet can carry.
  using RequestSizeFn = size_t (*)(const void* request, size_t payload_limit);

  template <typename Request>
  static size_t RequestSize(const void* request, size_t payload_limit) {
    return ReservationSize(*static_cast<const Request*>(request),
                           payload_limit);
  }

  /// Allocates a call and reserves its start packet. If `size_request` is
  /// provided, it computes the number of payload bytes to reserve; otherwise
  /// `max_message_size` bytes are reserved.
  ///
  /// Failures are reported through the returned reservation future:
  ///
  /// * `UNAVAILABLE`: the client is empty or closed.
  /// * `RESOURCE_EXHAUSTED`: the call could not be allocated, or the request
  ///   exceeds the transport's maximum write size.
  ReserveWriteFuture StartCall(uint32_t method_id,
                               MethodType type,
                               const void* request,
                               RequestSizeFn size_request,
                               size_t max_message_size) const;

  /// Like `StartCall()`, but reserves `max_message_size` bytes directly.
  ReserveWriteFuture StartReservedCall(uint32_t method_id,
                                       MethodType type,
                                       size_t max_message_size) const {
    return StartCall(method_id, type, nullptr, nullptr, max_message_size);
  }
};

/// Provides `client.Method::Copy(request)` for a unary or server-streaming
/// method.
///
/// Generated clients inherit one of these per single-request method through a
/// base class that aliases it to the method name (`using Method = ...;`).
/// `client.Method` resolves to the member function, while `client.Method::Copy`
/// resolves to the type alias because lookup before `::` considers only types
/// and namespaces.
template <typename DerivedClient,
          uint32_t kMethodId,
          MethodType kType,
          typename Response>
class CopyMethod {
 public:
  using Future = std::conditional_t<kType == MethodType::kUnary,
                                    UnaryFuture<ConstBuf, Response>,
                                    ServerStreamFuture<ConstBuf, Response>>;

  /// Starts a call that copies `request` into the outbound buffer.
  ///
  /// Pass `ConstBuf::Unowned(bytes)` to copy from bytes that outlive the
  /// returned future, or move in an owned `ConstBuf`, which is released once
  /// copied.
  [[nodiscard]] Future Copy(ConstBuf&& request) const {
    const GeneratedServiceClient& client =
        static_cast<const DerivedClient&>(*this);
    if constexpr (kType == MethodType::kUnary) {
      return client.CallUnary<ConstBuf, Response>(kMethodId,
                                                  std::move(request));
    } else {
      return client.CallServerStream<ConstBuf, Response>(kMethodId,
                                                         std::move(request));
    }
  }

 protected:
  constexpr CopyMethod() = default;
  constexpr CopyMethod(const CopyMethod&) = default;
  constexpr CopyMethod& operator=(const CopyMethod&) = default;
};

template <typename DerivedClient, uint32_t kMethodId, typename Response>
using UnaryCopyMethod =
    CopyMethod<DerivedClient, kMethodId, MethodType::kUnary, Response>;

template <typename DerivedClient, uint32_t kMethodId, typename Response>
using ServerStreamCopyMethod = CopyMethod<DerivedClient,
                                          kMethodId,
                                          MethodType::kServerStreaming,
                                          Response>;

}  // namespace pw::rpc2::internal
