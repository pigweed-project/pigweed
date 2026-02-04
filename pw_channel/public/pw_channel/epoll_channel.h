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
#include "pw_async2/epoll_dispatcher.h"
#include "pw_async2/poll.h"
#include "pw_channel/channel.h"
#include "pw_multibuf/allocator.h"
#include "pw_multibuf/multibuf.h"
#include "pw_multibuf/v1/allocator_async.h"

namespace pw::channel {

/// @module{pw_channel}

/// @defgroup pw_channel_epoll epoll
/// @{

/// Channel implementation which writes to and reads from a file descriptor,
/// backed by Linux's epoll notification system.
///
/// This channel depends on APIs provided by the EpollDispatcher and cannot be
/// used with any other dispatcher backend.
///
/// An instantiated EpollChannel takes ownership of the file descriptor it is
/// given, and will close it if the channel is closed or destroyed. Users should
/// not close a channel's file descriptor from outside.
class EpollChannel : public Implement<ByteReaderWriter> {
 public:
  EpollChannel(int channel_fd,
               async2::EpollDispatcher& dispatcher,
               multibuf::MultiBufAllocator& allocator)
      : channel_fd_(channel_fd),
        ready_to_write_(false),
        dispatcher_(&dispatcher),
        write_alloc_future_(allocator) {
    Register();
  }

  ~EpollChannel() override { Cleanup(); }

  EpollChannel(const EpollChannel&) = delete;
  EpollChannel& operator=(const EpollChannel&) = delete;

  EpollChannel(EpollChannel&&) = default;
  EpollChannel& operator=(EpollChannel&&) = default;

 private:
  static constexpr size_t kMinimumReadSize = 64;
  static constexpr size_t kDesiredReadSize = 1024;

  void Register();

  async2::PollResult<multibuf::MultiBuf> DoPendRead(
      async2::Context& cx) override;

  async2::Poll<Status> DoPendReadyToWrite(async2::Context& cx) final;

  async2::PollOptional<multibuf::MultiBuf> DoPendAllocateWriteBuffer(
      async2::Context& cx, size_t min_bytes) final {
    write_alloc_future_.SetDesiredSize(min_bytes);
    return write_alloc_future_.Pend(cx);
  }

  Status DoStageWrite(multibuf::MultiBuf&& data) final;

  async2::Poll<Status> DoPendWrite(async2::Context&) final {
    return OkStatus();
  }

  async2::Poll<Status> DoPendClose(async2::Context&) final {
    Cleanup();
    return async2::Ready(OkStatus());
  }

  void set_closed() {
    set_read_closed();
    set_write_closed();
  }

  void Cleanup();

  int channel_fd_;
  bool ready_to_write_;

  async2::EpollDispatcher* dispatcher_;
  multibuf::v1::MultiBufAllocationFuture write_alloc_future_;
  async2::Waker waker_;
};

/// @}

}  // namespace pw::channel
