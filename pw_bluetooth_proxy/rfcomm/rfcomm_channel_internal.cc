// Copyright 2026 The Pigweed Authors
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

#include "pw_bluetooth_proxy/rfcomm/internal/rfcomm_channel_internal.h"

#include <limits>
#include <mutex>

#include "pw_assert/check.h"
#include "pw_bluetooth/emboss_util.h"
#include "pw_bluetooth/rfcomm_frames.emb.h"
#include "pw_bluetooth_proxy/internal/l2cap_channel.h"
#include "pw_bluetooth_proxy/rfcomm/rfcomm_common.h"
#include "pw_log/log.h"
#include "pw_multibuf/multibuf.h"
#include "pw_span/cast.h"
#include "pw_sync/lock_annotations.h"
#include "pw_sync/mutex.h"

namespace pw::bluetooth::proxy::rfcomm::internal {

BorrowedRfcommChannel::BorrowedRfcommChannel(RfcommChannelInternal& channel)
    : channel_(channel) {
  channel_.Borrow();
}

BorrowedRfcommChannel::~BorrowedRfcommChannel() { channel_.Unborrow(); }

RfcommChannelInternal::~RfcommChannelInternal() {
  std::unique_lock lock(mutex_);
  while (num_borrows_ > 0) {
    // Release lock while waiting to avoid deadlock.
    lock.unlock();
    unborrowed_notification_.acquire();
    lock.lock();
  }
}

void RfcommChannelInternal::Borrow() {
  std::lock_guard lock(mutex_);
  num_borrows_++;
}

void RfcommChannelInternal::Unborrow() {
  std::lock_guard lock(mutex_);
  PW_CHECK(num_borrows_ > 0);
  num_borrows_--;
  if (num_borrows_ == 0) {
    unborrowed_notification_.release();
  }
}

RfcommChannelInternal::RfcommChannelInternal(
    multibuf::MultiBufAllocator& multibuf_allocator,
    ChannelProxy& l2cap_channel_proxy,
    ConnectionHandle connection_handle,
    uint8_t channel_number,
    RfcommDirection direction,
    bool mux_initiator,
    const RfcommChannelConfig& rx_config,
    const RfcommChannelConfig& tx_config,
    const pw::checksum::Crc8& crc_calculator,
    RfcommReceiveCallback&& receive_fn,
    RfcommEventCallback&& event_fn)
    : multibuf_allocator_(multibuf_allocator),
      l2cap_channel_proxy_(l2cap_channel_proxy),
      connection_handle_(connection_handle),
      channel_number_(channel_number),
      direction_(direction),
      mux_initiator_(mux_initiator),
      tx_config_(tx_config),
      crc_calculator_(crc_calculator),
      receive_fn_(std::move(receive_fn)),
      event_fn_(std::move(event_fn)),
      tx_credits_(tx_config.initial_credits),
      rx_credits_(rx_config.initial_credits),
      rx_total_credits_(rx_config.initial_credits) {
  PW_LOG_INFO("RFCOMM channel %u (direction %u): created",
              channel_number_,
              static_cast<uint8_t>(direction_));
}

StatusWithMultiBuf RfcommChannelInternal::Write(multibuf::MultiBuf&& payload) {
  std::lock_guard lock(mutex_);
  std::lock_guard tx_lock(tx_mutex_);
  if (state_ == State::kClosed) {
    return {Status::NotFound(), std::move(payload)};
  }

  // If the queue is full, return an unavailable status and pass the payload
  // back to the caller.
  if (tx_queue_.full()) {
    return {Status::Unavailable(), std::move(payload)};
  }
  tx_queue_.push(std::move(payload));
  // If there are credits available, try to send the packet.
  TryToSendPacket();
  return {OkStatus()};
}

void RfcommChannelInternal::AddCredits(uint8_t credits) {
  bool needs_notification = false;
  {
    std::lock_guard lock(mutex_);
    std::lock_guard tx_lock(tx_mutex_);
    if (state_ == State::kClosed) {
      return;
    }

    // Add credits, preventing overflow.
    if (std::numeric_limits<uint8_t>::max() - tx_credits_ < credits) {
      PW_LOG_WARN(
          "RFCOMM channel %u: TX credits overflow detected, capping at max.",
          channel_number_);
      tx_credits_ = std::numeric_limits<uint8_t>::max();
    } else {
      tx_credits_ += credits;
    }
    PW_LOG_DEBUG("RFCOMM channel %u: received %u TX credits, total is now %u",
                 channel_number_,
                 credits,
                 tx_credits_);
    // If the queue is not empty, try to send the packet. Otherwise, notify the
    // client that the channel is ready to send.
    if (!tx_queue_.empty()) {
      TryToSendPacket();
    } else {
      if (event_fn_) {
        needs_notification = true;
      }
    }
  }

  if (needs_notification) {
    event_fn_(RfcommEvent::kChannelReadyToSend);
  }
}

void RfcommChannelInternal::TryToSendPacket()
    PW_EXCLUSIVE_LOCKS_REQUIRED(tx_mutex_) {
  // Prioritize sending credits over data.
  if (pending_credit_tx_.has_value()) {
    auto credit_pdu_span = pending_credit_tx_->ContiguousSpan().value();
    if (!credit_pdu_span.empty()) {
      auto credits_sent =
          static_cast<uint16_t>(credit_pdu_span[credit_pdu_span.size() - 2]);
      StatusWithMultiBuf write_status =
          l2cap_channel_proxy_.Write(std::move(pending_credit_tx_.value()));
      if (!write_status.status.ok()) {
        if (write_status.buf.has_value() && !write_status.buf->empty()) {
          pending_credit_tx_ = std::move(write_status.buf);
        } else {
          PW_LOG_WARN("Pending tx credit packet lost after write failure");
          pending_credit_tx_.reset();
        }
        return;
      }
      pending_credit_tx_.reset();
      std::lock_guard lock(rx_mutex_);
      rx_credits_ += credits_sent;
    } else {
      PW_LOG_WARN("Invalid pending_credit_tx_ payload (size 0)");
      pending_credit_tx_.reset();
      return;
    }
  }

  const size_t max_frame_size = tx_config_.max_frame_size;

  // Try to send packets until the queue is empty or all credits are used.
  while (tx_credits_ > 0 && !tx_queue_.empty()) {
    // Get the first payload in the queue and the offset into the payload that
    // will be sent next.
    multibuf::MultiBuf& payload = tx_queue_.front();
    size_t offset = send_packet_offset_;

    // Calculate the size of the chunk to send. The chunk size is the minimum of
    // the remaining payload size and the maximum frame size.
    size_t chunk_size = std::min(payload.size() - offset, max_frame_size);

    const bool use_long_header =
        chunk_size >
        static_cast<size_t>(emboss::RfcommMaxInfoSize::ONE_BYTE_LENGTH);
    const size_t frame_size =
        chunk_size +
        static_cast<size_t>(
            use_long_header
                ? emboss::RfcommDataFrameOverhead::WITH_LONG_HEADER
                : emboss::RfcommDataFrameOverhead::WITH_SHORT_HEADER);

    // Allocate a buffer for the RFCOMM packet.
    auto result = multibuf_allocator_.AllocateContiguous(frame_size);
    if (!result.has_value()) {
      PW_LOG_ERROR(
          "RFCOMM channel %u: Failed to allocate buffer for RFCOMM packet",
          channel_number_);
      break;
    }
    multibuf::MultiBuf new_buffer = std::move(result.value());
    span<uint8_t> buffer =
        span_cast<uint8_t>(new_buffer.ContiguousSpan().value());

    auto frame_writer =
        emboss::MakeRfcommFrameView(buffer.data(), buffer.size());

    frame_writer.extended_address().Write(true);
    frame_writer.command_response().Write(mux_initiator_);
    frame_writer.direction().Write(direction_ == RfcommDirection::kInitiator);
    frame_writer.channel().Write(channel_number_);

    frame_writer.control().Write(pw::bluetooth::emboss::RfcommFrameType::
                                     UNNUMBERED_INFORMATION_WITH_HEADER_CHECK);
    if (use_long_header) {
      frame_writer.length_extended_flag().Write(
          pw::bluetooth::emboss::RfcommLengthExtended::EXTENDED);
      frame_writer.length_extended().Write(chunk_size);
    } else {
      frame_writer.length_extended_flag().Write(
          pw::bluetooth::emboss::RfcommLengthExtended::NORMAL);
      frame_writer.length().Write(chunk_size);
    }

    auto information = frame_writer.information();
    span<uint8_t> backing_storage(information.BackingStorage().data(),
                                  information.SizeInBytes());
    backing_storage = backing_storage.subspan(0, chunk_size);
    auto bytes_copied =
        payload.CopyTo(as_writable_bytes(backing_storage), offset);
    PW_CHECK_UINT_EQ(bytes_copied.size(), backing_storage.size());

    frame_writer.fcs().Write(crc_calculator_.Calculate(as_bytes(span(
        buffer.data(),
        static_cast<size_t>(emboss::RfcommHeaderLength::WITHOUT_LENGTH)))));

    // Write the packet to the L2CAP channel.
    StatusWithMultiBuf write_status =
        l2cap_channel_proxy_.Write(std::move(new_buffer));
    if (!write_status.status.ok()) {
      // L2CAP channel is busy, we will retry later. The packet remains in the
      // queue with its current offset.
      break;
    }

    tx_credits_--;
    send_packet_offset_ += chunk_size;

    // If have sent the entire payload, pop the queue and reset the offset
    // for the next packet.
    if (send_packet_offset_ >= payload.size()) {
      tx_queue_.pop();
      send_packet_offset_ = 0;
    }
  }
}

void RfcommChannelInternal::Close(RfcommEvent event) {
  {
    std::lock_guard lock(mutex_);
    std::lock_guard tx_lock(tx_mutex_);
    if (state_ == State::kClosed) {
      return;
    }
    state_ = State::kClosed;
    // Clear the queue and reset the offset for the next packet. This is to
    // avoid sending any packets after the channel is closed.
    tx_queue_.clear();
    send_packet_offset_ = 0;
  }
  // Notify the client that the channel is closed.
  if (event_fn_) {
    event_fn_(event);
  }
}

Status RfcommChannelInternal::SendAdditionalRxCredits(uint8_t credits) {
  {
    std::lock_guard lock(rx_mutex_);
    if (std::numeric_limits<uint8_t>::max() - credits < rx_total_credits_) {
      rx_total_credits_ = std::numeric_limits<uint8_t>::max();
      PW_LOG_WARN(
          "RFCOMM channel %u: RX total credits overflow detected, "
          "capping at max.",
          channel_number_);
    } else {
      rx_total_credits_ += credits;
    }
  }
  PW_LOG_DEBUG(
      "Max credits increased by: %d (to %d)", credits, rx_total_credits_);
  return SendCredits(credits);
}

// Sends credits (UIH frame with P-bit set) to the controller.
Status RfcommChannelInternal::SendCredits(uint8_t credits) {
  std::lock_guard lock(tx_mutex_);
  if (pending_credit_tx_.has_value()) {
    PW_LOG_WARN("Earlier rx credit send pending. Skipping");
    return Status::FailedPrecondition();
  }
  // RFCOMM frame with 1-byte length field and 1-byte credit.
  auto frame_size =
      static_cast<size_t>(emboss::RfcommDataFrameOverhead::WITH_LONG_HEADER);
  auto result = multibuf_allocator_.AllocateContiguous(frame_size);
  if (!result.has_value()) {
    PW_LOG_ERROR("RFCOMM channel %u: Failed to allocate buffer for credits",
                 channel_number_);
    return Status::ResourceExhausted();
  }
  multibuf::MultiBuf new_buffer = std::move(result.value());

  span<uint8_t> buffer =
      span_cast<uint8_t>(new_buffer.ContiguousSpan().value());
  auto frame_writer = emboss::MakeRfcommFrameView(buffer.data(), buffer.size());

  frame_writer.extended_address().Write(true);
  frame_writer.command_response().Write(mux_initiator_);
  frame_writer.direction().Write(direction_ == RfcommDirection::kInitiator);
  frame_writer.channel().Write(channel_number_);
  frame_writer.control().Write(
      pw::bluetooth::emboss::RfcommFrameType::
          UNNUMBERED_INFORMATION_WITH_HEADER_CHECK_AND_POLL_FINAL);

  frame_writer.length_extended_flag().Write(
      pw::bluetooth::emboss::RfcommLengthExtended::NORMAL);
  frame_writer.length().Write(0);

  frame_writer.credits().Write(credits);

  // For UIH frames, FCS is calculated over the address and control fields.
  frame_writer.fcs().Write(crc_calculator_.Calculate(as_bytes(
      span(buffer.data(),
           static_cast<size_t>(emboss::RfcommHeaderLength::WITHOUT_LENGTH)))));

  pending_credit_tx_ = std::move(new_buffer);
  TryToSendPacket();
  return OkStatus();
}

bool RfcommChannelInternal::HandlePduFromController(uint8_t credits,
                                                    ConstByteSpan pdu) {
  // Step 1: Handle credits if present.
  if (credits > 0) {
    AddCredits(credits);
  }

  // Step 2: Send data to the client.
  if (receive_fn_ && !pdu.empty()) {
    // TODO: https://pwbug.dev/478981478 - Rather than creating another
    // MultiBuf when a MultiBuf already exists for this packet in
    // RfcommChannelManager, return the payload part of the buffer.
    auto result = multibuf_allocator_.AllocateContiguous(pdu.size());
    if (!result.has_value()) {
      PW_LOG_ERROR("Failed to allocate buffer for RFCOMM channel number %u",
                   channel_number_);
      return true;
    }

    multibuf::MultiBuf& mbuf = result.value();
    auto bytes_copied = mbuf.CopyFrom(pdu);
    PW_CHECK(bytes_copied.ok());

    receive_fn_(std::move(mbuf));

    std::lock_guard lock(rx_mutex_);
    if (rx_credits_ > 0) {
      rx_credits_--;
    } else {
      PW_LOG_WARN(
          "RFCOMM channel %u: received packet with no RX credits remaining.",
          channel_number_);
    }
  }

  // Step 3: Update RX credits and send credits if needed.
  // Send credits if we have less than half of the initial credits to avoid
  // underflow.
  uint8_t credits_to_send = 0;
  bool needs_to_send_credits = false;
  {
    std::lock_guard lock(rx_mutex_);
    if (rx_credits_ <= rx_total_credits_ / 2) {
      credits_to_send = rx_total_credits_ - rx_credits_;
      if (credits_to_send > 0) {
        needs_to_send_credits = true;
      }
    }
  }
  if (needs_to_send_credits && !SendCredits(credits_to_send).ok()) {
    PW_LOG_WARN(
        "RFCOMM channel %u: Failed to send RX credits, will retry later.",
        channel_number_);
  }

  return false;
}

}  // namespace pw::bluetooth::proxy::rfcomm::internal
