// Copyright 2025 The Pigweed Authors
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

#include "pw_bluetooth_proxy/internal/recombiner.h"

#include <optional>

#include "pw_assert/check.h"
#include "pw_log/log.h"
#include "pw_status/status.h"
#include "pw_status/try.h"

namespace pw::bluetooth::proxy {

pw::Status Recombiner::StartRecombination(LockedL2capChannel& channel,
                                          size_t size,
                                          size_t extra_header_size) {
  if (IsActive()) {
    return Status::FailedPrecondition();
  }

  // Store extra space at front of recombine buffer so callier can use it
  // to create headers if needed.
  PW_TRY(channel.channel().StartRecombinationBuf(
      direction_, size, extra_header_size));

  is_active_ = true;
  local_cid_ = channel.channel().local_cid();
  expected_size_ = size;
  recombined_size_ = 0;

  return pw::OkStatus();
}

pw::Status Recombiner::RecombineFragment(
    std::optional<LockedL2capChannel>& channel, pw::span<const uint8_t> data) {
  if (!IsActive()) {
    return Status::FailedPrecondition();
  }

  if (!HasBuf(channel)) {
    // Channel we started recombination with was destroyed. Even if we were
    // passed a channel, if its recombination buffer is gone that indicates it
    // is a new channel.
    // We still need to complete recombination to read the fragments from ACL
    // connection, but Recombiner won't be storing the data (just tracking
    // sizes).

    PW_LOG_INFO(
        "The channel instance (local cid %#x) that was initially receiving the "
        "recombined PDU %s is no longer acquired. Will complete reading the "
        "recombined PDU, but result will be dropped.",
        local_cid(),
        DirectionToString(direction_));

  } else {
    // We have a channel, add data to its buf.

    // Since channel was found in caller using cid, this should never be false.
    PW_CHECK_INT_EQ(channel->channel().local_cid(), local_cid_);

    PW_TRY(channel->channel().CopyToRecombinationBuf(
        direction_, as_bytes(data), write_offset()));
  }

  recombined_size_ += data.size();

  if (IsComplete()) {
    // Recombination is complete so we are no longer active (no longer
    // recombining). Note buf is left in channel to be taken later in static
    // TakeBuf
    is_active_ = false;
  }

  return pw::OkStatus();
}

// static
multibuf::MultiBuf Recombiner::TakeBuf(
    std::optional<LockedL2capChannel>& channel, Direction direction) {
  PW_CHECK(channel.has_value());
  PW_CHECK(channel->channel().HasRecombinationBuf(direction));
  return channel->channel().TakeRecombinationBuf(direction);
}

void Recombiner::EndRecombination(std::optional<LockedL2capChannel>& channel) {
  is_active_ = false;
  expected_size_ = 0;
  recombined_size_ = 0;

  if (!channel.has_value()) {
    // Channel and its recombination writer has already been destroyed.
    return;
  }

  PW_CHECK_INT_EQ(channel->channel().local_cid(), local_cid_);

  channel->channel().EndRecombinationBuf(direction_);
}

}  // namespace pw::bluetooth::proxy
