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

#include "pw_channel/rp2_stdio_channel.h"

#include "pico/stdlib.h"
#include "pw_assert/check.h"
#include "pw_async2/dispatcher.h"
#include "pw_log/log.h"
#include "pw_multibuf/allocator.h"
#include "pw_multibuf/multibuf.h"
#include "pw_multibuf/v1/allocator_async.h"
#include "pw_status/status.h"

namespace pw::channel {
namespace {

using ::pw::async2::Context;
using ::pw::async2::Pending;
using ::pw::async2::Poll;
using ::pw::async2::PollOptional;
using ::pw::async2::PollResult;
using ::pw::async2::Ready;
using ::pw::async2::Waker;
using ::pw::multibuf::MultiBuf;
using ::pw::multibuf::MultiBufAllocator;
using ::pw::multibuf::v1::MultiBufAllocationFuture;

Waker global_chars_available_waker;

void InitStdio() {
  stdio_init_all();
  stdio_set_chars_available_callback(
      []([[maybe_unused]] void* arg) { global_chars_available_waker.Wake(); },
      nullptr);
}

void WriteMultiBuf(const MultiBuf& buf) {
  for (std::byte b : buf) {
    putchar_raw(static_cast<int>(b));
  }
}

Poll<std::byte> PollReadByte(Context& cx) {
  int c = getchar_timeout_us(0);
  if (c == PICO_ERROR_TIMEOUT) {
    PW_ASYNC_STORE_WAKER(
        cx,
        global_chars_available_waker,
        "RP2StdioChannel is waiting for stdio chars available");
    // Read again to ensure that no race occurred.
    //
    // The concern is an interleaving like this
    // Thread one: get_char is called and times out
    // Thread two: char becomes available, Wake is called
    // Thread one: sets Waker
    //
    // In this interleaving, the task on Thread one is never awoken,
    // so we must check for available characters *after* setting the Waker.
    c = getchar_timeout_us(0);
    if (c == PICO_ERROR_TIMEOUT) {
      return Pending();
    }
  }
  return static_cast<std::byte>(c);
}

// Channel implementation which writes to and reads from rp2040's stdio.
//
// NOTE: only one Rp2StdioChannel may be in existence.
class Rp2StdioChannel final : public pw::channel::Implement<ByteReaderWriter> {
 public:
  Rp2StdioChannel(MultiBufAllocator& read_allocator,
                  MultiBufAllocator& write_allocator)
      : read_allocation_future_(read_allocator),
        write_allocation_future_(write_allocator),
        buffer_(std::nullopt) {}

  Rp2StdioChannel(const Rp2StdioChannel&) = delete;
  Rp2StdioChannel& operator=(const Rp2StdioChannel&) = delete;

  // This is a singleton static, so there's no need for move constructors.
  Rp2StdioChannel(Rp2StdioChannel&&) = delete;
  Rp2StdioChannel& operator=(Rp2StdioChannel&&) = delete;

 private:
  static constexpr size_t kMinimumReadSize = 64;
  static constexpr size_t kDesiredReadSize = 1024;

  Poll<Status> PendGetReadBuffer(Context& cx);

  PollResult<MultiBuf> DoPendRead(Context& cx) override;

  Poll<Status> DoPendReadyToWrite(Context& cx) override;

  PollOptional<MultiBuf> DoPendAllocateWriteBuffer(Context& cx,
                                                   size_t min_bytes) override {
    write_allocation_future_.SetDesiredSize(min_bytes);
    return write_allocation_future_.Pend(cx);
  }

  Status DoStageWrite(MultiBuf&& data) override;

  Poll<Status> DoPendWrite(Context&) override { return OkStatus(); }

  Poll<Status> DoPendClose(Context&) override { return Ready(OkStatus()); }

  MultiBufAllocationFuture read_allocation_future_;
  MultiBufAllocationFuture write_allocation_future_;
  std::optional<MultiBuf> buffer_;
};

Poll<Status> Rp2StdioChannel::PendGetReadBuffer(Context& cx) {
  if (buffer_.has_value()) {
    return OkStatus();
  }

  read_allocation_future_.SetDesiredSizes(
      kMinimumReadSize, kDesiredReadSize, pw::multibuf::v1::kNeedsContiguous);
  PollOptional<MultiBuf> maybe_multibuf = read_allocation_future_.Pend(cx);
  if (maybe_multibuf.IsPending()) {
    return Pending();
  }
  if (!maybe_multibuf->has_value()) {
    PW_LOG_ERROR("Failed to allocate multibuf for reading");
    return Status::ResourceExhausted();
  }

  buffer_ = std::move(**maybe_multibuf);
  return OkStatus();
}

PollResult<MultiBuf> Rp2StdioChannel::DoPendRead(Context& cx) {
  Poll<Status> buffer_ready = PendGetReadBuffer(cx);
  if (buffer_ready.IsPending() || !buffer_ready->ok()) {
    return buffer_ready;
  }

  size_t len = 0;
  for (std::byte& b : *buffer_) {
    Poll<std::byte> next = PollReadByte(cx);
    if (next.IsPending()) {
      break;
    }
    b = *next;
    ++len;
  }
  if (len == 0) {
    return Pending();
  }
  buffer_->Truncate(len);
  MultiBuf buffer = std::move(*buffer_);
  buffer_ = std::nullopt;
  return buffer;
}

Poll<Status> Rp2StdioChannel::DoPendReadyToWrite(Context&) {
  return OkStatus();
}

Status Rp2StdioChannel::DoStageWrite(MultiBuf&& data) {
  WriteMultiBuf(data);

  return OkStatus();
}

}  // namespace

ByteReaderWriter& Rp2StdioChannelInit(MultiBufAllocator& read_allocator,
                                      MultiBufAllocator& write_allocator) {
  static Rp2StdioChannel channel = [&] {
    InitStdio();
    return Rp2StdioChannel(read_allocator, write_allocator);
  }();
  return channel.channel();
}

}  // namespace pw::channel
