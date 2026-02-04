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

#include "pw_channel/stream_channel.h"

#include "pw_async2/dispatcher.h"
#include "pw_log/log.h"
#include "pw_multibuf/multibuf.h"
#include "pw_status/status.h"
#include "pw_status/try.h"
#include "pw_thread/detached_thread.h"

namespace pw::channel {

using pw::OkStatus;
using pw::Result;
using pw::Status;
using pw::async2::Context;
using pw::async2::Pending;
using pw::async2::Poll;
using pw::async2::PollOptional;
using pw::async2::PollResult;
using pw::channel::ByteReaderWriter;
using pw::multibuf::MultiBuf;
using pw::multibuf::MultiBufAllocator;
using pw::multibuf::OwnedChunk;

namespace internal {

bool StreamChannelReadState::HasBufferToFill() {
  std::lock_guard lock(buffer_lock_);
  return !buffer_to_fill_.empty();
}

void StreamChannelReadState::ProvideBufferToFill(MultiBuf&& buf) {
  {
    std::lock_guard lock(buffer_lock_);
    buffer_to_fill_.PushSuffix(std::move(buf));
  }
  buffer_to_fill_available_.release();
}

PollResult<MultiBuf> StreamChannelReadState::PendFilledBuffer(Context& cx) {
  std::lock_guard lock(buffer_lock_);
  if (!filled_buffer_.empty()) {
    return std::move(filled_buffer_);
  }
  // Return an error status only after pulling all the data.
  if (!status_.ok()) {
    return status_;
  }
  PW_ASYNC_STORE_WAKER(
      cx, on_buffer_filled_, "StreamChannel is waiting on a `Stream::Read`");
  return Pending();
}

void StreamChannelReadState::ReadLoop(pw::stream::Reader& reader) {
  while (true) {
    OwnedChunk buffer = WaitForBufferToFillAndTakeFrontChunk();
    Result<pw::ByteSpan> read = reader.Read(buffer);
    if (!read.ok()) {
      SetReadError(read.status());

      if (!read.status().IsOutOfRange()) {
        PW_LOG_ERROR("Failed to read from stream in StreamChannel: %s",
                     read.status().str());
      }
      return;
    }
    buffer->Truncate(read->size());
    ProvideFilledBuffer(MultiBuf::FromChunk(std::move(buffer)));
  }
}

OwnedChunk StreamChannelReadState::WaitForBufferToFillAndTakeFrontChunk() {
  while (true) {
    {
      std::lock_guard lock(buffer_lock_);
      if (!buffer_to_fill_.empty()) {
        return buffer_to_fill_.TakeFrontChunk();
      }
    }
    buffer_to_fill_available_.acquire();
  }
  PW_UNREACHABLE;
}

void StreamChannelReadState::ProvideFilledBuffer(MultiBuf&& filled_buffer) {
  std::lock_guard lock(buffer_lock_);
  filled_buffer_.PushSuffix(std::move(filled_buffer));
  on_buffer_filled_.Wake();
}

void StreamChannelReadState::SetReadError(Status status) {
  std::lock_guard lock(buffer_lock_);
  status_ = status;
  on_buffer_filled_.Wake();
}

Status StreamChannelWriteState::SendData(MultiBuf&& buf) {
  {
    std::lock_guard lock(buffer_lock_);
    if (!status_.ok()) {
      return status_;
    }
    buffer_to_write_.PushSuffix(std::move(buf));
  }
  data_available_.release();
  return OkStatus();
}

void StreamChannelWriteState::WriteLoop(pw::stream::Writer& writer) {
  while (true) {
    data_available_.acquire();
    MultiBuf buffer;
    {
      std::lock_guard lock(buffer_lock_);
      if (buffer_to_write_.empty()) {
        continue;
      }
      buffer = std::move(buffer_to_write_);
    }
    for (const auto& chunk : buffer.Chunks()) {
      if (Status status = writer.Write(chunk); !status.ok()) {
        PW_LOG_ERROR("Failed to write to stream in StreamChannel: %s",
                     status.str());
        std::lock_guard lock(buffer_lock_);
        status_ = status;
        return;
      }
    }
  }
}

}  // namespace internal

static constexpr size_t kMinimumReadSize = 64;
static constexpr size_t kDesiredReadSize = 1024;

StreamChannel::StreamChannel(stream::Reader& reader,
                             const thread::Options& read_thread_options,
                             MultiBufAllocator& read_allocator,
                             stream::Writer& writer,
                             const thread::Options& write_thread_options,
                             MultiBufAllocator& write_allocator)
    : reader_(reader),
      writer_(writer),
      read_state_(),
      write_state_(),
      read_allocation_future_(read_allocator),
      write_allocation_future_(write_allocator) {
  pw::thread::DetachedThread(read_thread_options,
                             [this]() { read_state_.ReadLoop(reader_); });
  pw::thread::DetachedThread(write_thread_options,
                             [this]() { write_state_.WriteLoop(writer_); });
}

Status StreamChannel::ProvideBufferIfAvailable(Context& cx) {
  if (read_state_.HasBufferToFill()) {
    return OkStatus();
  }

  read_allocation_future_.SetDesiredSizes(
      kMinimumReadSize, kDesiredReadSize, pw::multibuf::v1::kNeedsContiguous);
  PollOptional<MultiBuf> maybe_multibuf = read_allocation_future_.Pend(cx);

  // If this is pending, we'll be awoken and this function will be re-run
  // when a buffer becomes available, allowing us to provide a buffer.
  if (maybe_multibuf.IsPending()) {
    return OkStatus();
  }

  if (!maybe_multibuf->has_value()) {
    PW_LOG_ERROR("Failed to allocate multibuf for reading");
    return Status::ResourceExhausted();
  }

  read_state_.ProvideBufferToFill(std::move(**maybe_multibuf));
  return OkStatus();
}

PollResult<MultiBuf> StreamChannel::DoPendRead(Context& cx) {
  PW_TRY(ProvideBufferIfAvailable(cx));
  return read_state_.PendFilledBuffer(cx);
}

Poll<Status> StreamChannel::DoPendReadyToWrite(Context&) { return OkStatus(); }

pw::Status StreamChannel::DoStageWrite(pw::multibuf::MultiBuf&& data) {
  PW_TRY(write_state_.SendData(std::move(data)));
  return OkStatus();
}

}  // namespace pw::channel
