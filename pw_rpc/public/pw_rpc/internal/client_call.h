// Copyright 2021 The Pigweed Authors
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

#include <cstdint>

#include "pw_bytes/span.h"
#include "pw_function/function.h"
#include "pw_rpc/internal/call.h"
#include "pw_rpc/internal/config.h"
#include "pw_rpc/internal/endpoint.h"
#include "pw_rpc/internal/lock.h"

#if PW_RPC_DYNAMIC_ALLOCATION
#include PW_RPC_MAKE_UNIQUE_PTR_INCLUDE
#endif  // PW_RPC_DYNAMIC_ALLOCATION

namespace pw::rpc::internal {

// A Call object, as used by an RPC client.
class ClientCall : public Call {
 public:
  uint32_t id() const PW_LOCKS_EXCLUDED(rpc_lock()) {
    RpcLockGuard lock;
    return Call::id();
  }

 protected:
  // Initializes CallProperties for a struct-based client call impl.
  static constexpr CallProperties StructCallProps(MethodType type) {
    return CallProperties(type, kClientCall, kProtoStruct);
  }

  // Initializes CallProperties for a raw client call.
  static constexpr CallProperties RawCallProps(MethodType type) {
    return CallProperties(type, kClientCall, kRawProto);
  }

  constexpr ClientCall() = default;

  ClientCall(LockedEndpoint& client,
             uint32_t channel_id,
             uint32_t service_id,
             uint32_t method_id,
             CallProperties properties) PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock())
      : Call(client, channel_id, service_id, method_id, properties) {}

  ~ClientCall() { DestroyClientCall(); }

  // Public function that closes a call client-side without cancelling it on the
  // server.
  void Abandon() PW_LOCKS_EXCLUDED(rpc_lock()) {
    RpcLockGuard lock;
    CloseClientCall();
  }

  void CloseAndWaitForCallbacks() { DestroyClientCall(); }

  void MoveClientCallFrom(ClientCall& other)
      PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock());
};

// Unary response client calls receive both a payload and the status in their
// on_completed callback. The on_next callback is not used.
class UnaryResponseClientCall : public ClientCall {
 public:
  ~UnaryResponseClientCall() { DestroyClientCall(); }

  // Start call for raw unary response RPCs.
  template <typename CallType>
  static CallType Start(Endpoint& client,
                        uint32_t channel_id,
                        uint32_t service_id,
                        uint32_t method_id,
                        Function<void(ConstByteSpan, Status)>&& on_completed,
                        Function<void(Status)>&& on_error,
                        ConstByteSpan request) PW_LOCKS_EXCLUDED(rpc_lock()) {
    PW_ASSERT(PW_RPC_ALLOW_INVOCATIONS_ON_STACK);  // RPC client calls on the
                                                   // stack are not allowed. Use
                                                   // DynamicClient instead.

    rpc_lock().lock();
    CallType call(client.ClaimLocked(), channel_id, service_id, method_id);
    call.set_on_completed_locked(std::move(on_completed));
    call.set_on_error_locked(std::move(on_error));

    call.SendInitialClientRequest(request);
    client.CleanUpCalls();
    return call;
  }

#if PW_RPC_DYNAMIC_ALLOCATION
  template <typename CallType>
  static auto StartDynamic(Endpoint& client,
                           uint32_t channel_id,
                           uint32_t service_id,
                           uint32_t method_id,
                           Function<void(ConstByteSpan, Status)>&& on_completed,
                           Function<void(Status)>&& on_error,
                           ConstByteSpan request)
      PW_LOCKS_EXCLUDED(rpc_lock()) {
    rpc_lock().lock();
    auto call = PW_RPC_MAKE_UNIQUE_PTR(
        CallType, client.ClaimLocked(), channel_id, service_id, method_id);
    call->set_on_completed_locked(std::move(on_completed));
    call->set_on_error_locked(std::move(on_error));

    call->SendInitialClientRequest(request);
    client.CleanUpCalls();
    return call;
  }
#endif  // PW_RPC_DYNAMIC_ALLOCATION

  void HandleCompleted(ConstByteSpan response, Status status)
      PW_UNLOCK_FUNCTION(rpc_lock());

 protected:
  constexpr UnaryResponseClientCall() = default;

  UnaryResponseClientCall(LockedEndpoint& client,
                          uint32_t channel_id,
                          uint32_t service_id,
                          uint32_t method_id,
                          CallProperties properties)
      PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock())
      : ClientCall(client, channel_id, service_id, method_id, properties) {}

  UnaryResponseClientCall(UnaryResponseClientCall&& other) {
    *this = std::move(other);
  }

  UnaryResponseClientCall& operator=(UnaryResponseClientCall&& other)
      PW_LOCKS_EXCLUDED(rpc_lock()) {
    RpcLockGuard lock;
    MoveUnaryResponseClientCallFrom(other);
    return *this;
  }

  void MoveUnaryResponseClientCallFrom(UnaryResponseClientCall& other)
      PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock()) {
    MoveClientCallFrom(other);
    on_completed_ = std::move(other.on_completed_);
  }

  void set_on_completed(Function<void(ConstByteSpan, Status)>&& on_completed)
      PW_LOCKS_EXCLUDED(rpc_lock()) {
    RpcLockGuard lock;
    set_on_completed_locked(std::move(on_completed));
  }

  void set_on_completed_locked(
      Function<void(ConstByteSpan, Status)>&& on_completed)
      PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock()) {
    on_completed_ = std::move(on_completed);
  }

 private:
  using internal::ClientCall::set_on_next;  // Not used in unary response calls.

  Function<void(ConstByteSpan, Status)> on_completed_ PW_GUARDED_BY(rpc_lock());
};

// Stream response client calls only receive the status in their on_completed
// callback. Payloads are sent through the on_next callback.
class StreamResponseClientCall : public ClientCall {
 public:
  ~StreamResponseClientCall() { DestroyClientCall(); }

  // Start call for raw stream response RPCs.
  template <typename CallType>
  static CallType Start(Endpoint& client,
                        uint32_t channel_id,
                        uint32_t service_id,
                        uint32_t method_id,
                        Function<void(ConstByteSpan)>&& on_next,
                        Function<void(Status)>&& on_completed,
                        Function<void(Status)>&& on_error,
                        ConstByteSpan request) PW_LOCKS_EXCLUDED(rpc_lock()) {
    PW_ASSERT(PW_RPC_ALLOW_INVOCATIONS_ON_STACK);  // RPC client calls on the
                                                   // stack are not allowed. Use
                                                   // DynamicClient instead.

    rpc_lock().lock();
    CallType call(client.ClaimLocked(), channel_id, service_id, method_id);

    call.set_on_next_locked(std::move(on_next));
    call.set_on_completed_locked(std::move(on_completed));
    call.set_on_error_locked(std::move(on_error));

    call.SendInitialClientRequest(request);
    client.CleanUpCalls();
    return call;
  }

#if PW_RPC_DYNAMIC_ALLOCATION
  template <typename CallType>
  static auto StartDynamic(Endpoint& client,
                           uint32_t channel_id,
                           uint32_t service_id,
                           uint32_t method_id,
                           Function<void(ConstByteSpan)>&& on_next,
                           Function<void(Status)>&& on_completed,
                           Function<void(Status)>&& on_error,
                           ConstByteSpan request)
      PW_LOCKS_EXCLUDED(rpc_lock()) {
    rpc_lock().lock();
    auto call = PW_RPC_MAKE_UNIQUE_PTR(
        CallType, client.ClaimLocked(), channel_id, service_id, method_id);

    call->set_on_next_locked(std::move(on_next));
    call->set_on_completed_locked(std::move(on_completed));
    call->set_on_error_locked(std::move(on_error));

    call->SendInitialClientRequest(request);
    client.CleanUpCalls();
    return call;
  }
#endif  // PW_RPC_DYNAMIC_ALLOCATION

  void HandleCompleted(Status status) PW_UNLOCK_FUNCTION(rpc_lock());

 protected:
  constexpr StreamResponseClientCall() = default;

  StreamResponseClientCall(LockedEndpoint& client,
                           uint32_t channel_id,
                           uint32_t service_id,
                           uint32_t method_id,
                           CallProperties properties)
      PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock())
      : ClientCall(client, channel_id, service_id, method_id, properties) {}

  StreamResponseClientCall(StreamResponseClientCall&& other) {
    *this = std::move(other);
  }

  StreamResponseClientCall& operator=(StreamResponseClientCall&& other)
      PW_LOCKS_EXCLUDED(rpc_lock()) {
    RpcLockGuard lock;
    MoveStreamResponseClientCallFrom(other);
    return *this;
  }

  void MoveStreamResponseClientCallFrom(StreamResponseClientCall& other)
      PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock()) {
    MoveClientCallFrom(other);
    on_completed_ = std::move(other.on_completed_);
  }

  void set_on_completed(Function<void(Status)>&& on_completed)
      PW_LOCKS_EXCLUDED(rpc_lock()) {
    RpcLockGuard lock;
    set_on_completed_locked(std::move(on_completed));
  }

  void set_on_completed_locked(Function<void(Status)>&& on_completed)
      PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock()) {
    on_completed_ = std::move(on_completed);
  }

 private:
  Function<void(Status)> on_completed_ PW_GUARDED_BY(rpc_lock());
};

}  // namespace pw::rpc::internal
