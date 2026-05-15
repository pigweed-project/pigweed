// Copyright 2022 The Pigweed Authors
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

// This file defines the ServerReaderWriter, ServerReader, ServerWriter, and
// UnaryResponder classes for the pw_protobuf RPC interface. These classes are
// used for bidirectional, client, and server streaming, and unary RPCs.
#pragma once

#include "pw_bytes/span.h"
#include "pw_function/function.h"
#include "pw_rpc/channel.h"
#include "pw_rpc/internal/lock.h"
#include "pw_rpc/internal/method_info.h"
#include "pw_rpc/internal/method_lookup.h"
#include "pw_rpc/internal/server_call.h"
#include "pw_rpc/method_type.h"
#include "pw_rpc/pwpb/internal/common.h"
#include "pw_rpc/server.h"

namespace pw::rpc {
namespace internal {

// Forward declarations for internal classes needed in friend statements.
namespace test {
template <typename, typename, uint32_t>
class InvocationContext;
}  // namespace test

class PwpbMethod;

// internal::PwpbServerCall extends internal::ServerCall by adding a method
// serializer/deserializer that is initialized based on the method context.
class PwpbServerCall : public ServerCall {
 public:
  // Allow construction using a call context and method type which creates
  // a working server call.
  PwpbServerCall(const LockedCallContext& context, MethodType type)
      PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock());

  ~PwpbServerCall() { DestroyServerCall(); }

  // Sends a unary response.
  // Returns the following Status codes:
  //
  //   OK - the response was successfully sent
  //   FAILED_PRECONDITION - the writer is closed
  //   INTERNAL - pw_rpc was unable to encode the pw_protobuf protobuf
  //   other errors - the ChannelOutput failed to send the packet; the error
  //       codes are determined by the ChannelOutput implementation
  //
  template <typename Response>
  Status SendUnaryResponse(const Response& response, Status status = OkStatus())
      PW_LOCKS_EXCLUDED(rpc_lock()) {
    RpcLockGuard lock;
    if (!active_locked()) {
      return Status::FailedPrecondition();
    }

    auto buffer = EncodeToPayloadBuffer(response, serde_->response());
    if (!buffer.ok()) {
      return CloseAndSendServerErrorLocked(Status::Internal());
    }

    return CloseAndSendResponseLocked(buffer.value().payload(), status);
  }

  template <typename Response>
  Status TrySendUnaryResponse(const Response& response,
                              Status status = OkStatus())
      PW_LOCKS_EXCLUDED(rpc_lock()) {
    RpcLockGuard lock;
    if (!active_locked()) {
      return Status::FailedPrecondition();
    }

    auto buffer = EncodeToPayloadBuffer(response, serde_->response());
    if (!buffer.ok()) {
      return TryCloseAndSendServerErrorLocked(Status::Internal());
    }

    return TryCloseAndSendResponseLocked(buffer.value().payload(), status);
  }

 protected:
  // Derived classes allow default construction so that users can declare a
  // variable into which to move server reader/writers from RPC calls.
  constexpr PwpbServerCall() : serde_(nullptr) {}

  // Give access to the serializer/deserializer object for converting requests
  // and responses between the wire format and pw_protobuf structs.
  const PwpbMethodSerde& serde() const PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock()) {
    return *serde_;
  }

  // Allow derived classes to be constructed moving another instance.
  PwpbServerCall(PwpbServerCall&& other) PW_LOCKS_EXCLUDED(rpc_lock()) {
    *this = std::move(other);
  }

  // Allow derived classes to use move assignment from another instance.
  PwpbServerCall& operator=(PwpbServerCall&& other)
      PW_LOCKS_EXCLUDED(rpc_lock()) {
    RpcLockGuard lock;
    MovePwpbServerCallFrom(other);
    return *this;
  }

  // Implement moving by copying the serde pointer.
  void MovePwpbServerCallFrom(PwpbServerCall& other)
      PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock()) {
    MoveServerCallFrom(other);
    serde_ = other.serde_;
  }

  // Sends a streamed response.
  // Returns the following Status codes:
  //
  //   OK - the response was successfully sent
  //   FAILED_PRECONDITION - the writer is closed
  //   INTERNAL - pw_rpc was unable to encode the pw_protobuf protobuf
  //   other errors - the ChannelOutput failed to send the packet; the error
  //       codes are determined by the ChannelOutput implementation
  //
  template <typename Response>
  Status SendStreamResponse(const Response& response)
      PW_LOCKS_EXCLUDED(rpc_lock()) {
    RpcLockGuard lock;
    return PwpbSendStream(*this, response, serde_);
  }

 private:
  const PwpbMethodSerde* serde_ PW_GUARDED_BY(rpc_lock());
};

// internal::BasePwpbServerReader extends internal::PwpbServerCall further by
// adding an on_next callback templated on the request type.
template <typename Request>
class BasePwpbServerReader : public PwpbServerCall {
 public:
  BasePwpbServerReader(const LockedCallContext& context, MethodType type)
      PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock())
      : PwpbServerCall(context, type) {}

  ~BasePwpbServerReader() { DestroyServerCall(); }

 protected:
  // Allow default construction so that users can declare a variable into
  // which to move server reader/writers from RPC calls.
  constexpr BasePwpbServerReader() = default;

  // Allow derived classes to be constructed moving another instance.
  BasePwpbServerReader(BasePwpbServerReader&& other)
      PW_LOCKS_EXCLUDED(rpc_lock()) {
    *this = std::move(other);
  }

  // Allow derived classes to use move assignment from another instance.
  BasePwpbServerReader& operator=(BasePwpbServerReader&& other)
      PW_LOCKS_EXCLUDED(rpc_lock()) {
    RpcLockGuard lock;
    MoveBasePwpbServerReaderFrom(other);
    return *this;
  }

  // Implement moving by copying the on_next function.
  void MoveBasePwpbServerReaderFrom(BasePwpbServerReader& other)
      PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock()) {
    MovePwpbServerCallFrom(other);
    set_pwpb_on_next_locked(std::move(other.pwpb_on_next_));
  }

  void set_on_next(Function<void(const Request& request)>&& on_next)
      PW_LOCKS_EXCLUDED(rpc_lock()) {
    RpcLockGuard lock;
    set_pwpb_on_next_locked(std::move(on_next));
  }

 private:
  void set_pwpb_on_next_locked(Function<void(const Request& request)>&& on_next)
      PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock()) {
    pwpb_on_next_ = std::move(on_next);

    Call::set_on_next_locked(
        [this](ConstByteSpan payload) PW_NO_LOCK_SAFETY_ANALYSIS {
          DecodeToStructAndInvokeOnNext(
              payload, serde().request(), pwpb_on_next_);
        });
  }

  Function<void(const Request&)> pwpb_on_next_ PW_GUARDED_BY(rpc_lock());
};

}  // namespace internal

// The PwpbServerReaderWriter is used to send and receive typed messages in a
// pw_protobuf bidirectional streaming RPC.
//
// These classes use private inheritance to hide the internal::Call API while
// allow direct use of its public and protected functions.
template <typename Request, typename Response>
class PwpbServerReaderWriter : private internal::BasePwpbServerReader<Request> {
 public:
  // Creates a PwpbServerReaderWriter that is ready to send responses for a
  // particular RPC. This can be used for testing or to send responses to an RPC
  // that has not been started by a client.
  template <auto kMethod, typename ServiceImpl>
  [[nodiscard]] static PwpbServerReaderWriter Open(Server& server,
                                                   uint32_t channel_id,
                                                   ServiceImpl& service)
      PW_LOCKS_EXCLUDED(internal::rpc_lock()) {
    using MethodInfo = internal::MethodInfo<kMethod>;
    static_assert(std::is_same_v<Request, typename MethodInfo::Request>,
                  "The request type of a PwpbServerReaderWriter must match "
                  "the method.");
    static_assert(std::is_same_v<Response, typename MethodInfo::Response>,
                  "The response type of a PwpbServerReaderWriter must match "
                  "the method.");
    return server.OpenCall<PwpbServerReaderWriter,
                           kMethod,
                           MethodType::kBidirectionalStreaming>(
        channel_id,
        service,
        internal::MethodLookup::GetPwpbMethod<ServiceImpl,
                                              MethodInfo::kMethodId>());
  }

  // Allow default construction so that users can declare a variable into
  // which to move server reader/writers from RPC calls.
  constexpr PwpbServerReaderWriter() = default;

  PwpbServerReaderWriter(PwpbServerReaderWriter&&) = default;
  PwpbServerReaderWriter& operator=(PwpbServerReaderWriter&&) = default;

  using internal::Call::active;
  using internal::Call::channel_id;
  using internal::Call::MaxWriteSizeBytes;

  // Functions for setting RPC event callbacks.
  using internal::Call::set_on_error;
  using internal::BasePwpbServerReader<Request>::set_on_next;
  using internal::ServerCall::set_on_completion_requested;
  using internal::ServerCall::set_on_completion_requested_if_enabled;

  // Writes a response. Returns the following Status codes:
  //
  //   OK - the response was successfully sent
  //   FAILED_PRECONDITION - the writer is closed
  //   INTERNAL - pw_rpc was unable to encode the pw_protobuf message
  //   other errors - the ChannelOutput failed to send the packet; the error
  //       codes are determined by the ChannelOutput implementation
  //
  Status Write(const Response& response) {
    return internal::PwpbServerCall::SendStreamResponse(response);
  }

  Status Finish(Status status = OkStatus()) {
    return internal::Call::CloseAndSendResponse(status);
  }

  Status TryFinish(Status status = OkStatus()) {
    return internal::Call::TryCloseAndSendResponse(status);
  }

 private:
  friend class internal::PwpbMethod;
  friend class Server;

  template <typename, typename, uint32_t>
  friend class internal::test::InvocationContext;

  PwpbServerReaderWriter(const internal::LockedCallContext& context,
                         MethodType type = MethodType::kBidirectionalStreaming)
      PW_EXCLUSIVE_LOCKS_REQUIRED(internal::rpc_lock())
      : internal::BasePwpbServerReader<Request>(context, type) {}
};

// The PwpbServerReader is used to receive typed messages and send a typed
// response in a pw_protobuf client streaming RPC.
//
// These classes use private inheritance to hide the internal::Call API while
// allow direct use of its public and protected functions.
template <typename Request, typename Response>
class PwpbServerReader : private internal::BasePwpbServerReader<Request> {
 public:
  // Creates a PwpbServerReader that is ready to send a response to a particular
  // RPC. This can be used for testing or to finish an RPC that has not been
  // started by the client.
  template <auto kMethod, typename ServiceImpl>
  [[nodiscard]] static PwpbServerReader Open(Server& server,
                                             uint32_t channel_id,
                                             ServiceImpl& service)
      PW_LOCKS_EXCLUDED(internal::rpc_lock()) {
    using MethodInfo = internal::MethodInfo<kMethod>;
    static_assert(std::is_same_v<Request, typename MethodInfo::Request>,
                  "The request type of a PwpbServerReader must match "
                  "the method.");
    static_assert(std::is_same_v<Response, typename MethodInfo::Response>,
                  "The response type of a PwpbServerReader must match "
                  "the method.");
    return server
        .OpenCall<PwpbServerReader, kMethod, MethodType::kClientStreaming>(
            channel_id,
            service,
            internal::MethodLookup::GetPwpbMethod<ServiceImpl,
                                                  MethodInfo::kMethodId>());
  }

  // Allow default construction so that users can declare a variable into
  // which to move server reader/writers from RPC calls.
  constexpr PwpbServerReader() = default;

  PwpbServerReader(PwpbServerReader&&) = default;
  PwpbServerReader& operator=(PwpbServerReader&&) = default;

  using internal::Call::active;
  using internal::Call::channel_id;
  using internal::Call::MaxWriteSizeBytes;

  // Functions for setting RPC event callbacks.
  using internal::Call::set_on_error;
  using internal::BasePwpbServerReader<Request>::set_on_next;
  using internal::ServerCall::set_on_completion_requested;
  using internal::ServerCall::set_on_completion_requested_if_enabled;

  // Sends the response. Returns the following Status codes:
  //
  //   OK - the response was successfully sent
  //   FAILED_PRECONDITION - the writer is closed
  //   INTERNAL - pw_rpc was unable to encode the pw_protobuf message
  //   other errors - the ChannelOutput failed to send the packet; the error
  //       codes are determined by the ChannelOutput implementation
  //
  Status Finish(const Response& response, Status status = OkStatus()) {
    return internal::PwpbServerCall::SendUnaryResponse(response, status);
  }

  Status TryFinish(const Response& response, Status status = OkStatus()) {
    return internal::PwpbServerCall::TrySendUnaryResponse(response, status);
  }

 private:
  friend class internal::PwpbMethod;
  friend class Server;

  template <typename, typename, uint32_t>
  friend class internal::test::InvocationContext;

  PwpbServerReader(const internal::LockedCallContext& context)
      PW_EXCLUSIVE_LOCKS_REQUIRED(internal::rpc_lock())
      : internal::BasePwpbServerReader<Request>(context,
                                                MethodType::kClientStreaming) {}
};

// The PwpbServerWriter is used to send typed responses in a pw_protobuf server
// streaming RPC.
//
// These classes use private inheritance to hide the internal::Call API while
// allow direct use of its public and protected functions.
template <typename Response>
class PwpbServerWriter : private internal::PwpbServerCall {
 public:
  // Creates a PwpbServerWriter that is ready to send responses for a particular
  // RPC. This can be used for testing or to send responses to an RPC that has
  // not been started by a client.
  template <auto kMethod, typename ServiceImpl>
  [[nodiscard]] static PwpbServerWriter Open(Server& server,
                                             uint32_t channel_id,
                                             ServiceImpl& service)
      PW_LOCKS_EXCLUDED(internal::rpc_lock()) {
    using MethodInfo = internal::MethodInfo<kMethod>;
    static_assert(std::is_same_v<Response, typename MethodInfo::Response>,
                  "The response type of a PwpbServerWriter must match "
                  "the method.");
    return server
        .OpenCall<PwpbServerWriter, kMethod, MethodType::kServerStreaming>(
            channel_id,
            service,
            internal::MethodLookup::GetPwpbMethod<ServiceImpl,
                                                  MethodInfo::kMethodId>());
  }

  // Allow default construction so that users can declare a variable into
  // which to move server reader/writers from RPC calls.
  constexpr PwpbServerWriter() = default;

  PwpbServerWriter(PwpbServerWriter&&) = default;
  PwpbServerWriter& operator=(PwpbServerWriter&&) = default;

  using internal::Call::active;
  using internal::Call::channel_id;
  using internal::Call::MaxWriteSizeBytes;

  // Functions for setting RPC event callbacks.
  using internal::Call::set_on_error;
  using internal::ServerCall::set_on_completion_requested;
  using internal::ServerCall::set_on_completion_requested_if_enabled;

  // Writes a response. Returns the following Status codes:
  //
  //   OK - the response was successfully sent
  //   FAILED_PRECONDITION - the writer is closed
  //   INTERNAL - pw_rpc was unable to encode the pw_protobuf message
  //   other errors - the ChannelOutput failed to send the packet; the error
  //       codes are determined by the ChannelOutput implementation
  //
  Status Write(const Response& response) {
    return internal::PwpbServerCall::SendStreamResponse(response);
  }

  Status Finish(Status status = OkStatus()) {
    return internal::Call::CloseAndSendResponse(status);
  }

  Status TryFinish(Status status = OkStatus()) {
    return internal::Call::TryCloseAndSendResponse(status);
  }

 private:
  friend class internal::PwpbMethod;
  friend class Server;

  template <typename, typename, uint32_t>
  friend class internal::test::InvocationContext;

  PwpbServerWriter(const internal::LockedCallContext& context)
      PW_EXCLUSIVE_LOCKS_REQUIRED(internal::rpc_lock())
      : internal::PwpbServerCall(context, MethodType::kServerStreaming) {}
};

// The PwpbUnaryResponder is used to send a typed response in a pw_protobuf
// unary RPC.
//
// These classes use private inheritance to hide the internal::Call API while
// allow direct use of its public and protected functions.
template <typename Response>
class PwpbUnaryResponder : private internal::PwpbServerCall {
 public:
  // Creates a PwpbUnaryResponder that is ready to send responses for a
  // particular RPC. This can be used for testing or to send responses to an
  // RPC that has not been started by a client.
  template <auto kMethod, typename ServiceImpl>
  [[nodiscard]] static PwpbUnaryResponder Open(Server& server,
                                               uint32_t channel_id,
                                               ServiceImpl& service)
      PW_LOCKS_EXCLUDED(internal::rpc_lock()) {
    using MethodInfo = internal::MethodInfo<kMethod>;
    static_assert(std::is_same_v<Response, typename MethodInfo::Response>,
                  "The response type of a PwpbUnaryResponder must match "
                  "the method.");
    return server
        .OpenCall<PwpbUnaryResponder<Response>, kMethod, MethodType::kUnary>(
            channel_id,
            service,
            internal::MethodLookup::GetPwpbMethod<ServiceImpl,
                                                  MethodInfo::kMethodId>());
  }

  // Allow default construction so that users can declare a variable into
  // which to move server reader/writers from RPC calls.
  constexpr PwpbUnaryResponder() = default;

  PwpbUnaryResponder(PwpbUnaryResponder&&) = default;
  PwpbUnaryResponder& operator=(PwpbUnaryResponder&&) = default;

  using internal::ServerCall::active;
  using internal::ServerCall::channel_id;
  using internal::ServerCall::MaxWriteSizeBytes;

  // Functions for setting RPC event callbacks.
  using internal::Call::set_on_error;

  // Sends the response. Returns the following Status codes:
  //
  //   OK - the response was successfully sent
  //   FAILED_PRECONDITION - the writer is closed
  //   INTERNAL - pw_rpc was unable to encode the pw_protobuf message
  //   other errors - the ChannelOutput failed to send the packet; the error
  //       codes are determined by the ChannelOutput implementation
  //
  Status Finish(const Response& response, Status status = OkStatus()) {
    return internal::PwpbServerCall::SendUnaryResponse(response, status);
  }

  Status TryFinish(const Response& response, Status status = OkStatus()) {
    return internal::PwpbServerCall::TrySendUnaryResponse(response, status);
  }

 private:
  friend class internal::PwpbMethod;
  friend class Server;

  template <typename, typename, uint32_t>
  friend class internal::test::InvocationContext;

  PwpbUnaryResponder(const internal::LockedCallContext& context)
      PW_EXCLUSIVE_LOCKS_REQUIRED(internal::rpc_lock())
      : internal::PwpbServerCall(context, MethodType::kUnary) {}
};

}  // namespace pw::rpc
