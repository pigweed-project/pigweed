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
#include <mutex>
#include <optional>

#include "pw_async2/dispatcher.h"
#include "pw_async2/poll.h"
#include "pw_channel/channel.h"
#include "pw_multibuf/allocator.h"
#include "pw_multibuf/v1/allocator_async.h"
#include "pw_sync/lock_annotations.h"
#include "pw_sync/mutex.h"

namespace pw::channel {
namespace internal {

// Internal Channel implementation for use with ForwardingChannelPair. It is
// specialized for kDatagram and kByte.
template <DataType kType>
class ForwardingChannel;

}  // namespace internal

/// @module{pw_channel}

/// @defgroup pw_channel_forwarding Forwarding
/// @{

/// Forwards either datagrams or bytes between two channels. Writes to the first
/// channel appear as reads on the second, and vice versa.
///
/// `ForwardingChannelPair` enables connecting two subsystems that communicate
/// with channels without implementing a custom channel.
template <DataType kType>
class ForwardingChannelPair {
 public:
  explicit constexpr ForwardingChannelPair(
      multibuf::MultiBufAllocator& first_write_alloc,
      multibuf::MultiBufAllocator& second_write_alloc);

  ForwardingChannelPair(const ForwardingChannelPair&) = delete;
  ForwardingChannelPair& operator=(const ForwardingChannelPair&) = delete;

  ForwardingChannelPair(ForwardingChannelPair&&) = delete;
  ForwardingChannelPair& operator=(ForwardingChannelPair&&) = delete;

  /// Returns the first channel in the pair.
  Channel<kType, kReliable, kReadable, kWritable>& first() {
    return first_.channel();
  }

  /// Returns a const reference to the first channel in the pair.
  const Channel<kType, kReliable, kReadable, kWritable>& first() const {
    return first_;
  }

  /// Returns the second channel in the pair.
  Channel<kType, kReliable, kReadable, kWritable>& second() {
    return second_.channel();
  }

  /// Returns a const reference to the second channel in the pair.
  const Channel<kType, kReliable, kReadable, kWritable>& second() const {
    return second_;
  }

 private:
  template <DataType>
  friend class internal::ForwardingChannel;

  sync::Mutex mutex_;
  // These channels refer to each other, so their lifetimes must match.
  internal::ForwardingChannel<kType> first_;
  internal::ForwardingChannel<kType> second_;
};

/// Alias for a pair of forwarding datagram channels.
using ForwardingDatagramChannelPair =
    ForwardingChannelPair<DataType::kDatagram>;

/// Alias for a pair of forwarding byte channels.
using ForwardingByteChannelPair = ForwardingChannelPair<DataType::kByte>;

/// @}

namespace internal {

template <>
class ForwardingChannel<DataType::kDatagram>
    : public Implement<ReliableDatagramReaderWriter> {
 public:
  ForwardingChannel(const ForwardingChannel&) = delete;
  ForwardingChannel& operator=(const ForwardingChannel&) = delete;

  ForwardingChannel(ForwardingChannel&&) = delete;
  ForwardingChannel& operator=(ForwardingChannel&&) = delete;

 private:
  friend class ForwardingChannelPair<DataType::kDatagram>;

  constexpr ForwardingChannel(ForwardingChannelPair<DataType::kDatagram>& pair,
                              ForwardingChannel* sibling,
                              multibuf::MultiBufAllocator& write_alloc)
      : pair_(pair), sibling_(*sibling), write_alloc_future_(write_alloc) {}

  async2::PollResult<multibuf::MultiBuf> DoPendRead(
      async2::Context& cx) override;

  async2::Poll<Status> DoPendReadyToWrite(async2::Context& cx) override;

  async2::PollOptional<multibuf::MultiBuf> DoPendAllocateWriteBuffer(
      async2::Context& cx, size_t min_bytes) override {
    write_alloc_future_.SetDesiredSize(min_bytes);
    return write_alloc_future_.Pend(cx);
  }

  Status DoStageWrite(multibuf::MultiBuf&& data) override;

  async2::Poll<Status> DoPendWrite(async2::Context&) override {
    return OkStatus();
  }

  async2::Poll<Status> DoPendClose(async2::Context&) override;

  // The two channels share one mutex. Lock safty analysis doesn't understand
  // that, so has to be disabled for some functions.
  ForwardingChannelPair<DataType::kDatagram>& pair_;
  ForwardingChannel& sibling_;

  // Could use a queue here.
  std::optional<multibuf::MultiBuf> read_queue_ PW_GUARDED_BY(pair_.mutex_);
  async2::Waker waker_ PW_GUARDED_BY(pair_.mutex_);
  multibuf::v1::MultiBufAllocationFuture write_alloc_future_;
};

template <>
class ForwardingChannel<DataType::kByte>
    : public Implement<ReliableByteReaderWriter> {
 public:
  ForwardingChannel(const ForwardingChannel&) = delete;
  ForwardingChannel& operator=(const ForwardingChannel&) = delete;

  ForwardingChannel(ForwardingChannel&&) = delete;
  ForwardingChannel& operator=(ForwardingChannel&&) = delete;

 private:
  friend class ForwardingChannelPair<DataType::kByte>;

  constexpr ForwardingChannel(ForwardingChannelPair<DataType::kByte>& pair,
                              ForwardingChannel* sibling,
                              multibuf::MultiBufAllocator& write_alloc)
      : pair_(pair), sibling_(*sibling), write_alloc_future_(write_alloc) {}

  async2::PollResult<multibuf::MultiBuf> DoPendRead(
      async2::Context& cx) override;

  async2::Poll<Status> DoPendReadyToWrite(async2::Context&) override {
    return async2::Ready(OkStatus());
  }

  async2::PollOptional<multibuf::MultiBuf> DoPendAllocateWriteBuffer(
      async2::Context& cx, size_t min_bytes) override {
    write_alloc_future_.SetDesiredSize(min_bytes);
    return write_alloc_future_.Pend(cx);
  }

  Status DoStageWrite(multibuf::MultiBuf&& data) override;

  async2::Poll<Status> DoPendWrite(async2::Context&) override {
    return OkStatus();
  }

  async2::Poll<Status> DoPendClose(async2::Context&) override;

  ForwardingChannelPair<DataType::kByte>& pair_;
  ForwardingChannel& sibling_;

  multibuf::MultiBuf read_queue_ PW_GUARDED_BY(pair_.mutex_);
  async2::Waker read_waker_ PW_GUARDED_BY(pair_.mutex_);
  multibuf::v1::MultiBufAllocationFuture write_alloc_future_;
};

}  // namespace internal

// Define the constructor out-of-line, after ForwardingChannel is defined.
template <DataType kType>
constexpr ForwardingChannelPair<kType>::ForwardingChannelPair(
    multibuf::MultiBufAllocator& first_write_allocator,
    multibuf::MultiBufAllocator& second_write_allocator)
    : first_(*this, &second_, first_write_allocator),
      second_(*this, &first_, second_write_allocator) {}

}  // namespace pw::channel
