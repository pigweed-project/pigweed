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
#include <optional>

#include "pw_async2/dispatcher.h"
#include "pw_async2/poll.h"
#include "pw_channel/channel.h"
#include "pw_multibuf/allocator.h"
#include "pw_multibuf/multibuf.h"
#include "pw_multibuf/v1/allocator_async.h"

namespace pw::channel {

/// @module{pw_channel}

/// @defgroup pw_channel_loopback Loopback
/// @{

// Channel implementation which will read its own writes.
template <DataType kType>
class LoopbackChannel;

/// Alias for a loopback channel that sends and receives datagrams.
using LoopbackDatagramChannel = LoopbackChannel<DataType::kDatagram>;

/// Alias for a loopback channel that sends and receives bytes.
using LoopbackByteChannel = LoopbackChannel<DataType::kByte>;

/// @}

template <>
class LoopbackChannel<DataType::kDatagram>
    : public Implement<ReliableDatagramReaderWriter> {
 public:
  LoopbackChannel(multibuf::MultiBufAllocator& write_allocator)
      : write_alloc_future_(write_allocator) {}
  LoopbackChannel(const LoopbackChannel&) = delete;
  LoopbackChannel& operator=(const LoopbackChannel&) = delete;

  LoopbackChannel(LoopbackChannel&&) = default;
  LoopbackChannel& operator=(LoopbackChannel&&) = default;

 private:
  async2::PollResult<multibuf::MultiBuf> DoPendRead(
      async2::Context& cx) override;

  async2::Poll<Status> DoPendReadyToWrite(async2::Context& cx) final;

  async2::PollOptional<multibuf::MultiBuf> DoPendAllocateWriteBuffer(
      async2::Context& cx, size_t min_bytes) final {
    write_alloc_future_.SetDesiredSize(min_bytes);
    return write_alloc_future_.Pend(cx);
  }

  Status DoStageWrite(multibuf::MultiBuf&& data) final;

  async2::Poll<Status> DoPendWrite(async2::Context&) final;

  async2::Poll<Status> DoPendClose(async2::Context&) final;

  multibuf::v1::MultiBufAllocationFuture write_alloc_future_;
  std::optional<multibuf::MultiBuf> queue_;

  async2::Waker waker_;
};

template <>
class LoopbackChannel<DataType::kByte>
    : public Implement<ReliableByteReaderWriter> {
 public:
  LoopbackChannel(multibuf::MultiBufAllocator& write_allocator)
      : write_alloc_future_(write_allocator) {}
  LoopbackChannel(const LoopbackChannel&) = delete;
  LoopbackChannel& operator=(const LoopbackChannel&) = delete;

  LoopbackChannel(LoopbackChannel&&) = default;
  LoopbackChannel& operator=(LoopbackChannel&&) = default;

 private:
  async2::PollResult<multibuf::MultiBuf> DoPendRead(
      async2::Context& cx) override;

  async2::Poll<Status> DoPendReadyToWrite(async2::Context&) final {
    return async2::Ready(OkStatus());
  }

  async2::PollOptional<multibuf::MultiBuf> DoPendAllocateWriteBuffer(
      async2::Context& cx, size_t min_bytes) final {
    write_alloc_future_.SetDesiredSize(min_bytes);
    return write_alloc_future_.Pend(cx);
  }

  Status DoStageWrite(multibuf::MultiBuf&& data) final;

  async2::Poll<Status> DoPendWrite(async2::Context&) final;

  async2::Poll<Status> DoPendClose(async2::Context&) final;

  multibuf::v1::MultiBufAllocationFuture write_alloc_future_;
  multibuf::MultiBuf queue_;

  async2::Waker read_waker_;
};

}  // namespace pw::channel
