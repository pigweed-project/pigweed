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
#include <utility>

#include "pw_buf/buf.h"
#include "pw_rpc2/client.h"
#include "pw_rpc2/internal/serialize.h"
#include "pw_rpc2/method_type.h"
#include "pw_rpc2/service_client.h"
#include "pw_rpc2/write_reservation.h"

namespace pw::rpc2::internal {

/// Base class for generated per-service clients. Provides the call-initiation
/// helpers that generated code wraps in typed, per-method functions.
class GeneratedServiceClient : public ServiceClient {
 protected:
  /// Constructs a service client with no connection. Calls on it fail with
  /// `UNAVAILABLE`.
  constexpr GeneratedServiceClient() = default;

  /// Binds `client` to the service with ID `service_id`.
  GeneratedServiceClient(Client client, uint32_t service_id)
      : ServiceClient(std::move(client), service_id) {}

  // To limit code size, these templates do only payload-type-dependent work.
  // Everything else is in the non-template `StartCall()`. Calls are allocated
  // immediately, but nothing is sent until the returned future is polled.

  /// Starts a unary call that sends `request`.
  template <typename Request, typename Response>
  [[nodiscard]] UnaryFuture<Request, Response> CallUnary(
      uint32_t method_id, Request request) const {
    ReserveWriteFuture req = StartCall(
        method_id, MethodType::kUnary, &request, &RequestSize<Request>);
    return UnaryFuture<Request, Response>(std::move(req), std::move(request));
  }

  /// Starts a raw unary call whose request is written into a reservation of up
  /// to `max_payload_size` bytes.
  [[nodiscard]] RawUnaryReserveFuture CallUnaryRaw(
      uint32_t method_id, size_t max_payload_size) const;

  /// Starts a server-streaming call that sends `request`.
  template <typename Request, typename Response>
  [[nodiscard]] ServerStreamFuture<Request, Response> CallServerStream(
      uint32_t method_id, Request request) const {
    ReserveWriteFuture req = StartCall(method_id,
                                       MethodType::kServerStreaming,
                                       &request,
                                       &RequestSize<Request>);
    return ServerStreamFuture<Request, Response>(std::move(req),
                                                 std::move(request));
  }

  /// Starts a raw server-streaming call whose request is written into a
  /// reservation of up to `max_payload_size` bytes.
  [[nodiscard]] RawServerStreamReserveFuture CallServerStreamRaw(
      uint32_t method_id, size_t max_payload_size) const;

  /// Starts a client-streaming call.
  template <typename Request, typename Response>
  [[nodiscard]] ClientStreamFuture<Request, Response> CallClientStream(
      uint32_t method_id) const {
    return ClientStreamFuture<Request, Response>(
        StartCall(method_id, MethodType::kClientStreaming, nullptr, nullptr));
  }

  /// Starts a bidirectional-streaming call.
  template <typename Request, typename Response>
  [[nodiscard]] BidiStreamFuture<Request, Response> CallBidiStream(
      uint32_t method_id) const {
    return BidiStreamFuture<Request, Response>(StartCall(
        method_id, MethodType::kBidirectionalStreaming, nullptr, nullptr));
  }

 private:
  /// Returns the number of payload bytes to reserve for `request`, given the
  /// largest payload a start packet can carry.
  using RequestSizeFn = size_t (*)(const void* request, size_t payload_limit);

  template <typename Request>
  static size_t RequestSize(const void* request, size_t payload_limit) {
    return ReservationSize(*static_cast<const Request*>(request),
                           payload_limit);
  }

  /// Allocates a call and reserves its start packet. `size_request` sizes the
  /// request payload for unary and server-streaming calls; it is null for
  /// client- and bidirectional-streaming calls, whose start packet has no
  /// payload.
  ///
  /// Failures are reported through the returned reservation future:
  ///
  /// * `UNAVAILABLE`: the client is empty or closed.
  /// * `RESOURCE_EXHAUSTED`: the call could not be allocated, or the request
  ///   exceeds the transport's maximum write size.
  ReserveWriteFuture StartCall(uint32_t method_id,
                               MethodType type,
                               const void* request,
                               RequestSizeFn size_request) const;
};

}  // namespace pw::rpc2::internal
