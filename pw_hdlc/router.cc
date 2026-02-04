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

#include "pw_hdlc/router.h"

#include <algorithm>
#include <cinttypes>

#include "pw_hdlc/encoder.h"
#include "pw_log/log.h"
#include "pw_multibuf/multibuf.h"
#include "pw_multibuf/v1/stream.h"
#include "pw_result/result.h"
#include "pw_stream/null_stream.h"

// TODO: b/416564319 - There is an issue with the newlib configuration in one of
// Pigweed's stm32 builds which results in it not defining the PRIu64 macro.
// This workaround truncates printed addresses on those systems.
#if defined(__NEWLIB__) && !defined(PRIu64)
#define PW_HDLC_ADDR_FMT() "u"
#define PW_HDLC_ADDR_CAST(addr) static_cast<unsigned>(addr)
#else
#define PW_HDLC_ADDR_FMT() PRIu64
#define PW_HDLC_ADDR_CAST(addr) (addr)
#endif

namespace pw::hdlc {

using ::pw::async2::Context;
using ::pw::async2::Pending;
using ::pw::async2::Poll;
using ::pw::async2::PollOptional;
using ::pw::async2::PollResult;
using ::pw::async2::Ready;
using ::pw::channel::ByteReaderWriter;
using ::pw::channel::DatagramReaderWriter;
using ::pw::multibuf::MultiBuf;
using ::pw::multibuf::v1::Chunk;
using ::pw::stream::CountingNullStream;

namespace {

/// HDLC encodes the contents of ``payload`` to ``writer``.
Status WriteMultiBufUIFrame(uint64_t address,
                            const MultiBuf& payload,
                            stream::Writer& writer) {
  Encoder encoder(writer);
  if (Status status = encoder.StartUnnumberedFrame(address); !status.ok()) {
    return status;
  }
  for (const Chunk& chunk : payload.Chunks()) {
    if (Status status = encoder.WriteData(chunk); !status.ok()) {
      return status;
    }
  }
  return encoder.FinishFrame();
}

/// Calculates the size of ``payload`` once HDLC-encoded.
Result<size_t> CalculateSizeOnceEncoded(uint64_t address,
                                        const MultiBuf& payload) {
  CountingNullStream null_stream;
  Status encode_status = WriteMultiBufUIFrame(address, payload, null_stream);
  if (!encode_status.ok()) {
    return encode_status;
  }
  return null_stream.bytes_written();
}

/// Attempts to decode a frame from ``data``, advancing ``data`` forwards by
/// any bytes that are consumed.
std::optional<Frame> DecodeFrame(Decoder& decoder, MultiBuf& data) {
  size_t processed = 0;
  for (std::byte byte : data) {
    Result<Frame> frame_result = decoder.Process(byte);
    ++processed;
    if (frame_result.status().IsUnavailable()) {
      // No frame is yet available.
    } else if (frame_result.ok()) {
      data.DiscardPrefix(processed);
      return std::move(*frame_result);
    } else if (frame_result.status().IsDataLoss()) {
      PW_LOG_ERROR("Discarding invalid incoming HDLC frame.");
    } else if (frame_result.status().IsResourceExhausted()) {
      PW_LOG_ERROR("Discarding incoming HDLC frame: too large for buffer.");
    }
  }
  data.DiscardPrefix(processed);
  return std::nullopt;
}

}  // namespace

Status Router::AddChannel(DatagramReaderWriter& channel,
                          uint64_t receive_address,
                          uint64_t send_address) {
  for (const ChannelData& data : channel_datas_) {
    if ((data.channel == &channel) ||
        (data.receive_address == receive_address) ||
        (data.send_address == send_address)) {
      return Status::AlreadyExists();
    }
  }
  channel_datas_.emplace_back(channel, receive_address, send_address);
  return OkStatus();
}

Status Router::RemoveChannel(DatagramReaderWriter& channel,
                             uint64_t receive_address,
                             uint64_t send_address) {
  auto channel_entry = std::find_if(
      channel_datas_.begin(),
      channel_datas_.end(),
      [&channel, receive_address, send_address](const ChannelData& data) {
        return (data.channel == &channel) &&
               (data.receive_address == receive_address) &&
               (data.send_address == send_address);
      });
  if (channel_entry == channel_datas_.end()) {
    return Status::NotFound();
  }
  if (channel_datas_.size() == 1) {
    channel_datas_.clear();
  } else {
    // Put the ChannelData in the back of
    // the list and pop it out to avoid shifting
    // all elements.
    std::swap(*channel_entry, channel_datas_.back());
    channel_datas_.pop_back();
  }
  return OkStatus();
}

Router::ChannelData* Router::FindChannelForReceiveAddress(
    uint64_t receive_address) {
  for (auto& channel : channel_datas_) {
    if (channel.receive_address == receive_address) {
      return &channel;
    }
  }
  return nullptr;
}

Poll<> Router::PollDeliverIncomingFrame(Context& cx, const Frame& frame) {
  ConstByteSpan data = frame.data();
  uint64_t address = frame.address();
  ChannelData* channel = FindChannelForReceiveAddress(address);
  if (channel == nullptr) {
    PW_LOG_ERROR("Received incoming HDLC packet with address %"
                 PW_HDLC_ADDR_FMT() ", but no channel with that incoming "
                 "address is registered.",
                 PW_HDLC_ADDR_CAST(address));
    return Ready();
  }
  Poll<Status> ready_to_write = channel->channel->PendReadyToWrite(cx);
  if (ready_to_write.IsPending()) {
    return Pending();
  }
  if (!ready_to_write->ok()) {
    PW_LOG_ERROR("Channel at incoming HDLC address %" PW_HDLC_ADDR_FMT()
                 " became unwriteable. Status: %d",
                 PW_HDLC_ADDR_CAST(channel->receive_address),
                 ready_to_write->code());
    return Ready();
  }
  PollOptional<MultiBuf> buffer =
      channel->channel->PendAllocateWriteBuffer(cx, data.size());
  if (buffer.IsPending()) {
    return Pending();
  }
  if (!buffer->has_value()) {
    PW_LOG_ERROR(
        "Unable to allocate a buffer of size %zu destined for incoming "
        "HDLC address %" PW_HDLC_ADDR_FMT() ". Packet will be discarded.",
        data.size(),
        PW_HDLC_ADDR_CAST(frame.address()));
    return Ready();
  }
  std::copy(frame.data().begin(), frame.data().end(), (**buffer).begin());
  Status write_status = channel->channel->StageWrite(std::move(**buffer));
  if (!write_status.ok()) {
    PW_LOG_ERROR(
        "Failed to write a buffer of size %zu destined for incoming HDLC "
        "address %" PW_HDLC_ADDR_FMT() ". Status: %d",
        data.size(),
        PW_HDLC_ADDR_CAST(channel->receive_address),
        write_status.code());
  }
  return Ready();
}

void Router::DecodeAndWriteIncoming(Context& cx) {
  while (true) {
    if (decoded_frame_.has_value()) {
      if (PollDeliverIncomingFrame(cx, *decoded_frame_).IsPending()) {
        return;
      }
      // Zero out the frame delivery state.
      decoded_frame_ = std::nullopt;
    }

    while (incoming_data_.empty()) {
      PollResult<MultiBuf> incoming = io_channel_.PendRead(cx);
      if (incoming.IsPending()) {
        return;
      }
      if (!incoming->ok()) {
        if (incoming->status().IsFailedPrecondition()) {
          PW_LOG_WARN("HDLC io_channel has closed.");
        } else {
          PW_LOG_ERROR("Unable to read from HDLC io_channel. Status: %d",
                       incoming->status().code());
        }
        return;
      }
      incoming_data_ = std::move(**incoming);
    }

    decoded_frame_ = DecodeFrame(decoder_, incoming_data_);
  }
}

void Router::TryFillBufferToEncodeAndSend(Context& cx) {
  if (buffer_to_encode_and_send_.has_value()) {
    return;
  }
  for (size_t i = 0; i < channel_datas_.size(); ++i) {
    ChannelData& cd =
        channel_datas_[(next_first_read_index_ + i) % channel_datas_.size()];
    PollResult<MultiBuf> buf_result = cd.channel->PendRead(cx);
    if (buf_result.IsPending()) {
      continue;
    }
    if (!buf_result->ok()) {
      if (buf_result->status().IsUnimplemented()) {
        PW_LOG_ERROR("Channel registered for outgoing HDLC address %"
                     PW_HDLC_ADDR_FMT() " is not readable.",
                     PW_HDLC_ADDR_CAST(cd.send_address));
      }
      // We ignore FAILED_PRECONDITION (closed) because it will be handled
      // elsewhere. OUT_OF_RANGE just means we have finished writing. No
      // action is needed because the channel may still be receiving data.
      continue;
    }
    MultiBuf& buf = **buf_result;
    uint64_t target_address = cd.send_address;
    Result<size_t> encoded_size = CalculateSizeOnceEncoded(target_address, buf);
    if (!encoded_size.ok()) {
      PW_LOG_ERROR(
          "Unable to compute size of encoded packet for outgoing buffer of "
          "size %zu destined for outgoing HDLC address %" PW_HDLC_ADDR_FMT()
          ". Packet will be discarded.",
          buf.size(),
          PW_HDLC_ADDR_CAST(target_address));
      continue;
    }
    buffer_to_encode_and_send_ =
        OutgoingBuffer{/*buffer=*/std::move(buf),
                       /*hdlc_encoded_size=*/*encoded_size,
                       /*target_address=*/target_address};
    // We received data, so ensure that we start by reading from a different
    // index next time.
    next_first_read_index_ =
        (next_first_read_index_ + 1) % channel_datas_.size();
    return;
  }
}

void Router::WriteOutgoingMessages(Context& cx) {
  while (io_channel_.is_write_open()) {
    io_channel_.PendWrite(cx).IgnorePoll();
    if (io_channel_.PendReadyToWrite(cx).IsPending()) {
      return;
    }
    TryFillBufferToEncodeAndSend(cx);
    if (!buffer_to_encode_and_send_.has_value()) {
      // No channels have new data to send.
      return;
    }
    uint64_t target_address = buffer_to_encode_and_send_->target_address;
    size_t hdlc_encoded_size = buffer_to_encode_and_send_->hdlc_encoded_size;
    PollOptional<MultiBuf> maybe_write_buffer =
        io_channel_.PendAllocateWriteBuffer(cx, hdlc_encoded_size);
    if (maybe_write_buffer.IsPending()) {
      // Channel cannot write any further messages until we can allocate.
      return;
    }
    // We've gotten the allocation: discard the future.
    if (!maybe_write_buffer->has_value()) {
      // We can't allocate a write buffer large enough for our encoded frame.
      // Sadly, we have to throw the frame away.
      PW_LOG_ERROR(
          "Unable to allocate a buffer of size %zu destined for outgoing "
          "HDLC address %" PW_HDLC_ADDR_FMT() ". Packet will be discarded.",
          hdlc_encoded_size,
          PW_HDLC_ADDR_CAST(target_address));
      buffer_to_encode_and_send_ = std::nullopt;
      continue;
    }
    MultiBuf write_buffer = std::move(**maybe_write_buffer);
    Status encode_status =
        WriteMultiBufUIFrame(target_address,
                             buffer_to_encode_and_send_->buffer,
                             pw::multibuf::v1::Stream(write_buffer));
    buffer_to_encode_and_send_ = std::nullopt;
    if (!encode_status.ok()) {
      PW_LOG_ERROR(
          "Failed to encode a buffer destined for outgoing HDLC address "
          "%" PW_HDLC_ADDR_FMT() ". Status: %d",
          PW_HDLC_ADDR_CAST(target_address),
          encode_status.code());
      continue;
    }
    Status write_status = io_channel_.StageWrite(std::move(write_buffer));
    if (!write_status.ok()) {
      PW_LOG_ERROR(
          "Failed to write a buffer of size %zu destined for outgoing HDLC "
          "address %" PW_HDLC_ADDR_FMT() ". Status: %d",
          hdlc_encoded_size,
          PW_HDLC_ADDR_CAST(target_address),
          write_status.code());
    }
  }
}

Poll<> Router::Pend(Context& cx) {
  // We check for ability to read, but not write, because we may not always
  // attempt a write, which would cause us to miss that the channel has closed
  // for writes.
  //
  // Additionally, it is uncommon for a channel to remain readable but not
  // writeable: the reverse is more common (still readable while no longer
  // writeable).
  if (!io_channel_.is_read_open()) {
    return PendClose(cx);
  }
  DecodeAndWriteIncoming(cx);
  WriteOutgoingMessages(cx);
  RemoveClosedChannels();
  if (!io_channel_.is_read_open()) {
    return PendClose(cx);
  }
  return Pending();
}

Poll<> Router::PendClose(Context& cx) {
  for (ChannelData& cd : channel_datas_) {
    // We ignore the status value from close.
    // If one or more channels are unable to close, they will remain after
    // `RemoveClosedChannels` and `channel_datas_.size()` will be nonzero.
    cd.channel->PendClose(cx).IgnorePoll();
  }
  RemoveClosedChannels();
  if (io_channel_.PendClose(cx).IsPending()) {
    return Pending();
  }
  if (channel_datas_.empty()) {
    return Ready();
  } else {
    return Pending();
  }
}

void Router::RemoveClosedChannels() {
  auto first_to_remove = std::remove_if(
      channel_datas_.begin(), channel_datas_.end(), [](const ChannelData& cd) {
        return !cd.channel->is_read_or_write_open();
      });
  channel_datas_.erase(first_to_remove, channel_datas_.end());
}

}  // namespace pw::hdlc
