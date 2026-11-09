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

// Implementation details of pw_rpc/rpc2_adapter.h.

#include <array>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>

#include "pw_async2/context.h"
#include "pw_async2/future.h"
#include "pw_async2/poll.h"
#include "pw_async2/waker.h"
#include "pw_buf/buf.h"
#include "pw_bytes/span.h"
#include "pw_containers/intrusive_forward_list.h"
#include "pw_containers/intrusive_queue.h"
#include "pw_rpc/internal/method_info.h"
#include "pw_rpc/internal/method_lookup.h"
#include "pw_rpc/internal/packet.h"
#include "pw_rpc/method_type.h"
#include "pw_rpc/service.h"
#include "pw_rpc/service_id.h"
#include "pw_rpc2/internal/method.h"
#include "pw_rpc2/internal/method_future.h"
#include "pw_rpc2/internal/server_connection_task.h"
#include "pw_rpc2/method_type.h"
#include "pw_rpc2/reader.h"
#include "pw_rpc2/writer.h"

namespace pw::rpc2 {
class Service;
namespace internal {
class ServerCall;
}  // namespace internal
}  // namespace pw::rpc2

namespace pw::rpc {

class Rpc2ServerAdapter;

namespace internal {

// Binds one pw_rpc2 connection to one legacy pw_rpc channel for the
// connection's lifetime. Owned by an Rpc2ServerAdapter and guarded by its lock.
//
// Bindings are never freed while the adapter runs. Once a connection closes,
// its binding (and channel) is taken over by a later connection.
struct Rpc2ChannelBinding {
  // Creates a binding for a new connection's first call.
  Rpc2ChannelBinding(rpc2::internal::ConnectionId connection_id,
                     uint32_t channel,
                     bool open)
      : connection(connection_id),
        channel_id(channel),
        live_calls(1),
        channel_open(open) {}

  // The pw_rpc2 connection that holds this binding. Remains valid to compare
  // after the connection closes.
  rpc2::internal::ConnectionId connection;
  uint32_t channel_id;
  // Number of Rpc2LegacyCalls using this binding. A binding with live calls
  // belongs to a live connection.
  uint16_t live_calls;
  // True while `channel_id` is open on the server: once the adapter's worker
  // has opened it, or always for an Rpc2Adapter, whose server sends
  // every channel's packets to the adapter as its default channel output.
  bool channel_open;
};

// A packet queued between the two sides of an Rpc2ServerAdapter. Its payload is
// stored in the same allocation, immediately after the object.
class Rpc2QueuedPacket : public IntrusiveQueue<Rpc2QueuedPacket>::Item {
 public:
  Rpc2QueuedPacket(Rpc2ChannelBinding& binding,
                   pwpb::PacketType type,
                   uint32_t service_id,
                   uint32_t method_id,
                   size_t payload_size)
      : binding_(&binding),
        payload_size_(payload_size),
        service_id_(service_id),
        method_id_(method_id),
        type_(type) {}

  Rpc2ChannelBinding& binding() const { return *binding_; }
  pwpb::PacketType type() const { return type_; }
  bool is_cancel() const { return type_ == pwpb::PacketType::CLIENT_ERROR; }
  uint32_t service_id() const { return service_id_; }
  uint32_t method_id() const { return method_id_; }
  uint32_t call_id() const { return call_id_; }
  void set_call_id(uint32_t call_id) { call_id_ = call_id; }

  ByteSpan payload() {
    return ByteSpan(reinterpret_cast<std::byte*>(this + 1), payload_size_);
  }
  ConstByteSpan payload() const {
    return ConstByteSpan(reinterpret_cast<const std::byte*>(this + 1),
                         payload_size_);
  }

  Packet ToPacket(uint32_t channel_id) const {
    return Packet(type_,
                  channel_id,
                  service_id_,
                  method_id_,
                  call_id_,
                  payload(),
                  is_cancel() ? Status::Cancelled() : OkStatus());
  }

  // Repurposes an already-processed REQUEST packet as a preallocated
  // CLIENT_ERROR packet, so cancelling a call never needs to allocate.
  void BecomeCancel() {
    payload_size_ = 0;
    type_ = pwpb::PacketType::CLIENT_ERROR;
  }

 private:
  Rpc2ChannelBinding* binding_;
  size_t payload_size_;
  uint32_t service_id_;
  uint32_t method_id_;
  uint32_t call_id_ = 0;
  pwpb::PacketType type_;
};

// The pw_rpc2 method future for one call to an adapted pw_rpc service.
//
// Runs on the pw_rpc2 server's dispatcher. Forwards the pw_rpc2 call's request
// (and, for client and bidirectional streaming RPCs, its request stream) to the
// legacy server through the adapter's work queue, and writes the packets the
// legacy server sends for the call to the pw_rpc2 call.
class Rpc2LegacyCall {
 public:
  using value_type = void;

  // Starts a call to an adapted method. `service` is an Rpc2ServiceAdapter.
  static rpc2::internal::ProtocolError Invoke(rpc2::Service& service,
                                              rpc2::internal::ServerCall& call,
                                              ConstBuf&& request,
                                              size_t method_index_and_flags);

  Rpc2LegacyCall() = default;

  Rpc2LegacyCall(const Rpc2LegacyCall&) = delete;
  Rpc2LegacyCall& operator=(const Rpc2LegacyCall&) = delete;

  // The pw::async2::Future concept requires std::movable, but a started call is
  // registered with the adapter (and its Shared state is an intrusive list
  // item), so it may not move. Calls are emplaced directly into their pw_rpc2
  // call with C++17 mandatory copy elision, so they are never moved. These are
  // declared to satisfy the concept, but intentionally not defined, so any
  // attempt to move a call fails to link.
  Rpc2LegacyCall(Rpc2LegacyCall&&) noexcept;
  Rpc2LegacyCall& operator=(Rpc2LegacyCall&&) noexcept;

  ~Rpc2LegacyCall();

  [[nodiscard]] bool is_pendable() const {
    return adapter_ != nullptr && !complete_;
  }
  [[nodiscard]] bool is_complete() const { return complete_; }

  async2::Poll<> Pend(async2::Context& cx);

 private:
  friend class ::pw::rpc::Rpc2ServerAdapter;

  // State shared between this call and the adapter, which may touch it from
  // any thread. Guarded by the adapter's lock once the call is registered.
  struct Shared : public IntrusiveForwardList<Shared>::Item {
    // State of a default-constructed (inactive) call.
    Shared()
        : binding(nullptr),
          call_id(0),
          service_id(0),
          method_id(0),
          type(rpc2::MethodType::kUnary),
          deliver_non_ok_response(false) {}

    // State of a call started by `request`.
    Shared(Rpc2QueuedPacket& request,
           rpc2::MethodType method_type,
           bool deliver_non_ok)
        : binding(&request.binding()),
          call_id(request.call_id()),
          service_id(request.service_id()),
          method_id(request.method_id()),
          type(method_type),
          deliver_non_ok_response(deliver_non_ok) {}

    // The binding of the pw_rpc2 connection this call is on.
    Rpc2ChannelBinding* const binding;
    // Packets the legacy server sent (stream messages, then the response of a
    // unary or client streaming RPC), not yet written to pw_rpc2.
    IntrusiveQueue<Rpc2QueuedPacket> inbox;
    // Preallocated CLIENT_ERROR packet that cancels the legacy call, set while
    // the legacy call is open.
    Rpc2QueuedPacket* cancel = nullptr;
    async2::Waker waker;
    const uint32_t call_id;  // Unique among the adapter's calls.
    const uint32_t service_id;
    const uint32_t method_id;
    uint16_t queued = 0;  // Number of packets in `inbox`.
    const rpc2::MethodType type;
    const bool deliver_non_ok_response;
    // How the legacy call ended, once `done` is set.
    rpc2::internal::ProtocolError error = rpc2::internal::ProtocolError::kNone;
    // True once the legacy call has ended. No more packets are queued.
    bool done = false;
    // True while a packet this call sent to the legacy server is queued or
    // being processed. The call sends one packet at a time.
    bool inbound_in_flight = false;
  };

  Rpc2LegacyCall(Rpc2ServerAdapter& adapter,
                 rpc2::internal::ServerCall& call,
                 rpc2::MethodType type,
                 bool deliver_non_ok_response,
                 Rpc2QueuedPacket& request);

  // Writes the packets the legacy server sent to the pw_rpc2 call. Ready once
  // the pw_rpc2 call has ended.
  async2::Poll<> PendLegacyToRpc2(async2::Context& cx);

  // Forwards the pw_rpc2 request stream to the legacy server. Ready if the
  // pw_rpc2 call ended.
  async2::Poll<> PendRpc2ToLegacy(async2::Context& cx);

  // Ends the pw_rpc2 call with `error`.
  void FailRpc2(rpc2::internal::ProtocolError error);

  Rpc2ServerAdapter* adapter_ = nullptr;
  Shared shared_;

  rpc2::RawReader reader_;
  rpc2::RawReadFuture read_;
  rpc2::RawWriter writer_;
  rpc2::RawUnaryWriter unary_writer_;

  // Writes `writing_`: a stream message, or the single response.
  rpc2::WriteFuture<ConstBuf> write_;
  Rpc2QueuedPacket* writing_ = nullptr;

  bool complete_ = false;
  bool final_write_ = false;
  bool client_stream_done_ = false;
};

static_assert(async2::Future<Rpc2LegacyCall>);

// pw_rpc and pw_rpc2 encode method types identically.
static_assert(static_cast<uint8_t>(MethodType::kUnary) ==
              static_cast<uint8_t>(rpc2::MethodType::kUnary));
static_assert(static_cast<uint8_t>(MethodType::kServerStreaming) ==
              static_cast<uint8_t>(rpc2::MethodType::kServerStreaming));
static_assert(static_cast<uint8_t>(MethodType::kClientStreaming) ==
              static_cast<uint8_t>(rpc2::MethodType::kClientStreaming));
static_assert(static_cast<uint8_t>(MethodType::kBidirectionalStreaming) ==
              static_cast<uint8_t>(rpc2::MethodType::kBidirectionalStreaming));

constexpr rpc2::MethodType ToRpc2MethodType(MethodType type) {
  return static_cast<rpc2::MethodType>(type);
}

// The pw_rpc2 invoker for the kIndex-th method of an adapted service. Packing
// the method index and non-OK policy into one argument lets adapted services
// share 4-argument tail-calling invokers across the binary.
template <size_t kIndex, bool kDeliverNonOkResponse>
rpc2::internal::ProtocolError InvokeAdaptedMethod(
    rpc2::Service& service,
    rpc2::internal::ServerCall& call,
    ConstBuf&& request) {
  return Rpc2LegacyCall::Invoke(
      service,
      call,
      std::move(request),
      (kIndex << 1) | static_cast<size_t>(kDeliverNonOkResponse));
}

template <auto... kMethods>
struct DeliverNonOkResponsesTag {
  explicit constexpr DeliverNonOkResponsesTag() = default;
};

template <typename T>
inline constexpr bool kIsDeliverNonOkResponsesTag = false;
template <auto... kMethods>
inline constexpr bool
    kIsDeliverNonOkResponsesTag<DeliverNonOkResponsesTag<kMethods...>> = true;

template <typename... Args>
inline constexpr bool kStartsWithDeliverNonOkTag = false;
template <typename First, typename... Rest>
inline constexpr bool kStartsWithDeliverNonOkTag<First, Rest...> =
    kIsDeliverNonOkResponsesTag<
        std::remove_cv_t<std::remove_reference_t<First>>>;

template <typename LegacyService, auto kMethod>
constexpr void ValidateNonOkResponseMethod() {
  using Info = MethodInfo<kMethod>;
  if constexpr (Info::kServiceId != 0 &&
                std::is_base_of_v<rpc::Service, LegacyService>) {
    static_assert(
        Info::kServiceId ==
            UnwrapServiceId(LegacyService::ServiceInfo::service_id()),
        "The specified pw_rpc method does not belong to this service");
    static_assert(
        !HasServerStream(Info::kType),
        "Only unary and client streaming pw_rpc methods send a response "
        "payload in their final packet; server and bidirectional streaming "
        "methods cannot be configured to deliver non-OK responses");
  }
}

template <typename LegacyService,
          bool kEnableNonOk,
          auto... kNonOkResponseMethods>
constexpr bool DeliversNonOkResponse(uint32_t method_id, MethodType type) {
  (ValidateNonOkResponseMethod<LegacyService, kNonOkResponseMethods>(), ...);
  if constexpr (!kEnableNonOk) {
    return false;
  } else if constexpr (sizeof...(kNonOkResponseMethods) == 0) {
    return !HasServerStream(type);
  } else {
    return ((MethodInfo<kNonOkResponseMethods>::kMethodId == method_id) || ...);
  }
}

template <typename LegacyService>
constexpr uint32_t AdaptedServiceId(const LegacyService& service) {
  static_assert(std::is_base_of_v<rpc::Service, LegacyService>,
                "Rpc2ServiceAdapter wraps a pw_rpc service implementation, "
                "which derives from a generated pw_rpc Service<Impl> class");
  if constexpr (std::is_base_of_v<rpc::Service, LegacyService>) {
    return UnwrapServiceId(service.service_id());
  } else {
    return 0;
  }
}

template <typename LegacyService>
constexpr size_t AdaptedMethodCount() {
  static_assert(std::is_base_of_v<rpc::Service, LegacyService>,
                "Rpc2ServiceAdapter wraps a pw_rpc service implementation, "
                "which derives from a generated pw_rpc Service<Impl> class");
  if constexpr (std::is_base_of_v<rpc::Service, LegacyService>) {
    return std::tuple_size_v<std::remove_reference_t<
        decltype(MethodLookup::MethodIds<LegacyService>())>>;
  } else {
    return 0;
  }
}

// Builds the pw_rpc2 method table for a generated legacy service from the
// method IDs and types in its generated code.
template <typename LegacyService,
          bool kEnableNonOk,
          auto... kNonOkResponseMethods,
          size_t... kIndex>
constexpr std::array<rpc2::internal::Method, sizeof...(kIndex)>
MakeAdaptedMethods(std::index_sequence<kIndex...>) {
  (ValidateNonOkResponseMethod<LegacyService, kNonOkResponseMethods>(), ...);
  if constexpr (std::is_base_of_v<rpc::Service, LegacyService>) {
    constexpr const auto& kIds = MethodLookup::MethodIds<LegacyService>();
    constexpr const auto& kTypes = MethodLookup::MethodTypes<LegacyService>();
    return {{rpc2::internal::Method(
        kIds[kIndex],
        ToRpc2MethodType(kTypes[kIndex]),
        sizeof(rpc2::internal::MethodFutureImpl<Rpc2LegacyCall>),
        &InvokeAdaptedMethod<kIndex,
                             DeliversNonOkResponse<LegacyService,
                                                   kEnableNonOk,
                                                   kNonOkResponseMethods...>(
                                 kIds[kIndex], kTypes[kIndex])>)...}};
  } else {
    return {};
  }
}

template <typename LegacyService>
inline constexpr auto kAdaptedMethods =
    MakeAdaptedMethods<LegacyService, /*kEnableNonOk=*/false>(
        std::make_index_sequence<AdaptedMethodCount<LegacyService>()>());

template <typename LegacyService, auto... kNonOkResponseMethods>
inline constexpr auto kAdaptedMethodsWithNonOkResponses =
    MakeAdaptedMethods<LegacyService,
                       /*kEnableNonOk=*/true,
                       kNonOkResponseMethods...>(
        std::make_index_sequence<AdaptedMethodCount<LegacyService>()>());

}  // namespace internal
}  // namespace pw::rpc
