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

#include "pw_bytes/span.h"
#include "pw_chrono/system_clock.h"
#include "pw_function/function.h"
#include "pw_rpc/raw/server_reader_writer.h"
#include "pw_rpc/writer.h"
#include "pw_stream/stream.h"
#include "pw_transfer/internal/protocol.h"

namespace pw::transfer {

class Handler;

namespace internal {

enum class TransferType : bool { kTransmit, kReceive };

enum class TransferStream {
  kClientRead,
  kClientWrite,
  kServerRead,
  kServerWrite,
};

enum class SetStreamBehavior {
  kNewClient,    // Aborts all existing transfers on the stream.
  kReopen,       // Restarts initiating transfers and aborts others.
  kCloseStream,  // Clears the existing stream without creating a new one.
};

enum class IdentifierType {
  Session,
  Resource,
  Handle,
};

enum class EventType {
  // Begins a new transfer in an available context.
  kNewClientTransfer,
  kNewServerTransfer,

  // Processes an incoming chunk for a transfer.
  kClientChunk,
  kServerChunk,

  // Runs the timeout handler for a transfer.
  kClientTimeout,
  kServerTimeout,

  // Terminates an ongoing transfer with a specified status, optionally sending
  // a status chunk to the other end of the transfer.
  kClientEndTransfer,
  kServerEndTransfer,

  // Sends a status chunk to terminate a transfer. This does not call into the
  // transfer context's completion handler; it is for out-of-band termination.
  kSendStatusChunk,

  // Changes parameters of an ongoing client transfer.
  kUpdateClientTransfer,

  // Manages the list of transfer handlers for a transfer service.
  kAddTransferHandler,
  kRemoveTransferHandler,

  // Updates one of the transfer thread's RPC streams.
  kSetStream,

  // For testing only: aborts the transfer thread.
  kTerminate,

  // Gets the status of a resource, if there is a handler registered for it.
  kGetResourceStatus,
};

// Forward declarations required for events.
class TransferParameters;
class TransferThread;

struct NewTransferEvent {
  TransferType type;
  ProtocolVersion protocol_version;
  uint32_t session_id;
  uint32_t resource_id;
  uint32_t handle_id;
  rpc::Writer* rpc_writer;
  const TransferParameters* max_parameters;
  chrono::SystemClock::duration timeout;
  chrono::SystemClock::duration initial_timeout;
  uint8_t max_retries;
  uint32_t max_lifetime_retries;
  TransferThread* transfer_thread;

  union {
    stream::Stream* stream;  // In client-side transfers.
    Handler* handler;        // In server-side transfers.
  };

  const std::byte* raw_chunk_data;
  size_t raw_chunk_size;

  uint32_t initial_offset;
};

// A chunk received by a transfer client / server.
struct ChunkEvent {
  // Identifier for the transfer to which the chunk belongs.
  uint32_t context_identifier;

  // If true, only match the identifier against context resource IDs.
  bool match_resource_id;

  // The raw data of the chunk.
  const std::byte* data;
  size_t size;
};

struct EndTransferEvent {
  IdentifierType id_type;
  uint32_t id;
  Status::Code status;
  bool send_status_chunk;
};

struct SendStatusChunkEvent {
  uint32_t session_id;
  ProtocolVersion protocol_version;
  Status::Code status;
  TransferStream stream;
};

struct SetStreamEvent {
  TransferStream stream;
  SetStreamBehavior behavior;
};

struct UpdateTransferEvent {
  uint32_t handle_id;
  size_t transfer_size_bytes;
};

struct ResourceStatus {
  uint32_t resource_id;
  uint64_t readable_offset;
  uint64_t writeable_offset;
  uint64_t read_checksum;
  uint64_t write_checksum;
};

using ResourceStatusCallback = Callback<void(Status, const ResourceStatus&)>;

struct GetResourceStatusEvent {
  uint32_t resource_id;
};

struct Event {
  EventType type;

  union {
    NewTransferEvent new_transfer;
    ChunkEvent chunk;
    EndTransferEvent end_transfer;
    SendStatusChunkEvent send_status_chunk;
    UpdateTransferEvent update_transfer;
    Handler* add_transfer_handler;
    Handler* remove_transfer_handler;
    SetStreamEvent set_stream;
    GetResourceStatusEvent resource_status;
  };
};

}  // namespace internal
}  // namespace pw::transfer
