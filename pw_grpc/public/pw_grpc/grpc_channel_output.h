// Copyright 2024 The Pigweed Authors
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
#include <functional>
#include <mutex>
#include <optional>
#include <utility>

#include "pw_bytes/span.h"
#include "pw_grpc/connection.h"
#include "pw_rpc/channel.h"
#include "pw_rpc/internal/packet.h"
#include "pw_rpc/method_type.h"
#include "pw_sync/lock_annotations.h"
#include "pw_sync/mutex.h"
#include "pw_sync/thread_notification.h"

/// pw_rpc-over-gRPC library
namespace pw::grpc {

class GrpcChannelOutput : public rpc::ChannelOutput {
 public:
  GrpcChannelOutput() : rpc::ChannelOutput("grpc") {}
  // This interface provides an API for the channel output to notify and lookup
  // info in PwRpcHandler when handling output packets.
  class PwRpcHandlerInterface {
   public:
    virtual ~PwRpcHandlerInterface() = default;

    // Called when a stream is completed from the server. Called on the same
    // thread as Send is called on.
    virtual void OnClose(StreamId) = 0;

    // Called to determine the method type of an active call. Should return an
    // error if the call by that id is not active. Called on the same thread
    // Send is called on.
    virtual Result<rpc::MethodType> LookupMethodType(uint32_t call_id) = 0;
  };

  void set_callbacks(PwRpcHandlerInterface& callbacks)
      PW_LOCKS_EXCLUDED(mutex_) {
    std::lock_guard lock(mutex_);
    callbacks_ = callbacks;
  }

  // Attaches a connection. Callers must call clear_connection() before
  // replacing an existing connection so any in-flight SendPacket() calls on
  // the previous connection are drained first.
  void set_connection(Connection& conn) PW_LOCKS_EXCLUDED(mutex_) {
    std::lock_guard lock(mutex_);
    connection_ = conn;
  }

  // Detaches the connection. Closes it to wake any SendPacket() call that is
  // blocked on it, then waits for all SendPacket() calls using it to return.
  // After this returns, the connection can be destroyed.
  //
  // Must not be called from SendPacket() or from the callbacks it invokes, or
  // concurrently with set_connection() or another clear_connection().
  void clear_connection() PW_LOCKS_EXCLUDED(mutex_) {
    std::optional<std::reference_wrapper<Connection>> old_connection;
    bool wait_for_sends;
    {
      std::lock_guard lock(mutex_);
      old_connection = std::exchange(connection_, std::nullopt);
      wait_for_sends = active_sends_ > 0;
      waiting_for_sends_ = wait_for_sends;
    }
    if (old_connection.has_value()) {
      old_connection->get().CloseStreams();
    }
    if (wait_for_sends) {
      sends_done_.acquire();
      // The last SendPacket() calls release() with mutex_ held. Take mutex_
      // so that call has returned and unlocked mutex_ before this returns.
      std::lock_guard lock(mutex_);
    }
  }

  bool SupportsSendPacket() const override { return true; }

  Status SendPacket(const rpc::internal::Packet& packet) override
      PW_LOCKS_EXCLUDED(mutex_) {
    Connection* connection;
    PwRpcHandlerInterface* callbacks;
    {
      std::lock_guard lock(mutex_);
      if (!connection_.has_value() || !callbacks_.has_value()) {
        return Status::FailedPrecondition();
      }
      connection = &connection_->get();
      callbacks = &callbacks_->get();
      active_sends_++;
    }

    const Status status = DoSendPacket(*connection, *callbacks, packet);

    std::lock_guard lock(mutex_);
    active_sends_--;
    if (active_sends_ == 0 && waiting_for_sends_) {
      waiting_for_sends_ = false;
      sends_done_.release();
    }
    return status;
  }

  Status Send(ConstByteSpan data) override PW_LOCKS_EXCLUDED(mutex_) {
    PW_TRY_ASSIGN(rpc::internal::Packet packet,
                  rpc::internal::Packet::FromBuffer(data));
    return SendPacket(packet);
  }

 private:
  static Status DoSendPacket(Connection& connection,
                             PwRpcHandlerInterface& callbacks,
                             const rpc::internal::Packet& packet) {
    using pw::rpc::internal::pwpb::PacketType;

    switch (packet.type()) {
      case PacketType::kResponse: {
        PW_TRY_ASSIGN(auto method_type,
                      callbacks.LookupMethodType(packet.call_id()));
        // pw_rpc groups sending the final payload and return status into one
        // RESPONSE packet for both server streaming responses and unary
        // responses. In gRPC it's allowed to close the server streaming
        // response without any payload, in which case we shouldn't send a DATA
        // frame. Unary and client streaming methods always have a response
        // message, which must be sent even when it encodes to zero bytes.
        if (!packet.payload().empty() ||
            method_type == rpc::MethodType::kUnary ||
            method_type == rpc::MethodType::kClientStreaming) {
          if (Status status = connection.SendResponseMessage(packet.call_id(),
                                                             packet.payload());
              !status.ok()) {
            connection.SendResponseComplete(packet.call_id(), status)
                .IgnoreError();
            callbacks.OnClose(packet.call_id());
            return status;
          }
        }
        connection.SendResponseComplete(packet.call_id(), packet.status())
            .IgnoreError();
        callbacks.OnClose(packet.call_id());
        break;
      }
      case PacketType::kServerStream: {
        PW_TRY(
            connection.SendResponseMessage(packet.call_id(), packet.payload()));
        break;
      }
      case PacketType::kServerError: {
        connection.SendResponseComplete(packet.call_id(), packet.status())
            .IgnoreError();
        callbacks.OnClose(packet.call_id());
        break;
      }
      case PacketType::kClientError:
      case PacketType::kClientRequestCompletion:
      case PacketType::kClientStream:
      case PacketType::kRequest:
      default:
        return Status::FailedPrecondition();
    }

    return OkStatus();
  }

  sync::Mutex mutex_;
  std::optional<std::reference_wrapper<PwRpcHandlerInterface>> callbacks_
      PW_GUARDED_BY(mutex_);
  std::optional<std::reference_wrapper<Connection>> connection_
      PW_GUARDED_BY(mutex_);
  // Number of SendPacket() calls using connection_ outside of mutex_.
  size_t active_sends_ PW_GUARDED_BY(mutex_) = 0;
  // Set when clear_connection() is waiting for active_sends_ to reach 0.
  bool waiting_for_sends_ PW_GUARDED_BY(mutex_) = false;
  sync::ThreadNotification sends_done_;
};

}  // namespace pw::grpc
