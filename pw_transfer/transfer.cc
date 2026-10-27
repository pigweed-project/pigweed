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

#include <mutex>
#define PW_LOG_MODULE_NAME "TRN"
#define PW_LOG_LEVEL PW_TRANSFER_CONFIG_LOG_LEVEL

#include "pw_assert/check.h"
#include "pw_log/log.h"
#include "pw_status/try.h"
#include "pw_transfer/internal/chunk.h"
#include "pw_transfer/internal/config.h"
#include "pw_transfer/transfer.h"

namespace pw::transfer {

void TransferService::HandleChunk(ConstByteSpan message,
                                  internal::TransferType type) {
  Result<internal::Chunk> chunk = internal::Chunk::Parse(message);
  if (!chunk.ok()) {
    PW_LOG_ERROR("Failed to decode transfer chunk: %d", chunk.status().code());
    return;
  }

  if (chunk->IsInitialChunk()) {
    uint32_t resource_id;
    uint32_t session_id;
    if (chunk->is_legacy()) {
      resource_id = chunk->session_id();
      session_id = chunk->session_id();
    } else if (chunk->desired_session_id().has_value() &&
               chunk->resource_id().has_value()) {
      resource_id = chunk->resource_id().value();
      session_id = chunk->desired_session_id().value();
    } else {
      // Non-legacy start chunks are required to specify desired_session_id and
      // resource_id.
      thread_.SendServerStatus(
          type,
          chunk->desired_session_id().value_or(chunk->session_id()),
          chunk->protocol_version(),
          Status::DataLoss());
      return;
    }

    uint32_t initial_offset;

    if (chunk->is_legacy()) {
      initial_offset = 0;
    } else {
      initial_offset = chunk->initial_offset();
    }

    thread_.StartServerTransfer(type,
                                chunk->protocol_version(),
                                session_id,
                                resource_id,
                                message,
                                max_parameters_,
                                chunk_timeout_,
                                max_retries_,
                                max_lifetime_retries_,
                                initial_offset);

    if (on_server_transfer_started_) {
      on_server_transfer_started_(session_id, resource_id);
    }
  } else {
    thread_.ProcessServerChunk(message);
  }
}

void TransferService::GetResourceStatus(pw::ConstByteSpan request,
                                        pw::rpc::RawUnaryResponder& responder) {
  uint32_t resource_id;
  Status status;
  std::array<std::byte, pwpb::ResourceStatus::kMaxEncodedSizeBytes> buffer = {};
  pwpb::ResourceStatus::MemoryEncoder encoder(buffer);

  protobuf::Decoder decoder(request);
  if (status = decoder.Next(); status.IsOutOfRange()) {
    resource_id = 0;
  } else if (!status.ok()) {
    responder.Finish({}, Status::DataLoss()).IgnoreError();
    return;
  } else if (static_cast<pwpb::ResourceStatusRequest::Fields>(
                 decoder.FieldNumber()) ==
             pwpb::ResourceStatusRequest::Fields::kResourceId) {
    if (status = decoder.ReadUint32(&resource_id); !status.ok()) {
      responder.Finish({}, Status::DataLoss()).IgnoreError();
      return;
    }
  } else {
    responder.Finish({}, Status::DataLoss()).IgnoreError();
    return;
  }

  encoder.WriteResourceId(resource_id).IgnoreError();

  {
    std::lock_guard lock(resource_responder_mutex_);
    if (TransferService::resource_responder_.active()) {
      PW_LOG_ERROR("Previous GetResourceStatus still being handled!");
      responder.Finish(ConstByteSpan(encoder), Status::Unavailable())
          .IgnoreError();
      return;
    }

    TransferService::resource_responder_ = std::move(responder);
  }

  thread_.EnqueueResourceEvent(
      resource_id,
      [this](Status call_status, const internal::ResourceStatus stats) {
        this->ResourceStatusCallback(call_status, stats);
      });
}

void TransferService::ResourceStatusCallback(
    Status status, const internal::ResourceStatus& stats) {
  std::lock_guard lock(resource_responder_mutex_);

  if (!resource_responder_.active()) {
    PW_LOG_ERROR("ResourceStatusCallback invoked without an active responder");
    return;
  }

  std::array<std::byte, pwpb::ResourceStatus::kMaxEncodedSizeBytes> buffer = {};
  pwpb::ResourceStatus::MemoryEncoder encoder(buffer);

  encoder.WriteResourceId(stats.resource_id).IgnoreError();
  encoder.WriteStatus(status.code()).IgnoreError();

  if (!status.ok()) {
    resource_responder_.Finish(ConstByteSpan(encoder), status).IgnoreError();
    return;
  }

  encoder.WriteReadableOffset(stats.readable_offset).IgnoreError();
  encoder.WriteReadChecksum(stats.read_checksum).IgnoreError();
  encoder.WriteWriteableOffset(stats.writeable_offset).IgnoreError();
  encoder.WriteWriteChecksum(stats.write_checksum).IgnoreError();

  if (!encoder.status().ok()) {
    resource_responder_.Finish(ConstByteSpan(encoder), encoder.status())
        .IgnoreError();
    return;
  }

  resource_responder_.Finish(ConstByteSpan(encoder), status).IgnoreError();
}

}  // namespace pw::transfer
