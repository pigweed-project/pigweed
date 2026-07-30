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

#define PW_LOG_MODULE_NAME "TRN"
#define PW_LOG_LEVEL PW_TRANSFER_CONFIG_LOG_LEVEL

#include "pw_transfer/transfer_thread.h"

#include "pw_assert/check.h"
#include "pw_log/log.h"
#include "pw_transfer/client.h"
#include "pw_transfer/internal/chunk.h"
#include "pw_transfer/internal/client_context.h"
#include "pw_transfer/internal/config.h"
#include "pw_transfer/internal/event.h"

PW_MODIFY_DIAGNOSTICS_PUSH();
PW_MODIFY_DIAGNOSTIC(ignored, "-Wmissing-field-initializers");

namespace pw::transfer::internal {

void TransferThread::Terminate() {
  next_event_ownership_.acquire();
  next_event_.type = EventType::kTerminate;
  event_notification_.release();
}

void TransferThread::SimulateTimeout(EventType type, uint32_t session_id) {
  next_event_ownership_.acquire();

  next_event_.type = type;
  next_event_.chunk = {};
  next_event_.chunk.context_identifier = session_id;

  event_notification_.release();

  WaitUntilEventIsProcessed();
}

void TransferThread::Run() {
  // Next event starts freed.
  next_event_ownership_.release();

  while (true) {
    std::optional<chrono::SystemClock::time_point> timeout =
        GetNextTransferTimeout();
    bool has_event = false;

    if (timeout.has_value()) {
      has_event = event_notification_.try_acquire_until(timeout.value());
    } else {
      event_notification_.acquire();
      has_event = true;
    }

    if (has_event) {
      HandleEvent(next_event_);

      // Sample event type before we release ownership of next_event_.
      bool is_terminating = next_event_.type == EventType::kTerminate;

      // Finished processing the event. Allow the next_event struct to be
      // overwritten.
      next_event_ownership_.release();

      if (is_terminating) {
        return;
      }
    }

    // Regardless of whether an event was received or not, check for any
    // transfers which have timed out and process them if so.
    for (Context& context : client_transfers_) {
      if (context.timed_out()) {
        context.HandleEvent({.type = EventType::kClientTimeout});
      }
    }
    for (Context& context : server_transfers_) {
      if (context.timed_out()) {
        context.HandleEvent({.type = EventType::kServerTimeout});
      }
    }
  }
}

std::optional<chrono::SystemClock::time_point>
TransferThread::GetNextTransferTimeout() const {
  std::optional<chrono::SystemClock::time_point> timeout = std::nullopt;

  for (Context& context : client_transfers_) {
    auto ctx_timeout = context.timeout();
    if (ctx_timeout.has_value() &&
        (!timeout.has_value() || ctx_timeout.value() < timeout.value())) {
      timeout = ctx_timeout.value();
    }
  }
  for (Context& context : server_transfers_) {
    auto ctx_timeout = context.timeout();
    if (ctx_timeout.has_value() &&
        (!timeout.has_value() || ctx_timeout.value() < timeout.value())) {
      timeout = ctx_timeout.value();
    }
  }

  return timeout;
}

void TransferThread::StartTransfer(
    TransferType type,
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
    uint32_t initial_offset) {
  if (!TryWaitForEventToProcess()) {
    return;
  }

  bool is_client_transfer = stream != nullptr;

  if (is_client_transfer) {
    if (version == ProtocolVersion::kLegacy) {
      session_id = resource_id;
    } else if (session_id == Context::kUnassignedSessionId) {
      session_id = AssignSessionId();
    }
  }

  next_event_.type = is_client_transfer ? EventType::kNewClientTransfer
                                        : EventType::kNewServerTransfer;

  PW_CHECK(raw_chunk.size() <= chunk_buffer_.size(),
           "Transfer received a larger chunk than it can handle.");

  if (!raw_chunk.empty()) {
    std::memcpy(chunk_buffer_.data(), raw_chunk.data(), raw_chunk.size());
  }

  next_event_.new_transfer = {
      .type = type,
      .protocol_version = version,
      .session_id = session_id,
      .resource_id = resource_id,
      .handle_id = handle_id,
      .max_parameters = &max_parameters,
      .timeout = timeout,
      .initial_timeout = initial_timeout,
      .max_retries = max_retries,
      .max_lifetime_retries = max_lifetime_retries,
      .transfer_thread = this,
      .raw_chunk_data = chunk_buffer_.data(),
      .raw_chunk_size = raw_chunk.size(),
      .initial_offset = initial_offset,
  };

  staged_on_completion_ = std::move(on_completion);

  // The transfer is initialized with either a stream (client-side) or a handler
  // (server-side). If no stream is provided, try to find a registered handler
  // with the specified ID.
  if (is_client_transfer) {
    next_event_.new_transfer.stream = stream;
    next_event_.new_transfer.rpc_writer =
        &(type == TransferType::kTransmit ? client_write_stream_.stream
                                          : client_read_stream_.stream)
             .as_writer();
  } else {
    auto handler = std::find_if(handlers_.begin(),
                                handlers_.end(),
                                [&](auto& h) { return h.id() == resource_id; });
    if (handler != handlers_.end()) {
      next_event_.new_transfer.handler = &*handler;
      next_event_.new_transfer.rpc_writer =
          &(type == TransferType::kTransmit ? server_read_stream_
                                            : server_write_stream_)
               .as_writer();
    } else {
      // No handler exists for the transfer: return a NOT_FOUND.
      next_event_.type = EventType::kSendStatusChunk;
      next_event_.send_status_chunk = {
          .session_id = session_id,
          .protocol_version = version,
          .status = Status::NotFound().code(),
          .stream = type == TransferType::kTransmit
                        ? TransferStream::kServerRead
                        : TransferStream::kServerWrite,
      };
    }
  }

  event_notification_.release();
}

void TransferThread::ProcessChunk(EventType type, ConstByteSpan chunk) {
  // If this assert is hit, there is a bug in the transfer implementation.
  // Contexts' max_chunk_size_bytes fields should be set based on the size of
  // chunk_buffer_.
  PW_CHECK(chunk.size() <= chunk_buffer_.size(),
           "Transfer received a larger chunk than it can handle.");

  Result<Chunk::Identifier> identifier = Chunk::ExtractIdentifier(chunk);
  if (!identifier.ok()) {
    PW_LOG_ERROR("Received a malformed chunk without a context identifier");
    return;
  }

  if (!TryWaitForEventToProcess()) {
    return;
  }

  std::memcpy(chunk_buffer_.data(), chunk.data(), chunk.size());

  next_event_.type = type;
  next_event_.chunk = {
      .context_identifier = identifier->value(),
      .match_resource_id = identifier->is_legacy(),
      .data = chunk_buffer_.data(),
      .size = chunk.size(),
  };

  event_notification_.release();
}

void TransferThread::SendStatus(TransferStream stream,
                                uint32_t session_id,
                                ProtocolVersion version,
                                Status status) {
  if (!TryWaitForEventToProcess()) {
    return;
  }

  next_event_.type = EventType::kSendStatusChunk;
  next_event_.send_status_chunk = {
      .session_id = session_id,
      .protocol_version = version,
      .status = status.code(),
      .stream = stream,
  };

  event_notification_.release();
}

void TransferThread::EndTransfer(EventType type,
                                 IdentifierType id_type,
                                 uint32_t id,
                                 Status status,
                                 bool send_status_chunk) {
  if (!TryWaitForEventToProcess()) {
    return;
  }

  next_event_.type = type;
  next_event_.end_transfer = {
      .id_type = id_type,
      .id = id,
      .status = status.code(),
      .send_status_chunk = send_status_chunk,
  };

  event_notification_.release();
}

void TransferThread::SetClientReadStream(
    rpc::RawClientReaderWriter& read_stream,
    Client* client,
    Function<void(ConstByteSpan)>&& on_next,
    internal::SetStreamBehavior behavior) {
  client_read_stream_.stream.set_on_next(nullptr);
  if (!TryWaitForEventToProcess()) {
    return;
  }
  staged_client_stream_.stream = std::move(read_stream);
  staged_client_stream_.client = client;
  staged_client_on_next_ = std::move(on_next);

  next_event_.type = EventType::kSetStream;
  next_event_.set_stream = {
      .stream = TransferStream::kClientRead,
      .behavior = behavior,
  };

  event_notification_.release();
  WaitUntilEventIsProcessed();
}

void TransferThread::CloseClientReadStream(Client* client) {
  if (!TryWaitForEventToProcess()) {
    return;
  }
  staged_client_stream_.client = client;

  next_event_.type = EventType::kSetStream;
  next_event_.set_stream = {
      .stream = TransferStream::kClientRead,
      .behavior = internal::SetStreamBehavior::kCloseStream,
  };

  event_notification_.release();
  WaitUntilEventIsProcessed();
}

void TransferThread::SetClientWriteStream(
    rpc::RawClientReaderWriter& write_stream,
    Client* client,
    Function<void(ConstByteSpan)>&& on_next,
    internal::SetStreamBehavior behavior) {
  client_write_stream_.stream.set_on_next(nullptr);
  if (!TryWaitForEventToProcess()) {
    return;
  }
  staged_client_stream_.stream = std::move(write_stream);
  staged_client_stream_.client = client;
  staged_client_on_next_ = std::move(on_next);

  next_event_.type = EventType::kSetStream;
  next_event_.set_stream = {
      .stream = TransferStream::kClientWrite,
      .behavior = behavior,
  };

  event_notification_.release();
  WaitUntilEventIsProcessed();
}

void TransferThread::CloseClientWriteStream(Client* client) {
  if (!TryWaitForEventToProcess()) {
    return;
  }
  staged_client_stream_.client = client;

  next_event_.type = EventType::kSetStream;
  next_event_.set_stream = {
      .stream = TransferStream::kClientWrite,
      .behavior = internal::SetStreamBehavior::kCloseStream,
  };

  event_notification_.release();
  WaitUntilEventIsProcessed();
}

void TransferThread::SetServerReadStream(
    rpc::RawServerReaderWriter& read_stream,
    Function<void(ConstByteSpan)>&& on_next) {
  server_read_stream_.set_on_next(nullptr);
  if (!TryWaitForEventToProcess()) {
    return;
  }
  staged_server_stream_ = std::move(read_stream);
  staged_server_on_next_ = std::move(on_next);

  next_event_.type = EventType::kSetStream;
  next_event_.set_stream = {
      .stream = TransferStream::kServerRead,
      .behavior = internal::SetStreamBehavior::kNewClient,
  };

  event_notification_.release();
  WaitUntilEventIsProcessed();
}

void TransferThread::SetServerWriteStream(
    rpc::RawServerReaderWriter& write_stream,
    Function<void(ConstByteSpan)>&& on_next) {
  server_write_stream_.set_on_next(nullptr);
  if (!TryWaitForEventToProcess()) {
    return;
  }
  staged_server_stream_ = std::move(write_stream);
  staged_server_on_next_ = std::move(on_next);

  next_event_.type = EventType::kSetStream;
  next_event_.set_stream = {
      .stream = TransferStream::kServerWrite,
      .behavior = internal::SetStreamBehavior::kNewClient,
  };

  event_notification_.release();
  WaitUntilEventIsProcessed();
}

void TransferThread::UpdateClientTransfer(uint32_t handle_id,
                                          size_t transfer_size_bytes) {
  if (!TryWaitForEventToProcess()) {
    return;
  }

  next_event_.type = EventType::kUpdateClientTransfer;
  next_event_.update_transfer.handle_id = handle_id;
  next_event_.update_transfer.transfer_size_bytes = transfer_size_bytes;

  event_notification_.release();
}

bool TransferThread::TransferHandlerEvent(EventType type, Handler& handler) {
  if (!TryWaitForEventToProcess()) {
    return false;
  }

  next_event_.type = type;
  if (type == EventType::kAddTransferHandler) {
    next_event_.add_transfer_handler = &handler;
  } else {
    next_event_.remove_transfer_handler = &handler;
  }

  event_notification_.release();
  return true;
}

void TransferThread::HandleEvent(const internal::Event& event) {
  switch (event.type) {
    case EventType::kTerminate:
      // Terminate server contexts.
      for (ServerContext& server_context : server_transfers_) {
        server_context.HandleEvent(Event{
            .type = EventType::kServerEndTransfer,
            .end_transfer =
                EndTransferEvent{
                    .id_type = IdentifierType::Session,
                    .id = server_context.session_id(),
                    .status = Status::Aborted().code(),
                    .send_status_chunk = false,
                },
        });
      }

      // Terminate client contexts.
      for (ClientContext& client_context : client_transfers_) {
        client_context.HandleEvent(Event{
            .type = EventType::kClientEndTransfer,
            .end_transfer =
                EndTransferEvent{
                    .id_type = IdentifierType::Session,
                    .id = client_context.session_id(),
                    .status = Status::Aborted().code(),
                    .send_status_chunk = false,
                },
        });
      }

      // Cancel/Finish streams.
      client_read_stream_.stream.Cancel().IgnoreError();
      client_write_stream_.stream.Cancel().IgnoreError();
      server_read_stream_.Finish(Status::Aborted()).IgnoreError();
      server_write_stream_.Finish(Status::Aborted()).IgnoreError();
      return;

    case EventType::kSendStatusChunk:
      SendStatusChunk(event.send_status_chunk);
      break;

    case EventType::kAddTransferHandler:
      handlers_.push_front(*event.add_transfer_handler);
      return;

    case EventType::kRemoveTransferHandler:
      for (ServerContext& server_context : server_transfers_) {
        if (server_context.handler() == event.remove_transfer_handler) {
          server_context.HandleEvent(Event{
              .type = EventType::kServerEndTransfer,
              .end_transfer =
                  EndTransferEvent{
                      .id_type = IdentifierType::Session,
                      .id = server_context.session_id(),
                      .status = Status::Aborted().code(),
                      .send_status_chunk = false,
                  },
          });
        }
      }
      handlers_.remove(*event.remove_transfer_handler);
      return;

    case EventType::kSetStream:
      HandleSetStreamEvent(event.set_stream.stream, event.set_stream.behavior);
      return;

    case EventType::kGetResourceStatus:
      GetResourceState(event.resource_status.resource_id);
      return;

    case EventType::kNewClientTransfer:
    case EventType::kNewServerTransfer:
    case EventType::kClientChunk:
    case EventType::kServerChunk:
    case EventType::kClientTimeout:
    case EventType::kServerTimeout:
    case EventType::kClientEndTransfer:
    case EventType::kServerEndTransfer:
    case EventType::kUpdateClientTransfer:
    default:
      // Other events are handled by individual transfer contexts.
      break;
  }

  Context* ctx = FindContextForEvent(event);
  if (ctx == nullptr) {
    // No context was found. For new transfer events, report a
    // RESOURCE_EXHAUSTED error with starting the transfer.
    if (event.type == EventType::kNewClientTransfer) {
      // On the client, invoke the completion callback directly.
      staged_on_completion_(Status::ResourceExhausted());
    } else if (event.type == EventType::kNewServerTransfer) {
      // On the server, send a status chunk back to the client.
      SendStatusChunk(
          {.session_id = event.new_transfer.session_id,
           .protocol_version = event.new_transfer.protocol_version,
           .status = Status::ResourceExhausted().code(),
           .stream = event.new_transfer.type == TransferType::kTransmit
                         ? TransferStream::kServerRead
                         : TransferStream::kServerWrite});
    }
    return;
  }

  if (event.type == EventType::kNewClientTransfer) {
    // TODO(frolv): This is terrible.
    ClientContext* cctx = static_cast<ClientContext*>(ctx);
    cctx->set_on_completion(std::move(staged_on_completion_));
    cctx->set_handle_id(event.new_transfer.handle_id);
  }

  if (event.type == EventType::kUpdateClientTransfer) {
    static_cast<ClientContext&>(*ctx).set_transfer_size_bytes(
        event.update_transfer.transfer_size_bytes);
    return;
  }

  ctx->HandleEvent(event);
}

Context* TransferThread::FindContextForEvent(
    const internal::Event& event) const {
  switch (event.type) {
    case EventType::kNewClientTransfer:
      return FindNewTransfer(client_transfers_, event.new_transfer.session_id);
    case EventType::kNewServerTransfer:
      return FindNewTransfer(server_transfers_, event.new_transfer.session_id);

    case EventType::kClientChunk:
      if (event.chunk.match_resource_id) {
        return FindActiveTransferByResourceId(client_transfers_,
                                              event.chunk.context_identifier);
      }
      return FindActiveTransferByLegacyId(client_transfers_,
                                          event.chunk.context_identifier);

    case EventType::kServerChunk:
      if (event.chunk.match_resource_id) {
        return FindActiveTransferByResourceId(server_transfers_,
                                              event.chunk.context_identifier);
      }
      return FindActiveTransferByLegacyId(server_transfers_,
                                          event.chunk.context_identifier);

    case EventType::kClientTimeout:  // Manually triggered client timeout
      return FindActiveTransferByLegacyId(client_transfers_,
                                          event.chunk.context_identifier);
    case EventType::kServerTimeout:  // Manually triggered server timeout
      return FindActiveTransferByLegacyId(server_transfers_,
                                          event.chunk.context_identifier);

    case EventType::kClientEndTransfer:
      if (event.end_transfer.id_type == IdentifierType::Handle) {
        return FindClientTransferByHandleId(event.end_transfer.id);
      }
      return FindActiveTransferByLegacyId(client_transfers_,
                                          event.end_transfer.id);
    case EventType::kServerEndTransfer:
      PW_DCHECK(event.end_transfer.id_type != IdentifierType::Handle);
      return FindActiveTransferByLegacyId(server_transfers_,
                                          event.end_transfer.id);

    case EventType::kUpdateClientTransfer:
      return FindClientTransferByHandleId(event.update_transfer.handle_id);

    case EventType::kSendStatusChunk:
    case EventType::kAddTransferHandler:
    case EventType::kRemoveTransferHandler:
    case EventType::kSetStream:
    case EventType::kTerminate:
    case EventType::kGetResourceStatus:
    default:
      return nullptr;
  }
}

void TransferThread::SendStatusChunk(
    const internal::SendStatusChunkEvent& event) {
  rpc::Writer& destination = stream_for(event.stream);

  Chunk chunk =
      Chunk::Final(event.protocol_version, event.session_id, event.status);

  Result<ConstByteSpan> result = chunk.Encode(chunk_buffer_);
  if (!result.ok()) {
    PW_LOG_ERROR("Failed to encode final chunk for transfer %u",
                 static_cast<unsigned>(event.session_id));
    return;
  }

  if (!destination.Write(result.value()).ok()) {
    PW_LOG_ERROR("Failed to send final chunk for transfer %u",
                 static_cast<unsigned>(event.session_id));
    return;
  }
}

// Should only be called with the `next_event_ownership_` lock held.
uint32_t TransferThread::AssignSessionId() {
  uint32_t session_id = next_session_id_++;
  if (session_id == 0) {
    session_id = next_session_id_++;
  }
  return session_id;
}

template <typename T>
void TerminateTransfers(span<T> contexts,
                        TransferType type,
                        EventType event_type,
                        Status status,
                        bool skip_initiating = false) {
  Event event;
  event.type = event_type;
  for (Context& context : contexts) {
    if (context.active() && context.type() == type) {
      if (skip_initiating && context.is_initiating()) {
        continue;
      }
      event.end_transfer = EndTransferEvent{
          .id_type = IdentifierType::Session,
          .id = context.session_id(),
          .status = status.code(),
          .send_status_chunk = false,
      };

      context.HandleEvent(event);
    }
  }
}

void TransferThread::CancelExistingStream(OwnedClientStream& stream,
                                          TransferType type) {
  if (stream.stream.active()) {
    if (staged_client_stream_.client != nullptr && stream.client != nullptr &&
        stream.client != staged_client_stream_.client) {
      stream.client->OnRpcError(Status::Cancelled(), type);
    }
    stream.stream.Cancel().IgnoreError();
  }
}

void TransferThread::HandleSetStreamEvent(
    TransferStream stream, internal::SetStreamBehavior behavior) {
  switch (stream) {
    case TransferStream::kClientRead: {
      if (behavior == internal::SetStreamBehavior::kCloseStream) {
        if (client_read_stream_.client == staged_client_stream_.client) {
          CancelExistingStream(client_read_stream_, TransferType::kReceive);
          TerminateTransfers(client_transfers_,
                             TransferType::kReceive,
                             EventType::kClientEndTransfer,
                             Status::Aborted());
          client_read_stream_.client = nullptr;
          client_read_stream_.stream = rpc::RawClientReaderWriter();
        }
        break;
      }

      CancelExistingStream(client_read_stream_, TransferType::kReceive);

      bool skip_initiating = behavior == internal::SetStreamBehavior::kReopen;
      TerminateTransfers(client_transfers_,
                         TransferType::kReceive,
                         EventType::kClientEndTransfer,
                         Status::Aborted(),
                         skip_initiating);

      client_read_stream_ = std::move(staged_client_stream_);
      client_read_stream_.stream.set_on_next(std::move(staged_client_on_next_));
      // on_error must be controlled by the client

      if (behavior == internal::SetStreamBehavior::kReopen) {
        // Restart initiating transfers
        for (Context& context : client_transfers_) {
          if (context.active() && context.type() == TransferType::kReceive &&
              context.is_initiating()) {
            context.InitiateTransferAsClient();
          }
        }
      }
      break;
    }

    case TransferStream::kClientWrite: {
      if (behavior == internal::SetStreamBehavior::kCloseStream) {
        if (client_write_stream_.client == staged_client_stream_.client) {
          CancelExistingStream(client_write_stream_, TransferType::kTransmit);
          TerminateTransfers(client_transfers_,
                             TransferType::kTransmit,
                             EventType::kClientEndTransfer,
                             Status::Aborted());
          client_write_stream_.client = nullptr;
          client_write_stream_.stream = rpc::RawClientReaderWriter();
        }
        break;
      }

      CancelExistingStream(client_write_stream_, TransferType::kTransmit);

      bool skip_initiating = behavior == internal::SetStreamBehavior::kReopen;
      TerminateTransfers(client_transfers_,
                         TransferType::kTransmit,
                         EventType::kClientEndTransfer,
                         Status::Aborted(),
                         skip_initiating);

      client_write_stream_ = std::move(staged_client_stream_);
      client_write_stream_.stream.set_on_next(
          std::move(staged_client_on_next_));
      // on_error must be controlled by the client

      if (behavior == internal::SetStreamBehavior::kReopen) {
        // Restart initiating transfers
        for (Context& context : client_transfers_) {
          if (context.active() && context.type() == TransferType::kTransmit &&
              context.is_initiating()) {
            context.InitiateTransferAsClient();
          }
        }
      }
      break;
    }

    case TransferStream::kServerRead: {
      TerminateTransfers(server_transfers_,
                         TransferType::kTransmit,
                         EventType::kServerEndTransfer,
                         Status::Aborted());
      server_read_stream_ = std::move(staged_server_stream_);
      server_read_stream_.set_on_next(std::move(staged_server_on_next_));
      server_read_stream_.set_on_error([](Status status) {
        PW_LOG_WARN("Server read stream closed unexpectedly: %s", status.str());
      });
      break;
    }

    case TransferStream::kServerWrite: {
      TerminateTransfers(server_transfers_,
                         TransferType::kReceive,
                         EventType::kServerEndTransfer,
                         Status::Aborted());
      server_write_stream_ = std::move(staged_server_stream_);
      server_write_stream_.set_on_next(std::move(staged_server_on_next_));
      server_write_stream_.set_on_error([](Status status) {
        PW_LOG_WARN("Server write stream closed unexpectedly: %s",
                    status.str());
      });
      break;
    }
  }
}

// Adds GetResourceStatusEvent to the queue. Will fail if there is already a
// GetResourceStatusEvent in process.
void TransferThread::EnqueueResourceEvent(uint32_t resource_id,
                                          ResourceStatusCallback&& callback) {
  if (!TryWaitForEventToProcess()) {
    internal::ResourceStatus stats;
    callback(Status::Unavailable(), stats);
    return;
  }

  next_event_.type = EventType::kGetResourceStatus;

  resource_status_callback_ = std::move(callback);

  next_event_.resource_status.resource_id = resource_id;

  event_notification_.release();
}

// Should only be called when we got a valid callback and RPC responder from
// GetResourceStatus transfer RPC.
void TransferThread::GetResourceState(uint32_t resource_id) {
  PW_ASSERT(resource_status_callback_ != nullptr);

  auto handler = std::find_if(handlers_.begin(), handlers_.end(), [&](auto& h) {
    return h.id() == resource_id;
  });
  internal::ResourceStatus stats;
  stats.resource_id = resource_id;

  if (handler != handlers_.end()) {
    Status status = handler->GetStatus(stats.readable_offset,
                                       stats.writeable_offset,
                                       stats.read_checksum,
                                       stats.write_checksum);

    resource_status_callback_(status, stats);
  } else {
    resource_status_callback_(Status::NotFound(), stats);
  }

  resource_status_callback_ = nullptr;
}

rpc::Writer& TransferThread::stream_for(TransferStream stream) {
  switch (stream) {
    case TransferStream::kClientRead:
      return client_read_stream_.stream.as_writer();
    case TransferStream::kClientWrite:
      return client_write_stream_.stream.as_writer();
    case TransferStream::kServerRead:
      return server_read_stream_.as_writer();
    case TransferStream::kServerWrite:
      return server_write_stream_.as_writer();
  }
  // An unknown TransferStream value was passed, which means this function
  // was passed an invalid enum value.
  PW_CRASH("Unsupported stream type");
}

}  // namespace pw::transfer::internal

PW_MODIFY_DIAGNOSTICS_POP();
