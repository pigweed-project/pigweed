// Copyright 2023 The Pigweed Authors
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

#include "pw_transfer/internal/chunk.h"

#include "pw_assert/check.h"
#include "pw_log/log.h"
#include "pw_log/rate_limited.h"
#include "pw_protobuf/decoder.h"
#include "pw_protobuf/serialized_size.h"
#include "pw_status/try.h"
#include "pw_transfer/internal/config.h"

namespace pw::transfer::internal {

namespace ProtoChunk = transfer::pwpb::Chunk;

Result<Chunk::Identifier> Chunk::ExtractIdentifier(ConstByteSpan message) {
  protobuf::Decoder decoder(message);

  uint32_t session_id = 0;
  uint32_t desired_session_id = 0;
  bool legacy = true;

  Status status;

  while ((status = decoder.Next()).ok()) {
    ProtoChunk::Fields field =
        static_cast<ProtoChunk::Fields>(decoder.FieldNumber());

    if (field == ProtoChunk::Fields::kTransferId) {
      // Interpret a legacy transfer_id field as a session ID if an explicit
      // session_id field has not already been seen.
      if (session_id == 0) {
        PW_TRY(decoder.ReadUint32(&session_id));
      }
    } else if (field == ProtoChunk::Fields::kSessionId) {
      // A session_id field always takes precedence over transfer_id.
      PW_TRY(decoder.ReadUint32(&session_id));
      legacy = false;
    } else if (field == ProtoChunk::Fields::kDesiredSessionId) {
      PW_TRY(decoder.ReadUint32(&desired_session_id));
    }
  }

  if (!status.IsOutOfRange()) {
    return Status::DataLoss();
  }

  if (desired_session_id != 0) {
    // Can't have both a desired and regular session_id.
    if (!legacy && session_id != 0) {
      return Status::DataLoss();
    }
    return Identifier::Desired(desired_session_id);
  }

  if (session_id != 0) {
    return legacy ? Identifier::Legacy(session_id)
                  : Identifier::Session(session_id);
  }

  return Status::DataLoss();
}

Result<Chunk> Chunk::Parse(ConstByteSpan message) {
  protobuf::Decoder decoder(message);
  Status status;
  uint32_t value;

  Chunk chunk;

  // Determine the protocol version of the chunk depending on field presence in
  // the serialized message.
  chunk.protocol_version_ = ProtocolVersion::kUnknown;

  // Some older versions of the protocol set the deprecated pending_bytes field
  // in their chunks. The newer transfer handling code does not process this
  // field, instead working only in terms of window_end_offset. If pending_bytes
  // is encountered in the serialized message, save its value, then calculate
  // window_end_offset from it once parsing is complete.
  uint32_t pending_bytes = 0;

  bool has_session_id = false;

  while ((status = decoder.Next()).ok()) {
    ProtoChunk::Fields field =
        static_cast<ProtoChunk::Fields>(decoder.FieldNumber());

    switch (field) {
      case ProtoChunk::Fields::kTransferId:
        // transfer_id is a legacy field. session_id will always take precedence
        // over it, so it should only be read if session_id has not yet been
        // encountered.
        if (chunk.session_id_ == 0) {
          PW_TRY(decoder.ReadUint32(&chunk.session_id_));
        }
        break;

      case ProtoChunk::Fields::kSessionId:
        // The existence of a session_id field indicates that a newer protocol
        // is running. Update the deduced protocol unless it was explicitly
        // specified.
        if (chunk.protocol_version_ == ProtocolVersion::kUnknown) {
          chunk.protocol_version_ = ProtocolVersion::kVersionTwo;
        }
        has_session_id = true;
        PW_TRY(decoder.ReadUint32(&chunk.session_id_));
        break;

      case ProtoChunk::Fields::kPendingBytes:
        PW_TRY(decoder.ReadUint32(&pending_bytes));
        break;

      case ProtoChunk::Fields::kMaxChunkSizeBytes:
        PW_TRY(decoder.ReadUint32(&value));
        chunk.set_max_chunk_size_bytes(value);
        break;

      case ProtoChunk::Fields::kMinDelayMicroseconds:
        PW_TRY(decoder.ReadUint32(&value));
        chunk.set_min_delay_microseconds(value);
        break;

      case ProtoChunk::Fields::kOffset:
        PW_TRY(decoder.ReadUint32(&chunk.offset_));
        break;

      case ProtoChunk::Fields::kData:
        PW_TRY(decoder.ReadBytes(&chunk.payload_));
        break;

      case ProtoChunk::Fields::kRemainingBytes: {
        uint64_t remaining_bytes;
        PW_TRY(decoder.ReadUint64(&remaining_bytes));
        chunk.set_remaining_bytes(remaining_bytes);
        break;
      }

      case ProtoChunk::Fields::kStatus:
        PW_TRY(decoder.ReadUint32(&value));
        chunk.set_status(static_cast<Status::Code>(value));
        break;

      case ProtoChunk::Fields::kWindowEndOffset:
        PW_TRY(decoder.ReadUint32(&chunk.window_end_offset_));
        break;

      case ProtoChunk::Fields::kType: {
        uint32_t type;
        PW_TRY(decoder.ReadUint32(&type));
        chunk.type_ = static_cast<Chunk::Type>(type);
        break;
      }

      case ProtoChunk::Fields::kResourceId:
        PW_TRY(decoder.ReadUint32(&value));
        chunk.set_resource_id(value);
        break;

      case ProtoChunk::Fields::kProtocolVersion:
        // The protocol_version field is added as part of the initial handshake
        // starting from version 2. If provided, it should override any deduced
        // protocol version.
        PW_TRY(decoder.ReadUint32(&value));
        if (!ValidProtocolVersion(value)) {
          return Status::DataLoss();
        }
        chunk.protocol_version_ = static_cast<ProtocolVersion>(value);
        break;

      case ProtoChunk::Fields::kDesiredSessionId:
        PW_TRY(decoder.ReadUint32(&value));
        chunk.desired_session_id_ = value;
        break;

      case ProtoChunk::Fields::kInitialOffset:
        PW_TRY(decoder.ReadUint32(&value));
        chunk.set_initial_offset(value);
        break;

        // Silently ignore any unrecognized fields.
    }
  }

  if (chunk.desired_session_id_.has_value() && has_session_id) {
    // Setting both session_id and desired_session_id is not permitted.
    return Status::DataLoss();
  }

  if (chunk.protocol_version_ == ProtocolVersion::kUnknown) {
    // If no fields in the chunk specified its protocol version, assume it is a
    // legacy chunk.
    chunk.protocol_version_ = ProtocolVersion::kLegacy;
  }

  if (pending_bytes != 0) {
    // Compute window_end_offset if it isn't explicitly provided (in older
    // protocol versions).
    chunk.set_window_end_offset(chunk.offset() + pending_bytes);
  }

  if (chunk.IsInitialChunk() && chunk.has_payload()) {
    return Status::DataLoss();
  }

  if (status.ok() || status.IsOutOfRange()) {
    return chunk;
  }

  return status;
}

Result<ConstByteSpan> Chunk::Encode(ByteSpan buffer) const {
  PW_CHECK(protocol_version_ != ProtocolVersion::kUnknown,
           "Cannot encode a transfer chunk with an unknown protocol version");

  ProtoChunk::MemoryEncoder encoder(buffer);

  // Write the payload first to avoid clobbering it if it shares the same buffer
  // as the encode buffer.
  if (has_payload()) {
    encoder.WriteData(payload_).IgnoreError();
  }

  if (protocol_version_ >= ProtocolVersion::kVersionTwo) {
    if (session_id_ != 0) {
      PW_CHECK(!desired_session_id_.has_value(),
               "A chunk cannot set both a desired and regular session ID");
      encoder.WriteSessionId(session_id_).IgnoreError();
    }

    if (desired_session_id_.has_value()) {
      encoder.WriteDesiredSessionId(desired_session_id_.value()).IgnoreError();
    }

    if (resource_id_.has_value()) {
      encoder.WriteResourceId(resource_id_.value()).IgnoreError();
    }
  }

  // During the initial handshake, the chunk's configured protocol version is
  // explicitly serialized to the wire.
  if (IsInitialHandshakeChunk()) {
    encoder.WriteProtocolVersion(static_cast<uint32_t>(protocol_version_))
        .IgnoreError();
  }

  if (type_.has_value()) {
    encoder.WriteType(static_cast<ProtoChunk::Type>(type_.value()))
        .IgnoreError();
  }

  if (window_end_offset_ != 0) {
    encoder.WriteWindowEndOffset(window_end_offset_).IgnoreError();
  }

  // Encode additional fields from the legacy protocol.
  if (ShouldEncodeLegacyFields()) {
    // The legacy protocol uses the transfer_id field instead of session_id or
    // resource_id.
    if (resource_id_.has_value()) {
      encoder.WriteTransferId(resource_id_.value()).IgnoreError();
    } else {
      encoder.WriteTransferId(session_id_).IgnoreError();
    }

    // In the legacy protocol, the pending_bytes field must be set alongside
    // window_end_offset, as some transfer implementations require it.
    if (window_end_offset_ != 0) {
      encoder.WritePendingBytes(window_end_offset_ - offset_).IgnoreError();
    }
  }

  if (max_chunk_size_bytes_.has_value()) {
    encoder.WriteMaxChunkSizeBytes(max_chunk_size_bytes_.value()).IgnoreError();
  }
  if (min_delay_microseconds_.has_value()) {
    encoder.WriteMinDelayMicroseconds(min_delay_microseconds_.value())
        .IgnoreError();
  }

  if (offset_ != 0) {
    encoder.WriteOffset(offset_).IgnoreError();
  }

  if (initial_offset_ != 0) {
    encoder.WriteInitialOffset(initial_offset_).IgnoreError();
  }

  if (remaining_bytes_.has_value()) {
    encoder.WriteRemainingBytes(remaining_bytes_.value()).IgnoreError();
  }

  if (status_.has_value()) {
    encoder.WriteStatus(status_.value().code()).IgnoreError();
  }

  PW_TRY(encoder.status());
  return ConstByteSpan(encoder);
}

size_t Chunk::EncodedSize() const {
  size_t size = 0;

  if (session_id_ != 0) {
    if (protocol_version_ >= ProtocolVersion::kVersionTwo) {
      size += protobuf::SizeOfVarintField(ProtoChunk::Fields::kSessionId,
                                          session_id_);
    }

    if (ShouldEncodeLegacyFields()) {
      if (resource_id_.has_value()) {
        size += protobuf::SizeOfVarintField(ProtoChunk::Fields::kTransferId,
                                            resource_id_.value());
      } else {
        size += protobuf::SizeOfVarintField(ProtoChunk::Fields::kTransferId,
                                            session_id_);
      }
    }
  }

  if (IsInitialHandshakeChunk()) {
    size +=
        protobuf::SizeOfVarintField(ProtoChunk::Fields::kProtocolVersion,
                                    static_cast<uint32_t>(protocol_version_));
  }

  if (protocol_version_ >= ProtocolVersion::kVersionTwo) {
    if (resource_id_.has_value()) {
      size += protobuf::SizeOfVarintField(ProtoChunk::Fields::kResourceId,
                                          resource_id_.value());
    }
    if (desired_session_id_.has_value()) {
      size += protobuf::SizeOfVarintField(ProtoChunk::Fields::kDesiredSessionId,
                                          desired_session_id_.value());
    }
  }

  if (offset_ != 0) {
    size += protobuf::SizeOfVarintField(ProtoChunk::Fields::kOffset, offset_);
  }

  if (window_end_offset_ != 0) {
    size += protobuf::SizeOfVarintField(ProtoChunk::Fields::kWindowEndOffset,
                                        window_end_offset_);

    if (ShouldEncodeLegacyFields()) {
      size += protobuf::SizeOfVarintField(ProtoChunk::Fields::kPendingBytes,
                                          window_end_offset_ - offset_);
    }
  }

  if (type_.has_value()) {
    size += protobuf::SizeOfVarintField(ProtoChunk::Fields::kType,
                                        static_cast<uint32_t>(type_.value()));
  }

  if (has_payload()) {
    size += protobuf::SizeOfDelimitedField(ProtoChunk::Fields::kData,
                                           payload_.size());
  }

  if (max_chunk_size_bytes_.has_value()) {
    size += protobuf::SizeOfVarintField(ProtoChunk::Fields::kMaxChunkSizeBytes,
                                        max_chunk_size_bytes_.value());
  }

  if (min_delay_microseconds_.has_value()) {
    size +=
        protobuf::SizeOfVarintField(ProtoChunk::Fields::kMinDelayMicroseconds,
                                    min_delay_microseconds_.value());
  }

  if (remaining_bytes_.has_value()) {
    size += protobuf::SizeOfVarintField(ProtoChunk::Fields::kRemainingBytes,
                                        remaining_bytes_.value());
  }

  if (status_.has_value()) {
    size += protobuf::SizeOfVarintField(ProtoChunk::Fields::kStatus,
                                        status_.value().code());
  }

  return size;
}

void Chunk::LogChunk(bool received,
                     pw::chrono::SystemClock::duration rate_limit) const {
  // Log in two different spots so the rate limiting applies separately to sent
  // and received
  if (received) {
    PW_LOG_EVERY_N_DURATION(
        PW_LOG_LEVEL_DEBUG,
        rate_limit,
        "Chunk received, type: %u, session id: %u, protocol version: %u,\n"
        "resource id: %d, desired session id: %d, offset: %u, size: %u,\n"
        "window end offset: %u, remaining bytes: %d, status: %d",
        type_.has_value() ? static_cast<unsigned>(type_.value()) : 0,
        static_cast<unsigned>(session_id_),
        static_cast<unsigned>(protocol_version_),
        resource_id_.has_value() ? static_cast<unsigned>(resource_id_.value())
                                 : -1,
        desired_session_id_.has_value()
            ? static_cast<int>(desired_session_id_.value())
            : -1,
        static_cast<unsigned>(offset_),
        has_payload() ? static_cast<unsigned>(payload_.size()) : 0,
        static_cast<unsigned>(window_end_offset_),
        remaining_bytes_.has_value()
            ? static_cast<unsigned>(remaining_bytes_.value())
            : -1,
        status_.has_value() ? static_cast<unsigned>(status_.value().code())
                            : -1);
  } else {
    PW_LOG_EVERY_N_DURATION(
        PW_LOG_LEVEL_DEBUG,
        rate_limit,
        "Chunk sent, type: %u, session id: %u, protocol version: %u,\n"
        "resource id: %d, desired session id: %d, offset: %u, size: %u,\n"
        "window end offset: %u, remaining bytes: %d, status: %d",
        type_.has_value() ? static_cast<unsigned>(type_.value()) : 0,
        static_cast<unsigned>(session_id_),
        static_cast<unsigned>(protocol_version_),
        resource_id_.has_value() ? static_cast<unsigned>(resource_id_.value())
                                 : -1,
        desired_session_id_.has_value()
            ? static_cast<int>(desired_session_id_.value())
            : -1,
        static_cast<unsigned>(offset_),
        has_payload() ? static_cast<unsigned>(payload_.size()) : 0,
        static_cast<unsigned>(window_end_offset_),
        remaining_bytes_.has_value()
            ? static_cast<unsigned>(remaining_bytes_.value())
            : -1,
        status_.has_value() ? static_cast<unsigned>(status_.value().code())
                            : -1);
  }
}

}  // namespace pw::transfer::internal
