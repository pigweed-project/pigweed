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

#include <cstdint>

#include "pw_assert/assert.h"
#include "pw_bytes/span.h"
#include "pw_chrono/system_clock.h"
#include "pw_function/function.h"
#include "pw_rpc/raw/client_reader_writer.h"
#include "pw_rpc/raw/server_reader_writer.h"
#include "pw_span/span.h"
#include "pw_sync/binary_semaphore.h"
#include "pw_sync/timed_thread_notification.h"
#include "pw_thread/thread_core.h"
#include "pw_transfer/handler.h"
#include "pw_transfer/internal/client_context.h"
#include "pw_transfer/internal/config.h"
#include "pw_transfer/internal/context.h"
#include "pw_transfer/internal/event.h"
#include "pw_transfer/internal/server_context.h"

namespace pw::transfer {

class Client;

namespace internal {

class TransferThread : public thread::ThreadCore {
 public:
  TransferThread(span<ClientContext> client_transfers,
                 span<ServerContext> server_transfers,
                 ByteSpan chunk_buffer,
                 ByteSpan encode_buffer)
      : client_transfers_(client_transfers),
        server_transfers_(server_transfers),
        next_session_id_(1),
        chunk_buffer_(chunk_buffer),
        encode_buffer_(encode_buffer) {}

  /// Set callback to be invoked when a server-side transfer completes or fails.
  ///
  /// @warning This callback is invoked from the transfer thread, so the
  /// callback:
  /// 1. Must not attempt to interact with the transfer thread, as doing so can
  ///    cause deadlock or internal state corruption.
  /// 2. Must return quickly to avoid stalling the transfer thread or the
  ///    rpc thread.
  void SetServerCompletionCallback(
      pw::Function<void(uint32_t, pw::Status)>&& cb) {
    on_server_completion_ = std::move(cb);
  }

  void NotifyServerCompletion(uint32_t session_id, Status status) {
    if (on_server_completion_) {
      on_server_completion_(session_id, status);
    }
  }

  void StartClientTransfer(TransferType type,
                           ProtocolVersion version,
                           uint32_t resource_id,
                           uint32_t handle_id,
                           stream::Stream* stream,
                           const TransferParameters& max_parameters,
                           Function<void(Status)>&& on_completion,
                           chrono::SystemClock::duration timeout,
                           chrono::SystemClock::duration initial_timeout,
                           uint8_t max_retries,
                           uint32_t max_lifetime_retries,
                           uint32_t initial_offset = 0) {
    StartTransfer(type,
                  version,
                  Context::kUnassignedSessionId,  // Assigned later.
                  resource_id,
                  handle_id,
                  /*raw_chunk=*/{},
                  stream,
                  max_parameters,
                  std::move(on_completion),
                  timeout,
                  initial_timeout,
                  max_retries,
                  max_lifetime_retries,
                  initial_offset);
  }

  void StartServerTransfer(TransferType type,
                           ProtocolVersion version,
                           uint32_t session_id,
                           uint32_t resource_id,
                           ConstByteSpan raw_chunk,
                           const TransferParameters& max_parameters,
                           chrono::SystemClock::duration timeout,
                           uint8_t max_retries,
                           uint32_t max_lifetime_retries,
                           uint32_t initial_offset = 0) {
    StartTransfer(type,
                  version,
                  session_id,
                  resource_id,
                  /*handle_id=*/0,
                  raw_chunk,
                  /*stream=*/nullptr,
                  max_parameters,
                  /*on_completion=*/nullptr,
                  timeout,
                  timeout,
                  max_retries,
                  max_lifetime_retries,
                  initial_offset);
  }

  void ProcessClientChunk(ConstByteSpan chunk) {
    ProcessChunk(EventType::kClientChunk, chunk);
  }

  void ProcessServerChunk(ConstByteSpan chunk) {
    ProcessChunk(EventType::kServerChunk, chunk);
  }

  void SendServerStatus(TransferType type,
                        uint32_t session_id,
                        ProtocolVersion version,
                        Status status) {
    SendStatus(type == TransferType::kTransmit ? TransferStream::kServerRead
                                               : TransferStream::kServerWrite,
               session_id,
               version,
               status);
  }

  void CancelClientTransfer(uint32_t handle_id) {
    EndTransfer(EventType::kClientEndTransfer,
                IdentifierType::Handle,
                handle_id,
                Status::Cancelled(),
                /*send_status_chunk=*/true);
  }

  void EndClientTransfer(uint32_t session_id,
                         Status status,
                         bool send_status_chunk = false) {
    EndTransfer(EventType::kClientEndTransfer,
                IdentifierType::Session,
                session_id,
                status,
                send_status_chunk);
  }

  void EndServerTransfer(uint32_t session_id,
                         Status status,
                         bool send_status_chunk = false) {
    EndTransfer(EventType::kServerEndTransfer,
                IdentifierType::Session,
                session_id,
                status,
                send_status_chunk);
  }

  /// Updates the transfer thread's client read stream.
  ///
  /// The provided stream should not have an on_next function set. Instead,
  /// on_next is passed separately to ensure that it is only set when the new
  /// stream becomes the transfer thread's primary stream.
  ///
  /// If the thread has an existing active client read stream, closes it and
  /// terminates any transfers running on it.
  void SetClientReadStream(rpc::RawClientReaderWriter& read_stream,
                           Client* client,
                           Function<void(ConstByteSpan)>&& on_next,
                           internal::SetStreamBehavior behavior);

  void CloseClientReadStream(Client* client);

  /// Updates the transfer thread's client write stream.
  ///
  /// The provided stream should not have an on_next function set. Instead,
  /// on_next is passed separately to ensure that it is only set when the new
  /// stream becomes the transfer thread's primary stream.
  ///
  /// If the thread has an existing active client write stream, closes it and
  /// terminates any transfers running on it.
  void SetClientWriteStream(rpc::RawClientReaderWriter& write_stream,
                            Client* client,
                            Function<void(ConstByteSpan)>&& on_next,
                            internal::SetStreamBehavior behavior);

  void CloseClientWriteStream(Client* client);

  /// Updates the transfer thread's server read stream.
  ///
  /// The provided stream should not have an on_next function set. Instead,
  /// on_next is passed separately to ensure that it is only set when the new
  /// stream becomes the transfer thread's primary stream.
  ///
  /// If the thread has an existing active server read stream, closes it and
  /// terminates any transfers running on it.
  void SetServerReadStream(rpc::RawServerReaderWriter& read_stream,
                           Function<void(ConstByteSpan)>&& on_next);

  /// Updates the transfer thread's server write stream.
  ///
  /// The provided stream should not have an on_next function set. Instead,
  /// on_next is passed separately to ensure that it is only set when the new
  /// stream becomes the transfer thread's primary stream.
  ///
  /// If the thread has an existing active server write stream, closes it and
  /// terminates any transfers running on it.
  void SetServerWriteStream(rpc::RawServerReaderWriter& write_stream,
                            Function<void(ConstByteSpan)>&& on_next);

  bool AddTransferHandler(Handler& handler) {
    if (!TransferHandlerEvent(EventType::kAddTransferHandler, handler)) {
      return false;
    }
    // Wait for handler to be fully registered before returning.
    WaitUntilEventIsProcessed();
    return true;
  }

  bool RemoveTransferHandler(Handler& handler) {
    if (!TransferHandlerEvent(EventType::kRemoveTransferHandler, handler)) {
      return false;
    }
    // Ensure this function blocks until the transfer handler is fully cleaned
    // up.
    WaitUntilEventIsProcessed();
    return true;
  }

  size_t max_chunk_size() const { return chunk_buffer_.size(); }

  // For testing only: terminates the transfer thread with a kTerminate event.
  void Terminate();

  // For testing only: blocks until the next event can be acquired, which means
  // a previously enqueued event has been processed.
  void WaitUntilEventIsProcessed() {
    next_event_ownership_.acquire();
    next_event_ownership_.release();
  }

  // For testing only: simulates a timeout event for a client transfer.
  void SimulateClientTimeout(uint32_t session_id) {
    SimulateTimeout(EventType::kClientTimeout, session_id);
  }

  // For testing only: simulates a timeout event for a server transfer.
  void SimulateServerTimeout(uint32_t session_id) {
    SimulateTimeout(EventType::kServerTimeout, session_id);
  }

  void EnqueueResourceEvent(uint32_t resource_id,
                            ResourceStatusCallback&& callback);

 private:
  friend class transfer::Client;
  friend class Context;

  Function<void(uint32_t, Status)> on_server_completion_;

  void UpdateClientTransfer(uint32_t handle_id, size_t transfer_size_bytes);

  // Finds an active server or client transfer, matching against its legacy ID.
  template <typename T>
  static Context* FindActiveTransferByLegacyId(const span<T>& transfers,
                                               uint32_t session_id) {
    auto transfer =
        std::find_if(transfers.begin(), transfers.end(), [session_id](auto& c) {
          return c.initialized() && c.session_id() == session_id;
        });
    return transfer != transfers.end() ? &*transfer : nullptr;
  }

  // Finds an active server or client transfer, matching against resource ID.
  template <typename T>
  static Context* FindActiveTransferByResourceId(const span<T>& transfers,
                                                 uint32_t resource_id) {
    auto transfer = std::find_if(
        transfers.begin(), transfers.end(), [resource_id](auto& c) {
          return c.initialized() && c.resource_id() == resource_id;
        });
    return transfer != transfers.end() ? &*transfer : nullptr;
  }

  Context* FindClientTransferByHandleId(uint32_t handle_id) const {
    auto transfer =
        std::find_if(client_transfers_.begin(),
                     client_transfers_.end(),
                     [handle_id](auto& c) {
                       return c.initialized() && c.handle_id() == handle_id;
                     });
    return transfer != client_transfers_.end() ? &*transfer : nullptr;
  }

  void SimulateTimeout(EventType type, uint32_t session_id);

  // Finds an new server or client transfer.
  template <typename T>
  static Context* FindNewTransfer(const span<T>& transfers,
                                  uint32_t session_id) {
    Context* new_transfer = nullptr;

    for (Context& context : transfers) {
      if (context.active()) {
        if (context.session_id() == session_id) {
          // Restart an already active transfer.
          return &context;
        }
      } else {
        // Store the inactive context as an option, but keep checking for the
        // restart case.
        new_transfer = &context;
      }
    }

    return new_transfer;
  }

  const ByteSpan& encode_buffer() const { return encode_buffer_; }

  void Run() final;

  void HandleTimeouts();

  rpc::Writer& stream_for(TransferStream stream);

  bool TryWaitForEventToProcess() {
    if constexpr (cfg::kWaitForEventProcessingIndefinitely) {
      next_event_ownership_.acquire();
      return true;
    }
    return next_event_ownership_.try_acquire_for(cfg::kEventProcessingTimeout);
  }

  // Returns the earliest timeout among all active transfers, if any.
  std::optional<chrono::SystemClock::time_point> GetNextTransferTimeout() const;

  uint32_t AssignSessionId();

  void StartTransfer(TransferType type,
                     ProtocolVersion version,
                     uint32_t session_id,
                     uint32_t resource_id,
                     uint32_t handle_id,
                     ConstByteSpan raw_chunk,
                     stream::Stream* stream,
                     const TransferParameters& max_parameters,
                     Function<void(Status)>&& on_completion,
                     chrono::SystemClock::duration timeout,
                     chrono::SystemClock::duration initial_timeout,
                     uint8_t max_retries,
                     uint32_t max_lifetime_retries,
                     uint32_t initial_offset);

  void ProcessChunk(EventType type, ConstByteSpan chunk);

  void SendStatus(TransferStream stream,
                  uint32_t session_id,
                  ProtocolVersion version,
                  Status status);

  void EndTransfer(EventType type,
                   IdentifierType id_type,
                   uint32_t session_id,
                   Status status,
                   bool send_status_chunk);

  void HandleSetStreamEvent(TransferStream stream,
                            internal::SetStreamBehavior behavior);

  bool TransferHandlerEvent(EventType type, Handler& handler);

  void HandleEvent(const Event& event);
  Context* FindContextForEvent(const Event& event) const;

  void SendStatusChunk(const SendStatusChunkEvent& event);

  void GetResourceState(uint32_t resource_id);

  sync::TimedThreadNotification event_notification_;
  sync::BinarySemaphore next_event_ownership_;

  Event next_event_;
  Function<void(Status)> staged_on_completion_;

  struct OwnedClientStream {
    rpc::RawClientReaderWriter stream;
    Client* client{nullptr};
  };

  void CancelExistingStream(OwnedClientStream& stream, TransferType type);

  OwnedClientStream client_read_stream_;
  OwnedClientStream client_write_stream_;
  OwnedClientStream staged_client_stream_;
  Function<void(ConstByteSpan)> staged_client_on_next_;

  rpc::RawServerReaderWriter server_read_stream_;
  rpc::RawServerReaderWriter server_write_stream_;
  rpc::RawServerReaderWriter staged_server_stream_;
  Function<void(ConstByteSpan)> staged_server_on_next_;

  span<ClientContext> client_transfers_;
  span<ServerContext> server_transfers_;

  // Identifier to use for the next started transfer, unique over the RPC
  // channel between the transfer client and server.
  //
  // TODO(frolv): If we ever support changing the RPC channel, this should be
  // reset to 1.
  uint32_t next_session_id_;

  // All registered transfer handlers.
  IntrusiveList<Handler> handlers_;

  // Buffer in which chunk data is staged for CHUNK events.
  ByteSpan chunk_buffer_;

  // Buffer into which responses are encoded. Only ever used from within the
  // transfer thread, so no locking is required.
  ByteSpan encode_buffer_;

  ResourceStatusCallback resource_status_callback_ = nullptr;
};

}  // namespace internal

/// @module{pw_transfer}

/// Non-templated transfer thread base type referenced by `Client` and
/// `TransferService`. Derives from `pw::thread::ThreadCore`.
using TransferThread = internal::TransferThread;

/// Dedicated background thread for processing client and server transfer
/// events.
///
/// Derives from `pw::thread::ThreadCore` so it can be passed directly when
/// creating a `pw::thread::Thread`. A single `Thread` instance can be shared
/// between a `Client` and a `TransferService`.
///
/// @tparam kMaxConcurrentClientTransfers Maximum number of concurrent client
/// transfers supported (may be `0` if only running a server).
/// @tparam kMaxConcurrentServerTransfers Maximum number of concurrent server
/// transfers supported (may be `0` if only running a client).
template <size_t kMaxConcurrentClientTransfers,
          size_t kMaxConcurrentServerTransfers>
class Thread final : public internal::TransferThread {
 public:
  /// Constructs a transfer thread using the provided staging and encoding
  /// buffers.
  ///
  /// @param[in] chunk_buffer Buffer used to stage incoming transfer chunks from
  /// the RPC system. Must fit the largest chunk supported by the system.
  /// @param[in] encode_buffer Buffer used to encode outgoing RPC packets.
  /// Typically sized to the transport layer's maximum transmission unit (MTU).
  Thread(ByteSpan chunk_buffer, ByteSpan encode_buffer)
      : internal::TransferThread(
            client_contexts_, server_contexts_, chunk_buffer, encode_buffer) {}

 private:
  std::array<internal::ClientContext, kMaxConcurrentClientTransfers>
      client_contexts_;
  std::array<internal::ServerContext, kMaxConcurrentServerTransfers>
      server_contexts_;
};

/// @endmodule

}  // namespace pw::transfer
