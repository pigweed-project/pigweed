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

#pragma once

#include "pw_assert/assert.h"
#include "pw_bluetooth_proxy/internal/locked_l2cap_channel.h"
#include "pw_multibuf/multibuf.h"
#include "pw_span/span.h"
#include "pw_status/status.h"

namespace pw::bluetooth::proxy {

// Recombiner recombines a fragmented ACL payload for a channel into a single
// payload.
//
// Its functions are passed the locked target channel which it uses to
// provide storage for the payload as it is recombined. Currently the locked
// channel needs to be passed for each call because client can dtor it from
// under us. Passing the locked version of the channel helps to ensure we
// actually have the lock.
//
// TODO: https://pwbug.dev/402454277 - Once we have channel ref ptrs the
// Recombiner can hold on to its channel's ref ptr for the duration of
// recombination. So will only need it passed at start of recombination.
class Recombiner {
 public:
  Recombiner(Direction direction) : direction_(direction) {}

  // Starts a new recombination session.
  //
  // Allocate extra_header_size at front of the multibuf for possible use in
  // headers if needed. That extra is discarded so it will not be part of
  // initial MultiBuf view returned by TakeBuf.
  //
  // Precondition: `!IsActive()`
  //
  // Postcondition: `IsActive()`
  //
  // Returns:
  // * FAILED_PRECONDITION if recombination is already active.
  // * Any error from MultiBufWriter::Create(), namely RESOURCE_EXHAUSTED.
  // * OK if recombination is started.
  pw::Status StartRecombination(LockedL2capChannel& channel,
                                size_t size,
                                size_t extra_header_size);

  // Adds a fragment of data to the recombination buffer.
  //
  // If the data completes recombination, then `IsComplete() will now be true
  // and `IsActive()` will be false.
  //
  // Preconditions: `IsActive()`, `!IsComplete()`
  //
  // Postconditions if recombination is not completed: Same as Preconditions.
  //
  // Postconditions if recombination is completed: `!IsActive()`, `IsComplete()`
  //
  // Returns:
  // * FAILED_PRECONDITION if recombination is not active.
  // * Any error from MultiBufWriter::Write(), namely RESOURCE_EXHAUSTED.
  // * OK if the data was written
  pw::Status RecombineFragment(std::optional<LockedL2capChannel>& channel,
                               pw::span<const uint8_t> data);

  // Returns the recombined MultiBuf and ends recombination.
  //
  // The MultiBuf will be non-empty and contiguous.
  //
  // Preconditions: `IsComplete()` on relevant Recombiner instance.
  //
  // Postconditions: `!HasBuf()`
  //
  // This is static method so that it can be called outside of the connection
  // mutex. It can be called once after IsComplete is returned by the relevant
  // Recombiner instance.
  static multibuf::MultiBuf TakeBuf(std::optional<LockedL2capChannel>& channel,
                                    Direction direction);

  // Ends recombination.
  //
  // Postconditions: `!IsActive()`
  //
  // Frees the MultiBuf held in the channel (if any).
  void EndRecombination(std::optional<LockedL2capChannel>& channel);

  // Returns true if channel is present and has a recombination buffer for the
  // given direction.
  static bool HasBuf(std::optional<LockedL2capChannel>& channel,
                     Direction direction) {
    return channel.has_value() &&
           channel->channel().HasRecombinationBuf(direction);
  }

  // Returns true if channel is present and has a recombination buffer.
  bool HasBuf(std::optional<LockedL2capChannel>& channel) const {
    return HasBuf(channel, direction_);
  }

  // Returns true if recombined size matches specified size.
  // Should only be called after a RecombineFragment.
  bool IsComplete() const {
    PW_ASSERT(expected_size_ > 0);
    return recombined_size_ == expected_size_;
  }

  // Returns true if recombination is is in progress. So we have started
  // receiving and recombining fragments, but have not completed yet.
  uint16_t IsActive() const { return is_active_; }

  // Returns CID of channel being recombined (local_cid for kFromController,
  // remote_cid for kFromHost). Should only be called when recombination is
  // active.
  uint16_t cid() const {
    PW_ASSERT(IsActive());
    return cid_;
  }

 private:
  size_t write_offset() const { return recombined_size_; }

  bool is_active_ = false;
  Direction direction_;
  uint16_t cid_ = 0;
  size_t expected_size_ = 0;
  size_t recombined_size_ = 0;
};

}  // namespace pw::bluetooth::proxy
