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

#include "pw_transfer/internal/context.h"

#include <chrono>
#include <limits>

#include "pw_assert/check.h"
#include "pw_chrono/system_clock.h"
#include "pw_log/log.h"
#include "pw_log/rate_limited.h"
#include "pw_preprocessor/compiler.h"
#include "pw_protobuf/serialized_size.h"
#include "pw_transfer/internal/config.h"
#include "pw_transfer/transfer.pwpb.h"
#include "pw_transfer/transfer_thread.h"
#include "pw_varint/varint.h"

namespace pw::transfer::internal {

void Context::NotifyServerCompletion() {
  if (thread_ != nullptr) {
    thread_->NotifyServerCompletion(session_id_, status_);
  }
}

void Context::HandleEvent(const Event& event) {
  switch (event.type) {
    case EventType::kNewClientTransfer:
    case EventType::kNewServerTransfer: {
      if (active()) {
        if (event.type == EventType::kNewServerTransfer &&
            event.new_transfer.session_id == session_id_ &&
            last_chunk_sent_ == Chunk::Type::kStartAck) {
          // The client is retrying its initial chunk as the response may not
          // have made it back. Re-send the handshake response without going
          // through handler reinitialization.
          RetryHandshake();
          return;
        }
        Abort(Status::Aborted());
      }

      Initialize(event.new_transfer);

      if (event.type == EventType::kNewClientTransfer) {
        InitiateTransferAsClient();
      } else {
        if (StartTransferAsServer(event.new_transfer)) {
          // TODO(frolv): This should probably be restructured.
          HandleChunkEvent({.context_identifier = event.new_transfer.session_id,
                            .match_resource_id = false,  // Unused.
                            .data = event.new_transfer.raw_chunk_data,
                            .size = event.new_transfer.raw_chunk_size});
        }
      }
      return;
    }

    case EventType::kClientChunk:
    case EventType::kServerChunk:
      PW_CHECK(initialized());
      HandleChunkEvent(event.chunk);
      return;

    case EventType::kClientTimeout:
    case EventType::kServerTimeout:
      HandleTimeout();
      return;

    case EventType::kClientEndTransfer:
    case EventType::kServerEndTransfer:
      if (active()) {
        if (event.end_transfer.send_status_chunk) {
          TerminateTransfer(event.end_transfer.status);
        } else {
          Abort(event.end_transfer.status);
        }
      }
      return;

    case EventType::kSendStatusChunk:
    case EventType::kAddTransferHandler:
    case EventType::kRemoveTransferHandler:
    case EventType::kSetStream:
    case EventType::kTerminate:
    case EventType::kUpdateClientTransfer:
    case EventType::kGetResourceStatus:
      // These events are intended for the transfer thread and should never be
      // forwarded through to a context.
      PW_CRASH("Transfer context received a transfer thread event");
  }
}

void Context::InitiateTransferAsClient() {
  PW_DCHECK(active());

  SetTimeout(initial_chunk_timeout_);

  PW_LOG_INFO("Starting transfer for resource %u",
              static_cast<unsigned>(resource_id_));

  // Receive transfers should prepare their initial parameters to be send in the
  // initial chunk.
  if (type() == TransferType::kReceive) {
    UpdateTransferParameters(TransmitAction::kBegin);
  }

  if (desired_protocol_version_ == ProtocolVersion::kLegacy) {
    // Legacy transfers go straight into the data transfer phase without a
    // handshake.
    if (type() == TransferType::kReceive) {
      SendTransferParameters(TransmitAction::kBegin);
    } else {
      SendInitialLegacyTransmitChunk();
    }

    LogTransferConfiguration();
    return;
  }

  // In newer protocol versions, begin the initial transfer handshake.
  Chunk start_chunk(desired_protocol_version_, Chunk::Type::kStart);
  start_chunk.set_desired_session_id(session_id_);
  start_chunk.set_resource_id(resource_id_);
  start_chunk.set_initial_offset(offset_);

  if (type() == TransferType::kReceive) {
    // Parameters should still be set on the initial chunk for backwards
    // compatibility if the server only supports the legacy protocol.
    SetTransferParameters(start_chunk);
  }

  EncodeAndSendChunk(start_chunk);
}

bool Context::StartTransferAsServer(const NewTransferEvent& new_transfer) {
  PW_LOG_INFO("Starting %s transfer %u for resource %u with offset %u",
              new_transfer.type == TransferType::kTransmit ? "read" : "write",
              static_cast<unsigned>(new_transfer.session_id),
              static_cast<unsigned>(new_transfer.resource_id),
              static_cast<unsigned>(new_transfer.initial_offset));
  LogTransferConfiguration();

  flags_ |= kFlagsContactMade;

  if (Status status = new_transfer.handler->Prepare(
          new_transfer.type, new_transfer.initial_offset);
      !status.ok()) {
    PW_LOG_WARN("Transfer handler %u prepare failed with status %u",
                static_cast<unsigned>(new_transfer.handler->id()),
                status.code());

    // As this failure occurs at the start of a transfer, no protocol version is
    // yet negotiated and one must be set to send a response. It is okay to use
    // the desired version here, as that comes from the client.
    configured_protocol_version_ = desired_protocol_version_;

    status = (status.IsPermissionDenied() || status.IsUnimplemented() ||
              status.IsResourceExhausted())
                 ? status
                 : Status::DataLoss();
    TerminateTransfer(status, /*with_resource_id=*/true);
    return false;
  }

  // Initialize doesn't set the handler since it's specific to server transfers.
  static_cast<ServerContext&>(*this).set_handler(*new_transfer.handler);

  // Server transfers use the stream provided by the handler rather than the
  // stream included in the NewTransferEvent.
  stream_ = &new_transfer.handler->stream();

  return true;
}

void Context::SendInitialLegacyTransmitChunk() {
  // A transmitter begins a transfer by sending the ID of the resource to which
  // it wishes to write.
  Chunk chunk(ProtocolVersion::kLegacy, Chunk::Type::kStart);
  chunk.set_session_id(resource_id_);

  EncodeAndSendChunk(chunk);
}

void Context::UpdateTransferParameters(TransmitAction action) {
  max_chunk_size_bytes_ = MaxWriteChunkSize(
      max_parameters_->max_chunk_size_bytes(), rpc_writer_->channel_id());
  uint32_t window_size = 0;

  if (max_chunk_size_bytes_ > max_parameters_->max_window_size_bytes()) {
    window_size =
        std::min(max_parameters_->max_window_size_bytes(),
                 static_cast<uint32_t>(writer().ConservativeWriteLimit()));
  } else {
    // Adjust the window size based on the latest event in the transfer.
    switch (action) {
      case TransmitAction::kBegin:
      case TransmitAction::kFirstParameters:
        // A transfer always begins with a window size of one chunk, set during
        // initialization. No further handling is required.
        break;

      case TransmitAction::kExtend:
        // Window was received successfully without packet loss and should grow.
        // Double the window size during slow start, or increase it by a single
        // chunk in congestion avoidance.
        if (transmit_phase_ == TransmitPhase::kCongestionAvoidance) {
          window_size_multiplier_ += 1;
        } else {
          window_size_multiplier_ *= 2;
        }

        // The window size can never exceed the user-specified maximum bytes. If
        // it does, reduce the multiplier to the largest size that fits.
        if (window_size_multiplier_ * max_chunk_size_bytes_ >
            max_parameters_->max_window_size_bytes()) {
          window_size_multiplier_ =
              max_parameters_->max_window_size_bytes() / max_chunk_size_bytes_;
        }
        break;

      case TransmitAction::kRetransmit:
        // A packet was lost: shrink the window size. Additionally, after the
        // first packet loss, transition from the slow start to the congestion
        // avoidance phase of the transfer.
        if (transmit_phase_ == TransmitPhase::kSlowStart) {
          transmit_phase_ = TransmitPhase::kCongestionAvoidance;
        }
        window_size_multiplier_ =
            std::max(window_size_multiplier_ / static_cast<uint32_t>(2),
                     static_cast<uint32_t>(1));
        break;
    }

    window_size =
        std::min({window_size_multiplier_ * max_chunk_size_bytes_,
                  max_parameters_->max_window_size_bytes(),
                  static_cast<uint32_t>(writer().ConservativeWriteLimit())});
  }

  window_size_ = window_size;
  window_end_offset_ = offset_ + window_size;
}

void Context::SetTransferParameters(Chunk& parameters) {
  parameters.set_window_end_offset(window_end_offset_)
      .set_max_chunk_size_bytes(max_chunk_size_bytes_)
      .set_min_delay_microseconds(kDefaultChunkDelayMicroseconds)
      .set_offset(offset_);
}

void Context::UpdateAndSendTransferParameters(TransmitAction action) {
  UpdateTransferParameters(action);

  return SendTransferParameters(action);
}

void Context::SendTransferParameters(TransmitAction action) {
  Chunk::Type type = Chunk::Type::kParametersRetransmit;

  switch (action) {
    case TransmitAction::kBegin:
      type = Chunk::Type::kStart;
      break;
    case TransmitAction::kFirstParameters:
    case TransmitAction::kRetransmit:
      type = Chunk::Type::kParametersRetransmit;
      break;
    case TransmitAction::kExtend:
      type = Chunk::Type::kParametersContinue;
      break;
  }

  Chunk parameters(configured_protocol_version_, type);
  parameters.set_session_id(session_id_);
  SetTransferParameters(parameters);

  PW_LOG_EVERY_N_DURATION(
      PW_LOG_LEVEL_INFO,
      log_rate_limit_,
      "Transfer rate: %u B/s",
      static_cast<unsigned>(transfer_rate_.GetRateBytesPerSecond()));

  PW_LOG_EVERY_N_DURATION(PW_LOG_LEVEL_INFO,
                          log_rate_limit_,
                          "Transfer %u sending transfer parameters: "
                          "offset=%u, window_end_offset=%u, max_chunk_size=%u",
                          static_cast<unsigned>(session_id_),
                          static_cast<unsigned>(offset_),
                          static_cast<unsigned>(window_end_offset_),
                          static_cast<unsigned>(max_chunk_size_bytes_));

  if (log_chunks_before_rate_limit_ > 0) {
    log_chunks_before_rate_limit_--;

    if (log_chunks_before_rate_limit_ == 0) {
      log_rate_limit_ = log_rate_limit_cfg_;
    }
  }

  EncodeAndSendChunk(parameters);
}

void Context::EncodeAndSendChunk(const Chunk& chunk) {
  last_chunk_sent_ = chunk.type();

#if PW_TRANSFER_CONFIG_DEBUG_CHUNKS
  if ((chunk.remaining_bytes().has_value() &&
       chunk.remaining_bytes().value() == 0) ||
      (chunk.type() != Chunk::Type::kData &&
       chunk.type() != Chunk::Type::kParametersContinue)) {
    chunk.LogChunk(false, pw::chrono::SystemClock::duration::zero());
  }
#endif

#if PW_TRANSFER_CONFIG_DEBUG_DATA_CHUNKS
  if (chunk.type() == Chunk::Type::kData ||
      chunk.type() == Chunk::Type::kParametersContinue) {
    chunk.LogChunk(false, log_rate_limit_);
  }
#endif

  Result<ConstByteSpan> data = chunk.Encode(thread_->encode_buffer());
  if (!data.ok()) {
    PW_LOG_ERROR("Failed to encode chunk for transfer %u: %d",
                 static_cast<unsigned>(chunk.session_id()),
                 data.status().code());
    if (active()) {
      TerminateTransfer(Status::Internal());
    }
    return;
  }

  if (const Status status = rpc_writer_->Write(*data); !status.ok()) {
    PW_LOG_ERROR("Failed to write chunk for transfer %u: %d",
                 static_cast<unsigned>(chunk.session_id()),
                 status.code());
    if (active()) {
      TerminateTransfer(Status::Internal());
    }
    return;
  }
}

void Context::Initialize(const NewTransferEvent& new_transfer) {
  PW_DCHECK(!active());

  PW_DCHECK_INT_NE(new_transfer.protocol_version,
                   ProtocolVersion::kUnknown,
                   "Cannot start a transfer with an unknown protocol");

  session_id_ = new_transfer.session_id;
  resource_id_ = new_transfer.resource_id;
  desired_protocol_version_ = new_transfer.protocol_version;
  configured_protocol_version_ = ProtocolVersion::kUnknown;

  flags_ = static_cast<uint8_t>(new_transfer.type);
  transfer_state_ = TransferState::kWaiting;
  retries_ = 0;
  max_retries_ = new_transfer.max_retries;
  lifetime_retries_ = 0;
  max_lifetime_retries_ = new_transfer.max_lifetime_retries;

  if (desired_protocol_version_ == ProtocolVersion::kLegacy) {
    // In a legacy transfer, there is no protocol negotiation stage.
    // Automatically configure the context to run the legacy protocol and
    // proceed to waiting for a chunk.
    configured_protocol_version_ = ProtocolVersion::kLegacy;
    transfer_state_ = TransferState::kWaiting;
  } else {
    transfer_state_ = TransferState::kInitiating;
  }

  rpc_writer_ = new_transfer.rpc_writer;
  stream_ = new_transfer.stream;

  offset_ = new_transfer.initial_offset;
  initial_offset_ = new_transfer.initial_offset;
  window_size_ = 0;
  window_end_offset_ = 0;
  max_chunk_size_bytes_ = new_transfer.max_parameters->max_chunk_size_bytes();

  window_size_multiplier_ = 1;
  transmit_phase_ = TransmitPhase::kSlowStart;

  max_parameters_ = new_transfer.max_parameters;
  thread_ = new_transfer.transfer_thread;

  last_chunk_sent_ = Chunk::Type::kStart;
  last_chunk_offset_ = 0;
  chunk_timeout_ = new_transfer.timeout;
  initial_chunk_timeout_ = new_transfer.initial_timeout;
  interchunk_delay_ = chrono::SystemClock::for_at_least(
      std::chrono::microseconds(kDefaultChunkDelayMicroseconds));
  next_timeout_ = kNoTimeout;
  log_chunks_before_rate_limit_ = log_chunks_before_rate_limit_cfg_;

  transfer_rate_.Reset();
}

void Context::HandleChunkEvent(const ChunkEvent& event) {
  Result<Chunk> maybe_chunk =
      Chunk::Parse(ConstByteSpan(event.data, event.size));
  if (!maybe_chunk.ok()) {
    return;
  }

  Chunk chunk = *maybe_chunk;

  // Received some data. Reset the retry counter.
  retries_ = 0;
  flags_ |= kFlagsContactMade;

#if PW_TRANSFER_CONFIG_DEBUG_CHUNKS
  if (chunk.type() != Chunk::Type::kData &&
      chunk.type() != Chunk::Type::kParametersContinue) {
    chunk.LogChunk(true, pw::chrono::SystemClock::duration::zero());
  }
#endif
#if PW_TRANSFER_CONFIG_DEBUG_DATA_CHUNKS
  if (chunk.type() == Chunk::Type::kData ||
      chunk.type() == Chunk::Type::kParametersContinue) {
    chunk.LogChunk(true, log_rate_limit_);
  }
#endif

  if (chunk.IsTerminatingChunk()) {
    if (active()) {
      HandleTermination(chunk.status().value());
    } else {
      PW_LOG_INFO("Got final status %d for completed transfer %d",
                  static_cast<int>(chunk.status().value().code()),
                  static_cast<int>(session_id_));
    }
    return;
  }

  if (type() == TransferType::kTransmit) {
    HandleTransmitChunk(chunk);
  } else {
    HandleReceiveChunk(chunk);
  }
}

void Context::PerformInitialHandshake(const Chunk& chunk) {
  switch (chunk.type()) {
    // Initial packet sent from a client to a server.
    case Chunk::Type::kStart: {
      UpdateLocalProtocolConfigurationFromPeer(chunk);

      if (type() == TransferType::kReceive) {
        // Update window end offset so it is valid.
        window_end_offset_ = offset_;
      }

      // This cast is safe as we know we're running in a transfer server.
      uint32_t resource_id = static_cast<ServerContext&>(*this).handler()->id();

      Chunk start_ack(configured_protocol_version_, Chunk::Type::kStartAck);
      start_ack.set_session_id(session_id_);
      start_ack.set_resource_id(resource_id);
      start_ack.set_initial_offset(offset_);

      EncodeAndSendChunk(start_ack);
      break;
    }

    // Response packet sent from a server to a client, confirming the protocol
    // version and session_id of the transfer.
    case Chunk::Type::kStartAck: {
      UpdateLocalProtocolConfigurationFromPeer(chunk);

      // This should confirm the offset we're starting at
      if (offset_ != chunk.initial_offset()) {
        TerminateTransfer(Status::Unimplemented());
        break;
      }

      Chunk start_ack_confirmation(configured_protocol_version_,
                                   Chunk::Type::kStartAckConfirmation);
      start_ack_confirmation.set_session_id(session_id_);

      if (type() == TransferType::kReceive) {
        // In a receive transfer, tag the initial transfer parameters onto the
        // confirmation chunk so that the server can immediately begin sending
        // data.
        UpdateTransferParameters(TransmitAction::kFirstParameters);
        SetTransferParameters(start_ack_confirmation);
      }

      set_transfer_state(TransferState::kWaiting);
      EncodeAndSendChunk(start_ack_confirmation);
      // we received a response, so we can re-up the timeout while waiting for
      // parameters.
      SetTimeout(chunk_timeout_);
      break;
    }

    // Confirmation sent by a client to a server of the configured transfer
    // version and session ID. Completes the handshake and begins the actual
    // data transfer.
    case Chunk::Type::kStartAckConfirmation: {
      set_transfer_state(TransferState::kWaiting);

      if (type() == TransferType::kTransmit) {
        HandleTransmitChunk(chunk);
      } else {
        HandleReceiveChunk(chunk);
      }
      break;
    }

    // If a non-handshake chunk is received during an INITIATING state, the
    // transfer peer is running a legacy protocol version, which does not
    // perform a handshake. End the handshake, revert to the legacy protocol,
    // and process the chunk appropriately.
    case Chunk::Type::kData:
    case Chunk::Type::kParametersRetransmit:
    case Chunk::Type::kParametersContinue:

      // Update the local session_id, which will map to the transfer_id of the
      // legacy chunk.
      session_id_ = chunk.session_id();

      configured_protocol_version_ = ProtocolVersion::kLegacy;
      // Cancel if we are not using at least version 2, and we tried to start a
      // non-zero offset transfer
      if (chunk.initial_offset() != 0) {
        PW_LOG_ERROR("Legacy transfer does not support offset transfers!");
        TerminateTransfer(Status::Internal());
        break;
      }

      set_transfer_state(TransferState::kWaiting);

      PW_LOG_DEBUG(
          "Transfer %u tried to start on protocol version %d, but peer only "
          "supports legacy",
          id_for_log(),
          static_cast<int>(desired_protocol_version_));

      if (type() == TransferType::kTransmit) {
        HandleTransmitChunk(chunk);
      } else {
        HandleReceiveChunk(chunk);
      }
      break;

    case Chunk::Type::kCompletion:
    case Chunk::Type::kCompletionAck:
      PW_CRASH(
          "Transfer completion packets should be processed by "
          "HandleChunkEvent()");
      break;
  }
}

void Context::UpdateLocalProtocolConfigurationFromPeer(const Chunk& chunk) {
  PW_LOG_DEBUG("Negotiating protocol version: ours=%d, theirs=%d",
               static_cast<int>(desired_protocol_version_),
               static_cast<int>(chunk.protocol_version()));

  configured_protocol_version_ =
      std::min(desired_protocol_version_, chunk.protocol_version());

  PW_LOG_INFO("Transfer %u: using protocol version %d",
              id_for_log(),
              static_cast<int>(configured_protocol_version_));
}

void Context::HandleTransmitChunk(const Chunk& chunk) {
  switch (transfer_state_) {
    case TransferState::kInactive:
    case TransferState::kRecovery:
      PW_CRASH("Never should handle chunk while inactive");

    case TransferState::kCompleted:
      // In a legacy transfer, if the transfer has already completed and another
      // chunk is received, tell the other end that the transfer is over.
      if (!chunk.IsInitialChunk() && status_.ok()) {
        status_ = Status::FailedPrecondition();
      }

      SendFinalStatusChunk();
      return;

    case TransferState::kInitiating:
      PerformInitialHandshake(chunk);
      return;

    case TransferState::kWaiting:
    case TransferState::kTransmitting:
      if (chunk.protocol_version() == configured_protocol_version_) {
        HandleTransferParametersUpdate(chunk);
      } else {
        PW_LOG_ERROR(
            "Transmit transfer %u was configured to use protocol version %d "
            "but received a chunk with version %d",
            id_for_log(),
            static_cast<int>(configured_protocol_version_),
            static_cast<int>(chunk.protocol_version()));
        TerminateTransfer(Status::Internal());
      }
      return;

    case TransferState::kTerminating:
      HandleTerminatingChunk(chunk);
      return;
  }
}

void Context::HandleTransferParametersUpdate(const Chunk& chunk) {
  bool retransmit = chunk.RequestsTransmissionFromOffset();

  if (retransmit) {
    // If the offsets don't match, attempt to seek on the reader. Not all
    // readers support seeking; abort with UNIMPLEMENTED if this handler
    // doesn't.
    if (offset_ != chunk.offset()) {
      if (Status seek_status = SeekReader(chunk.offset()); !seek_status.ok()) {
        PW_LOG_WARN("Transfer %u seek to %u failed with status %u",
                    static_cast<unsigned>(session_id_),
                    static_cast<unsigned>(chunk.offset()),
                    seek_status.code());

        // Remap status codes to return one of the following:
        //
        //   INTERNAL: invalid seek, never should happen
        //   DATA_LOSS: the reader is in a bad state
        //   UNIMPLEMENTED: seeking is not supported
        //
        if (seek_status.IsOutOfRange()) {
          seek_status = Status::Internal();
        } else if (!seek_status.IsUnimplemented()) {
          seek_status = Status::DataLoss();
        }

        TerminateTransfer(seek_status);
        return;
      }
    }

    offset_ = chunk.offset();
  } else if (chunk.window_end_offset() <= offset_) {
    PW_LOG_DEBUG("Transfer %u ignoring old rolling window chunk", id_for_log());
    SetTimeout(chunk_timeout_);
    return;
  }

  window_end_offset_ = chunk.window_end_offset();

  if (chunk.max_chunk_size_bytes().has_value()) {
    max_chunk_size_bytes_ = std::min(chunk.max_chunk_size_bytes().value(),
                                     max_parameters_->max_chunk_size_bytes());
  }

  if (chunk.min_delay_microseconds().has_value()) {
    interchunk_delay_ = chrono::SystemClock::for_at_least(
        std::chrono::microseconds(chunk.min_delay_microseconds().value()));
  }

  if (retransmit) {
    PW_LOG_INFO(
        "Transfer %u received parameters type=RETRANSMIT offset=%u "
        "window_end_offset=%u",
        static_cast<unsigned>(session_id_),
        static_cast<unsigned>(chunk.offset()),
        static_cast<unsigned>(window_end_offset_));
  } else {
    PW_LOG_EVERY_N_DURATION(
        PW_LOG_LEVEL_INFO,
        std::chrono::seconds(3),
        "Transfer %u received parameters type=CONTINUE offset=%u "
        "window_end_offset=%u",
        static_cast<unsigned>(session_id_),
        static_cast<unsigned>(chunk.offset()),
        static_cast<unsigned>(window_end_offset_));
  }

  // Parsed all of the parameters; start sending the window.
  set_transfer_state(TransferState::kTransmitting);

  TransmitNextChunk(retransmit);
}

void Context::TransmitNextChunk(bool retransmit_requested) {
  Chunk chunk(configured_protocol_version_, Chunk::Type::kData);
  chunk.set_session_id(session_id_);
  chunk.set_offset(offset_);

  // Reserve space for the data proto field overhead and use the remainder of
  // the buffer for the chunk data.
  size_t reserved_size =
      chunk.EncodedSize() + 1 /* data key */ + 5 /* data size */;

  size_t total_size = TransferSizeBytes();
  if (total_size != std::numeric_limits<size_t>::max()) {
    reserved_size += protobuf::SizeOfVarintField(
        pwpb::Chunk::Fields::kRemainingBytes, total_size);
  }

  ByteSpan buffer = thread_->encode_buffer();
  Result<ByteSpan> data;

  if (offset_ < total_size) {
    // Read the next chunk of data into the encode buffer.
    ByteSpan data_buffer = buffer.subspan(reserved_size);
    size_t max_bytes_to_send =
        std::min(window_end_offset_ - offset_, max_chunk_size_bytes_);

    if (max_bytes_to_send < data_buffer.size()) {
      data_buffer = data_buffer.first(max_bytes_to_send);
    }

    data = reader().Read(data_buffer);
  } else {
    // The user-specified resource size has been reached: respect it.
    data = Status::OutOfRange();
  }

  if (data.status().IsOutOfRange()) {
    // No more data to read.
    chunk.set_remaining_bytes(0);
    window_end_offset_ = offset_;

    PW_LOG_INFO("Transfer %u sending final chunk with remaining_bytes=0",
                static_cast<unsigned>(session_id_));
  } else if (data.ok()) {
    if (offset_ == window_end_offset_) {
      if (retransmit_requested) {
        PW_LOG_ERROR(
            "Transfer %u: received an empty retransmit request, but there is "
            "still data to send; aborting with RESOURCE_EXHAUSTED",
            id_for_log());
        TerminateTransfer(Status::ResourceExhausted());
      } else {
        PW_LOG_DEBUG(
            "Transfer %u: ignoring continuation packet for transfer window "
            "that has already been sent",
            id_for_log());
        SetTimeout(chunk_timeout_);
      }
      return;  // No data was requested, so there is nothing else to do.
    }

    PW_LOG_EVERY_N_DURATION(PW_LOG_LEVEL_DEBUG,
                            std::chrono::seconds(3),
                            "Transfer %u sending chunk offset=%u size=%u",
                            static_cast<unsigned>(session_id_),
                            static_cast<unsigned>(offset_),
                            static_cast<unsigned>(data.value().size()));

    chunk.set_payload(data.value());
    last_chunk_offset_ = offset_;
    offset_ += static_cast<uint32_t>(data.value().size());

    if (total_size != std::numeric_limits<size_t>::max()) {
      chunk.set_remaining_bytes(total_size - offset_);
    }
  } else {
    PW_LOG_ERROR("Transfer %u Read() failed with status %u",
                 static_cast<unsigned>(session_id_),
                 data.status().code());
    TerminateTransfer(Status::DataLoss());
    return;
  }

  Result<ConstByteSpan> encoded_chunk = chunk.Encode(buffer);
  if (!encoded_chunk.ok()) {
    PW_LOG_ERROR("Transfer %u failed to encode transmit chunk",
                 static_cast<unsigned>(session_id_));
    TerminateTransfer(Status::Internal());
    return;
  }

  if (const Status status = rpc_writer_->Write(*encoded_chunk); !status.ok()) {
    PW_LOG_ERROR("Transfer %u failed to send transmit chunk, status %u",
                 static_cast<unsigned>(session_id_),
                 status.code());
    TerminateTransfer(Status::DataLoss());
    return;
  }

  last_chunk_sent_ = chunk.type();
  flags_ |= kFlagsDataSent;

  if (offset_ == window_end_offset_ || offset_ == total_size) {
    // Sent all requested data. Must now wait for next parameters from the
    // receiver.
    set_transfer_state(TransferState::kWaiting);
    SetTimeout(chunk_timeout_);
  } else {
    // More data is to be sent. Set a timeout to send the next chunk following
    // the chunk delay.
    SetTimeout(chrono::SystemClock::for_at_least(interchunk_delay_));
  }
}

void Context::HandleReceiveChunk(const Chunk& chunk) {
  if (transfer_state_ == TransferState::kInitiating) {
    PerformInitialHandshake(chunk);
    return;
  }

  if (transfer_state_ == TransferState::kCompleted) {
    // If the transfer has already completed and another chunk is received,
    // re-send the final status chunk.
    SendFinalStatusChunk();
    return;
  }

  if (chunk.protocol_version() != configured_protocol_version_) {
    PW_LOG_ERROR(
        "Receive transfer %u was configured to use protocol version %d "
        "but received a chunk with version %d",
        id_for_log(),
        static_cast<int>(configured_protocol_version_),
        static_cast<int>(chunk.protocol_version()));
    TerminateTransfer(Status::Internal());
    return;
  }

  switch (transfer_state_) {
    case TransferState::kInactive:
    case TransferState::kTransmitting:
    case TransferState::kInitiating:
      PW_CRASH("HandleReceiveChunk() called in bad transfer state %d",
               static_cast<int>(transfer_state_));

    case TransferState::kCompleted:
      // Handled earlier.
      PW_UNREACHABLE;

    case TransferState::kRecovery:
      if (chunk.offset() != offset_) {
        if (last_chunk_offset_ == chunk.offset()) {
          PW_LOG_DEBUG(
              "Transfer %u received repeated offset %u; retry detected, "
              "resending transfer parameters",
              static_cast<unsigned>(session_id_),
              static_cast<unsigned>(chunk.offset()));

          log_chunks_before_rate_limit_ = log_chunks_before_rate_limit_cfg_;
          log_rate_limit_ = kNoRateLimit;

          UpdateAndSendTransferParameters(TransmitAction::kRetransmit);
          if (DataTransferComplete()) {
            return;
          }
          PW_LOG_DEBUG("Transfer %u waiting for offset %u, ignoring %u",
                       static_cast<unsigned>(session_id_),
                       static_cast<unsigned>(offset_),
                       static_cast<unsigned>(chunk.offset()));
        }

        last_chunk_offset_ = chunk.offset();
        SetTimeout(chunk_timeout_);
        return;
      }

      PW_LOG_DEBUG("Transfer %u received expected offset %u, resuming transfer",
                   static_cast<unsigned>(session_id_),
                   static_cast<unsigned>(offset_));
      set_transfer_state(TransferState::kWaiting);

      // The correct chunk was received; process it normally.
      [[fallthrough]];
    case TransferState::kWaiting:
      HandleReceivedData(chunk);
      return;

    case TransferState::kTerminating:
      HandleTerminatingChunk(chunk);
      return;
  }
}

void Context::HandleReceivedData(const Chunk& chunk) {
  if (chunk.offset() != offset_) {
    if (chunk.offset() + chunk.payload().size() <= offset_ &&
        chunk.type() != Chunk::Type::kStartAckConfirmation) {
      // If the chunk's data has already been received, don't go through a full
      // recovery cycle to avoid shrinking the window size and potentially
      // thrashing. The expected data may already be in-flight, so just allow
      // the transmitter to keep going with a CONTINUE parameters chunk.
      //
      // However, as a retried chunk indicates a potential issue with the
      // underlying connection, shrink the transfer window.
      //
      // Start ack confs do not come with an offset set, so it can get stuck
      // here if we are doing an offset transfer.
      PW_LOG_DEBUG("Transfer %u received duplicate chunk with offset %u",
                   id_for_log(),
                   static_cast<unsigned>(chunk.offset()));
      UpdateTransferParameters(TransmitAction::kRetransmit);
      SendTransferParameters(TransmitAction::kExtend);
    } else {
      // Bad offset; reset window size to send another parameters chunk.
      PW_LOG_WARN(
          "Transfer %u expected offset %u, received %u; entering recovery "
          "state",
          static_cast<unsigned>(session_id_),
          static_cast<unsigned>(offset_),
          static_cast<unsigned>(chunk.offset()));

      set_transfer_state(TransferState::kRecovery);
      UpdateAndSendTransferParameters(TransmitAction::kRetransmit);
    }

    SetTimeout(chunk_timeout_);
    return;
  }

  if (chunk.offset() + chunk.payload().size() > window_end_offset_) {
    PW_LOG_WARN(
        "Transfer %u received more data than what was requested (%u received "
        "for %u pending); attempting to recover.",
        id_for_log(),
        static_cast<unsigned>(chunk.payload().size()),
        static_cast<unsigned>(window_end_offset_ - offset_));

    // To prevent an improperly implemented client which doesn't respect
    // window_end_offset from entering an infinite retry loop, limit recovery
    // attempts to the lifetime retry count.
    lifetime_retries_++;
    if (lifetime_retries_ <= max_lifetime_retries_) {
      set_transfer_state(TransferState::kRecovery);
      SetTimeout(chunk_timeout_);

      UpdateAndSendTransferParameters(TransmitAction::kRetransmit);
    } else {
      TerminateTransfer(Status::Internal());
    }
    return;
  }

  // Update the last offset seen so that retries can be detected.
  last_chunk_offset_ = chunk.offset();

  // Write staged data from the buffer to the stream.
  if (chunk.has_payload()) {
    if (Status status = writer().Write(chunk.payload()); !status.ok()) {
      PW_LOG_ERROR(
          "Transfer %u write of %u B chunk failed with status %u; aborting "
          "with DATA_LOSS",
          static_cast<unsigned>(session_id_),
          static_cast<unsigned>(chunk.payload().size()),
          status.code());
      TerminateTransfer(Status::DataLoss());
      return;
    }

    transfer_rate_.Update(chunk.payload().size());
  }

  // Update the transfer state.
  offset_ += static_cast<uint32_t>(chunk.payload().size());

  // When the client sets remaining_bytes to 0, it indicates completion of the
  // transfer. Acknowledge the completion through a status chunk and clean up.
  if (chunk.IsFinalTransmitChunk()) {
    TerminateTransfer(OkStatus());
    return;
  }

  if (chunk.window_end_offset() != 0) {
    if (chunk.window_end_offset() < offset_) {
      PW_LOG_ERROR(
          "Transfer %u got invalid end offset of %u (current offset %u)",
          id_for_log(),
          static_cast<unsigned>(chunk.window_end_offset()),
          static_cast<unsigned>(offset_));
      TerminateTransfer(Status::Internal());
      return;
    }

    if (chunk.window_end_offset() > window_end_offset_) {
      // A transmitter should never send a larger end offset than what the
      // receiver has advertised. If this occurs, there is a bug in the
      // transmitter implementation. Terminate the transfer.
      PW_LOG_ERROR(
          "Transfer %u transmitter sent invalid end offset of %u, "
          "greater than receiver offset %u",
          id_for_log(),
          static_cast<unsigned>(chunk.window_end_offset()),
          static_cast<unsigned>(window_end_offset_));
      TerminateTransfer(Status::Internal());
      return;
    }

    window_end_offset_ = chunk.window_end_offset();
  }

  SetTimeout(chunk_timeout_);

  if (chunk.type() == Chunk::Type::kStartAckConfirmation) {
    // Send the first parameters in the receive transfer.
    UpdateAndSendTransferParameters(TransmitAction::kFirstParameters);
    return;
  }

  if (offset_ == window_end_offset_) {
    // Received all pending data. Advance the transfer parameters.
    UpdateAndSendTransferParameters(TransmitAction::kExtend);
    return;
  }

  // Once the transmitter has sent a sufficient amount of data, try to extend
  // the window to allow it to continue sending data without blocking.
  uint32_t remaining_window_size = window_end_offset_ - offset_;
  bool extend_window = remaining_window_size <=
                       window_size_ / max_parameters_->extend_window_divisor();

  if (extend_window) {
    UpdateAndSendTransferParameters(TransmitAction::kExtend);
  }
}

void Context::HandleTerminatingChunk(const Chunk& chunk) {
  switch (chunk.type()) {
    case Chunk::Type::kCompletion:
      PW_CRASH("Completion chunks should be processed by HandleChunkEvent()");

    case Chunk::Type::kCompletionAck:
      PW_LOG_INFO(
          "Transfer %u completed with status %u", id_for_log(), status_.code());
      SetInactive();
      break;

    case Chunk::Type::kData:
    case Chunk::Type::kStart:
    case Chunk::Type::kParametersRetransmit:
    case Chunk::Type::kParametersContinue:
    case Chunk::Type::kStartAck:
    case Chunk::Type::kStartAckConfirmation:
      // If a non-completion chunk is received in a TERMINATING state, re-send
      // the transfer's completion chunk to the peer.
      EncodeAndSendChunk(
          Chunk::Final(configured_protocol_version_, session_id_, status_));
      break;
  }
}

void Context::TerminateTransfer(Status status, bool with_resource_id) {
  if (transfer_state_ == TransferState::kTerminating ||
      transfer_state_ == TransferState::kCompleted) {
    // Transfer has already been terminated; no need to do it again.
    return;
  }

  Finish(status);

  PW_LOG_INFO("Transfer %u terminating with status: %u, offset: %u",
              static_cast<unsigned>(session_id_),
              status.code(),
              static_cast<unsigned>(offset_));

  if (ShouldSkipCompletionHandshake()) {
    set_transfer_state(TransferState::kCompleted);
  } else {
    set_transfer_state(TransferState::kTerminating);
    SetTimeout(chunk_timeout_);
  }

  // Don't send a final chunk if the other end of the transfer has not yet
  // made contact, as there is no one to notify.
  if ((flags_ & kFlagsContactMade) == kFlagsContactMade) {
    SendFinalStatusChunk(with_resource_id);
  }
}

void Context::HandleTermination(Status status) {
  Finish(status);

  PW_LOG_INFO("Transfer %u completed with status %u",
              static_cast<unsigned>(session_id_),
              status.code());

  if (ShouldSkipCompletionHandshake()) {
    set_transfer_state(TransferState::kCompleted);
  } else {
    EncodeAndSendChunk(
        Chunk(configured_protocol_version_, Chunk::Type::kCompletionAck)
            .set_session_id(session_id_));

    SetInactive();
  }
}

void Context::SendFinalStatusChunk(bool with_resource_id) {
  PW_DCHECK(transfer_state_ == TransferState::kCompleted ||
            transfer_state_ == TransferState::kTerminating);

  if (configured_protocol_version_ == ProtocolVersion::kUnknown) {
    // If the transfer is ended before contact is made with the peer,
    // the protocol version may not yet be configured. Use the desired
    // version for the status chunk.
    configured_protocol_version_ = desired_protocol_version_;
    PW_LOG_WARN(
        "Transfer %u ending before protocol version was confirmed; using "
        "version %u",
        id_for_log(),
        static_cast<unsigned>(desired_protocol_version_));
  }

  PW_LOG_INFO("Sending final chunk for transfer %u with status %u",
              static_cast<unsigned>(session_id_),
              status_.code());

  Chunk chunk =
      Chunk::Final(configured_protocol_version_, session_id_, status_);
  if (with_resource_id) {
    chunk.set_resource_id(resource_id_);
  }
  EncodeAndSendChunk(chunk);
}

void Context::Finish(Status status) {
  PW_DCHECK(active());

  status.Update(FinalCleanup(status));
  status_ = status;

  SetTimeout(kFinalChunkAckTimeout);
}

void Context::SetTimeout(chrono::SystemClock::duration timeout) {
  next_timeout_ = chrono::SystemClock::TimePointAfterAtLeast(timeout);
}

void Context::HandleTimeout() {
  ClearTimeout();

  switch (transfer_state_) {
    case TransferState::kCompleted:
      // A timeout occurring in a completed state indicates that the other side
      // never ACKed the final status packet. Reset the context to inactive.
      SetInactive();
      return;

    case TransferState::kTransmitting:
      // A timeout occurring in a TRANSMITTING state indicates that the transfer
      // has waited for its inter-chunk delay and should transmit its next
      // chunk.
      TransmitNextChunk(/*retransmit_requested=*/false);
      break;

    case TransferState::kInitiating:
    case TransferState::kWaiting:
    case TransferState::kRecovery:
    case TransferState::kTerminating:
      // A timeout occurring in a transfer or handshake state indicates that no
      // chunk has been received from the other side. The transfer should retry
      // its previous operation.
      //
      // The timeout is set immediately. Retry() will clear it if it fails.
      if (transfer_state_ == TransferState::kInitiating &&
          last_chunk_sent_ == Chunk::Type::kStart) {
        SetTimeout(initial_chunk_timeout_);
      } else {
        SetTimeout(chunk_timeout_);
      }
      Retry();
      break;

    case TransferState::kInactive:
      PW_LOG_ERROR("Timeout occurred in INACTIVE state");
      return;
  }
}

void Context::Retry() {
  if (retries_ == max_retries_ || lifetime_retries_ == max_lifetime_retries_) {
    PW_LOG_ERROR(
        "Transfer %u failed to receive a chunk after %u retries (lifetime %u).",
        id_for_log(),
        static_cast<unsigned>(retries_),
        static_cast<unsigned>(lifetime_retries_));
    PW_LOG_ERROR("Canceling transfer.");

    if (transfer_state_ == TransferState::kTerminating) {
      // Timeouts occurring in a TERMINATING state indicate that the completion
      // chunk was never ACKed. Simply clean up the transfer context.
      SetInactive();
    } else {
      TerminateTransfer(Status::DeadlineExceeded());
    }
    return;
  }

  ++retries_;
  ++lifetime_retries_;

  if (transfer_state_ == TransferState::kInitiating ||
      last_chunk_sent_ == Chunk::Type::kStartAckConfirmation) {
    RetryHandshake();
    return;
  }

  if (transfer_state_ == TransferState::kTerminating) {
    EncodeAndSendChunk(
        Chunk::Final(configured_protocol_version_, session_id_, status_));
    return;
  }

  if (type() == TransferType::kReceive) {
    // Resend the most recent transfer parameters.
    PW_LOG_DEBUG(
        "Receive transfer %u timed out waiting for chunk; resending parameters",
        static_cast<unsigned>(session_id_));

    UpdateAndSendTransferParameters(TransmitAction::kRetransmit);
    return;
  }

  // In a transmit, if a data chunk has not yet been sent, the initial transfer
  // parameters did not arrive from the receiver. Resend the initial chunk.
  if ((flags_ & kFlagsDataSent) != kFlagsDataSent) {
    PW_LOG_DEBUG(
        "Transmit transfer %u timed out waiting for initial parameters",
        static_cast<unsigned>(session_id_));
    SendInitialLegacyTransmitChunk();
    return;
  }

  // Otherwise, resend the most recent chunk. If the reader doesn't support
  // seeking, this isn't possible, so just terminate the transfer immediately.
  if (!SeekReader(last_chunk_offset_).ok()) {
    PW_LOG_ERROR("Transmit transfer %u timed out waiting for new parameters.",
                 id_for_log());
    PW_LOG_ERROR("Retrying requires a seekable reader. Alas, ours is not.");
    TerminateTransfer(Status::DeadlineExceeded());
    return;
  }

  // Rewind the transfer position and resend the chunk.
  offset_ = last_chunk_offset_;

  TransmitNextChunk(/*retransmit_requested=*/false);
}

void Context::RetryHandshake() {
  Chunk retry_chunk(configured_protocol_version_, last_chunk_sent_);

  switch (last_chunk_sent_) {
    case Chunk::Type::kStart:
      // No protocol version is yet configured at the time of sending the start
      // chunk, so we use the client's desired version instead.
      retry_chunk.set_protocol_version(desired_protocol_version_)
          .set_desired_session_id(session_id_)
          .set_resource_id(resource_id_)
          .set_initial_offset(offset_);
      if (type() == TransferType::kReceive) {
        SetTransferParameters(retry_chunk);
      }
      break;

    case Chunk::Type::kStartAck:
      retry_chunk.set_session_id(session_id_)
          .set_resource_id(static_cast<ServerContext&>(*this).handler()->id());
      break;

    case Chunk::Type::kStartAckConfirmation:
      retry_chunk.set_session_id(session_id_);
      if (type() == TransferType::kReceive) {
        SetTransferParameters(retry_chunk);
      }
      break;

    case Chunk::Type::kData:
    case Chunk::Type::kParametersRetransmit:
    case Chunk::Type::kParametersContinue:
    case Chunk::Type::kCompletion:
    case Chunk::Type::kCompletionAck:
      PW_CRASH("Should not RetryHandshake() when not in handshake phase");
  }

  EncodeAndSendChunk(retry_chunk);
}

uint32_t Context::MaxWriteChunkSize(uint32_t max_chunk_size_bytes,
                                    uint32_t channel_id) const {
  // Start with the user-provided maximum chunk size, which should be the usable
  // payload length on the RPC ingress path after any transport overhead.
  ptrdiff_t max_size = static_cast<ptrdiff_t>(max_chunk_size_bytes);

  // Subtract the RPC overhead (pw_rpc/internal/packet.proto).
  //
  //   type:       1 byte key, 1 byte value (CLIENT_STREAM)
  //   channel_id: 1 byte key, varint value (calculate from stream)
  //   service_id: 1 byte key, 4 byte value
  //   method_id:  1 byte key, 4 byte value
  //   payload:    1 byte key, varint length (remaining space)
  //   status:     0 bytes (not set in stream packets)
  //
  //   TOTAL: 14 bytes + encoded channel_id size + encoded payload length
  //
  max_size -= 14;
  max_size -= varint::EncodedSize(channel_id);
  max_size -= varint::EncodedSize(max_size);

  // TODO(frolv): Temporarily add 5 bytes for the new call_id change. The RPC
  // overhead calculation will be moved into an RPC helper to avoid having
  // pw_transfer depend on RPC internals.
  max_size -= 5;

  // Subtract the transfer service overhead for a client write chunk
  // (pw_transfer/transfer.proto).
  //
  //   session_id: 1 byte key, varint value (calculate)
  //   offset:     1 byte key, varint value (calculate)
  //   data:       1 byte key, varint length (remaining space)
  //
  //   TOTAL: 3 + encoded session_id + encoded offset + encoded data length
  //
  // Use a lower bound of a single chunk for the window end offset, as it will
  // always be at least in that range.
  size_t window_end_offset = std::max(window_end_offset_, max_chunk_size_bytes);
  max_size -= 3;
  max_size -= varint::EncodedSize(session_id_);
  max_size -= varint::EncodedSize(window_end_offset);
  max_size -= varint::EncodedSize(max_size);

  // A resulting value of zero (or less) renders write transfers unusable, as
  // there is no space to send any payload. This should be considered a
  // programmer error in the transfer service setup.
  PW_CHECK_INT_GT(
      max_size,
      0,
      "Transfer service maximum chunk size is too small to fit a payload. "
      "Increase max_chunk_size_bytes to support write transfers.");

  return static_cast<uint32_t>(max_size);
}

void Context::LogTransferConfiguration() {
  PW_LOG_DEBUG(
      "Local transfer timing configuration: "
      "chunk_timeout=%ums, max_retries=%u, interchunk_delay=%uus",
      static_cast<unsigned>(
          std::chrono::ceil<std::chrono::milliseconds>(chunk_timeout_).count()),
      static_cast<unsigned>(max_retries_),
      static_cast<unsigned>(
          std::chrono::ceil<std::chrono::microseconds>(interchunk_delay_)
              .count()));

  PW_LOG_DEBUG(
      "Local transfer windowing configuration: max_window_size_bytes=%u, "
      "extend_window_divisor=%u, max_chunk_size_bytes=%u",
      static_cast<unsigned>(max_parameters_->max_window_size_bytes()),
      static_cast<unsigned>(max_parameters_->extend_window_divisor()),
      static_cast<unsigned>(max_parameters_->max_chunk_size_bytes()));
}

}  // namespace pw::transfer::internal
