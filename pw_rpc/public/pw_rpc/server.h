// Copyright 2020 The Pigweed Authors
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
#include <tuple>

#include "pw_containers/intrusive_list.h"
#include "pw_rpc/channel.h"
#include "pw_rpc/internal/call.h"
#include "pw_rpc/internal/endpoint.h"
#include "pw_rpc/internal/grpc.h"
#include "pw_rpc/internal/lock.h"
#include "pw_rpc/internal/method.h"
#include "pw_rpc/internal/method_info.h"
#include "pw_rpc/internal/server_call.h"
#include "pw_rpc/service.h"
#include "pw_span/span.h"
#include "pw_status/status.h"

namespace pw::rpc {

/// @module{pw_rpc}

class Server : public internal::Endpoint {
 public:
  // If dynamic allocation is supported, it is not necessary to preallocate a
  // channels list.
#if PW_RPC_DYNAMIC_ALLOCATION
  _PW_RPC_CONSTEXPR Server() = default;
#endif  // PW_RPC_DYNAMIC_ALLOCATION

  // Creates a client that uses a set of RPC channels. Channels can be shared
  // between multiple clients and servers.
  _PW_RPC_CONSTEXPR Server(span<Channel> channels) : Endpoint(channels) {}

  // Registers one or more services with the server. This should not be called
  // directly with a Service; instead, use a generated class which inherits
  // from it.
  //
  // This function may be called with any number of services. Combining
  // registration into fewer calls is preferred so the RPC mutex is only
  // locked/unlocked once.
  template <typename... OtherServices>
  void RegisterService(Service& service, OtherServices&... services)
      PW_LOCKS_EXCLUDED(internal::rpc_lock()) {
    internal::RpcLockGuard lock;
    services_.push_front(service);  // Register the first service

    // Register any additional services by expanding the parameter pack. This
    // is a fold expression of the comma operator.
    (services_.push_front(services), ...);
  }

  // Returns whether a service is registered.
  //
  // Calling RegisterService with a registered service will assert. So depending
  // on your logic you might want to check if a service is currently registered.
  bool IsServiceRegistered(const Service& service) const
      PW_LOCKS_EXCLUDED(internal::rpc_lock()) {
    internal::RpcLockGuard lock;

    for (const Service& svc : services_) {
      if (&svc == &service) {
        return true;
      }
    }

    return false;
  }

  template <typename... OtherServices>
  void UnregisterService(Service& service, OtherServices&... services)
      PW_LOCKS_EXCLUDED(internal::rpc_lock()) {
    internal::rpc_lock().lock();
    UnregisterServiceLocked(service, static_cast<Service&>(services)...);
    CleanUpCalls();
  }

  // Processes an RPC packet. The packet may contain an RPC request or a control
  // packet, the result of which is processed in this function. Returns whether
  // the packet was able to be processed:
  //
  //   OK - The packet was processed by the server.
  //   DATA_LOSS - Failed to decode the packet.
  //   INVALID_ARGUMENT - The packet is intended for a client, not a server.
  //   UNAVAILABLE - No RPC channel with the requested ID was found.
  Status ProcessPacket(ConstByteSpan packet_data)
      PW_LOCKS_EXCLUDED(internal::rpc_lock());

 private:
  friend class internal::Call;
  friend class QueryService;
  friend class ServerTestHelper;

  // Iterates over all registered services.
  template <typename UnaryFunc>
  UnaryFunc ForEachService(UnaryFunc func) const
      PW_LOCKS_EXCLUDED(internal::rpc_lock()) {
    internal::RpcLockGuard lock;

    for (const auto& service : services_) {
      func(service);
    }

    return func;
  }

  template <typename UnaryPred>
  const Service* FindServiceIf(UnaryPred predicate)
      PW_LOCKS_EXCLUDED(internal::rpc_lock()) {
    internal::RpcLockGuard lock;

    for (const auto& service : services_) {
      if (predicate(service)) {
        return &service;
      }
    }

    return nullptr;
  }

  // Give gRPC integration access to FindMethod and internal::Packet version of
  // ProcessPacket
  friend class pw::grpc::PwRpcHandler;

  // Give call classes access to OpenCall.
  friend class RawServerReaderWriter;
  friend class RawServerWriter;
  friend class RawServerReader;
  friend class RawUnaryResponder;

  template <typename, typename>
  friend class NanopbServerReaderWriter;
  template <typename>
  friend class NanopbServerWriter;
  template <typename, typename>
  friend class NanopbServerReader;
  template <typename>
  friend class NanopbUnaryResponder;

  template <typename, typename>
  friend class PwpbServerReaderWriter;
  template <typename>
  friend class PwpbServerWriter;
  template <typename, typename>
  friend class PwpbServerReader;
  template <typename>
  friend class PwpbUnaryResponder;

  // Opens a call object for an unrequested RPC. Calls created with OpenCall
  // use a special call ID and will adopt the call ID from the first packet for
  // their channel, service, and method. Only one call object may be opened in
  // this fashion at a time.
  //
  // This function checks the type of RPC at compile time.
  template <typename CallType,
            auto kMethod,
            MethodType kExpected,
            typename ServiceImpl,
            typename MethodImpl>
  [[nodiscard]] CallType OpenCall(uint32_t channel_id,
                                  ServiceImpl& service,
                                  const MethodImpl& method)
      PW_LOCKS_EXCLUDED(internal::rpc_lock()) {
    internal::rpc_lock().lock();

    using Info = internal::MethodInfo<kMethod>;
    if constexpr (kExpected == MethodType::kUnary) {
      static_assert(
          Info::kType == kExpected,
          "UnaryResponder objects may only be opened for unary RPCs.");
    } else if constexpr (kExpected == MethodType::kServerStreaming) {
      static_assert(
          Info::kType == kExpected,
          "ServerWriters may only be opened for server streaming RPCs.");
    } else if constexpr (kExpected == MethodType::kClientStreaming) {
      static_assert(
          Info::kType == kExpected,
          "ServerReaders may only be opened for client streaming RPCs.");
    } else if constexpr (kExpected == MethodType::kBidirectionalStreaming) {
      static_assert(Info::kType == kExpected,
                    "ServerReaderWriters may only be opened for bidirectional "
                    "streaming RPCs.");
    }

    CallType call(internal::CallContext(
                      *this, channel_id, service, method, internal::kOpenCallId)
                      .ClaimLocked());
    CleanUpCalls();
    return call;
  }

  std::tuple<Service*, const internal::Method*> FindMethod(uint32_t service_id,
                                                           uint32_t method_id)
      PW_LOCKS_EXCLUDED(internal::rpc_lock());

  std::tuple<Service*, const internal::Method*> FindMethodLocked(
      uint32_t service_id, uint32_t method_id)
      PW_EXCLUSIVE_LOCKS_REQUIRED(internal::rpc_lock());

  std::tuple<Service*, const internal::Method*> FindMethodLocked(
      const internal::Packet& packet)
      PW_EXCLUSIVE_LOCKS_REQUIRED(internal::rpc_lock()) {
    // Packets always include service and method IDs.
    return FindMethodLocked(packet.service_id(), packet.method_id());
  }

  void HandleCompletionRequest(
      const internal::Packet& packet,
      internal::ChannelBase& channel,
      IntrusiveForwardList<internal::Call>::iterator call) const
      PW_UNLOCK_FUNCTION(internal::rpc_lock());

  void HandleClientStreamPacket(
      const internal::Packet& packet,
      internal::ChannelBase& channel,
      IntrusiveForwardList<internal::Call>::iterator call) const
      PW_UNLOCK_FUNCTION(internal::rpc_lock());

  template <typename... OtherServices>
  void UnregisterServiceLocked(Service& service, OtherServices&... services)
      PW_EXCLUSIVE_LOCKS_REQUIRED(internal::rpc_lock()) {
    services_.remove(service);
    UnregisterServiceLocked(services...);
    AbortCallsForService(service);
  }

  void UnregisterServiceLocked() {}  // Base case; nothing left to do.

  Status ProcessPacket(internal::Packet packet)
      PW_LOCKS_EXCLUDED(internal::rpc_lock());

  // Remove these internal::Endpoint functions from the public interface.
  using Endpoint::active_call_count;
  using Endpoint::ClaimLocked;
  using Endpoint::CleanUpCalls;
  using Endpoint::GetInternalChannel;

  IntrusiveList<Service> services_ PW_GUARDED_BY(internal::rpc_lock());
};

/// @}

}  // namespace pw::rpc
