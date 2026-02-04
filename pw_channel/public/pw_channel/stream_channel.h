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

#include "pw_async2/dispatcher.h"
#include "pw_channel/channel.h"
#include "pw_multibuf/allocator.h"
#include "pw_multibuf/multibuf.h"
#include "pw_multibuf/v1/allocator_async.h"
#include "pw_status/status.h"
#include "pw_stream/stream.h"
#include "pw_sync/interrupt_spin_lock.h"
#include "pw_sync/thread_notification.h"
#include "pw_thread/thread.h"

namespace pw::channel {
namespace internal {

/// State for the stream-reading thread.
class StreamChannelReadState {
 public:
  StreamChannelReadState() = default;
  StreamChannelReadState(const StreamChannelReadState&) = delete;
  StreamChannelReadState& operator=(const StreamChannelReadState&) = delete;
  StreamChannelReadState(StreamChannelReadState&&) = delete;
  StreamChannelReadState& operator=(StreamChannelReadState&&) = delete;

  /// Whether or not the `ReadLoop` already has a buffer available into which
  /// data can be read.
  bool HasBufferToFill();

  /// Provide a buffer for `ReadLoop` to read data into.
  void ProvideBufferToFill(multibuf::MultiBuf&& buf);

  /// Receives any available data processed by `ReadLoop`.
  ///
  /// If no data is available, schedules a wakeup of the task in `cx` when
  /// new data arrives.
  async2::PollResult<multibuf::MultiBuf> PendFilledBuffer(async2::Context& cx);

  /// A loop which reads data from `reader` into buffers provided by
  /// `ProvideBufferToFill` and then makes them available via
  /// `PendFilledBuffer`.
  ///
  /// This is blocking and is intended to be run on an independent thread.
  void ReadLoop(stream::Reader& reader);

 private:
  multibuf::OwnedChunk WaitForBufferToFillAndTakeFrontChunk();
  void ProvideFilledBuffer(multibuf::MultiBuf&& filled_buffer);
  void SetReadError(Status status);

  sync::ThreadNotification buffer_to_fill_available_;
  async2::Waker on_buffer_filled_;
  sync::InterruptSpinLock buffer_lock_;
  multibuf::MultiBuf buffer_to_fill_ PW_GUARDED_BY(buffer_lock_);
  multibuf::MultiBuf filled_buffer_ PW_GUARDED_BY(buffer_lock_);
  Status status_ PW_GUARDED_BY(buffer_lock_);
};

/// State for the stream-writing thread.
class StreamChannelWriteState {
 public:
  StreamChannelWriteState() = default;
  StreamChannelWriteState(const StreamChannelWriteState&) = delete;
  StreamChannelWriteState& operator=(const StreamChannelWriteState&) = delete;
  StreamChannelWriteState(StreamChannelWriteState&&) = delete;
  StreamChannelWriteState& operator=(StreamChannelWriteState&&) = delete;

  /// Queues `buf` to be sent into `writer` via the `WriteLoop`.
  ///
  /// Returns a status indicating whether the `WriteLoop` has encountered
  /// errors writing into `writer`.
  Status SendData(multibuf::MultiBuf&& buf);

  /// A loop which writes the data sent via `SendData` into `writer`.
  ///
  /// This is blocking and is intended to be run on an independent thread.
  void WriteLoop(stream::Writer& writer);

 private:
  sync::ThreadNotification data_available_;
  sync::InterruptSpinLock buffer_lock_;
  multibuf::MultiBuf buffer_to_write_ PW_GUARDED_BY(buffer_lock_);
  Status status_;
};

}  // namespace internal

/// @module{pw_channel}

/// @defgroup pw_channel_stream_channel Stream channel
/// @{

/// A channel which delegates to an underlying reader and writer stream.
///
/// NOTE: this channel as well as its `reader` and `writer` must all continue to
/// exist for the duration of the program, as they are referenced by other
/// threads.
///
/// This unfortunate requirement is due to the fact that `Stream::Read` and
/// `Stream::Write` are blocking. The stream reading and writing threaads
/// may be blocked on `Read` or `Write` calls, and therefore cannot cleanly
/// be shutdown.
class StreamChannel final
    : public channel::Implement<channel::ByteReaderWriter> {
 public:
  StreamChannel(stream::Reader& reader,
                const thread::Options& read_thread_options,
                multibuf::MultiBufAllocator& read_allocator,
                stream::Writer& writer,
                const thread::Options& write_thread_option,
                multibuf::MultiBufAllocator& write_allocator);

  // Deprecated: prefer the two-allocator constructor in order to prevent reads
  // and writes blocking on waiting for buffer space from the other.
  StreamChannel(multibuf::MultiBufAllocator& allocator,
                stream::Reader& reader,
                const thread::Options& read_thread_options,
                stream::Writer& writer,
                const thread::Options& write_thread_options)
      : StreamChannel(reader,
                      read_thread_options,
                      allocator,
                      writer,
                      write_thread_options,
                      allocator) {}

  // StreamChannel is referenced from other threads and is therefore not movable
  // or copyable.
  StreamChannel(const StreamChannel&) = delete;
  StreamChannel& operator=(const StreamChannel&) = delete;
  StreamChannel(StreamChannel&&) = delete;
  StreamChannel& operator=(StreamChannel&&) = delete;

 private:
  // StreamChannel must live forever, as its state is referenced by other
  // threads.
  ~StreamChannel() final = default;

  // Even though the destructor is never called, classes with private
  // destructors must friend `pw::NoDestructor`.
  friend class pw::NoDestructor<StreamChannel>;

  Status ProvideBufferIfAvailable(async2::Context& cx);

  async2::PollResult<multibuf::MultiBuf> DoPendRead(
      async2::Context& cx) override;

  async2::Poll<Status> DoPendReadyToWrite(async2::Context& cx) override;

  async2::PollOptional<multibuf::MultiBuf> DoPendAllocateWriteBuffer(
      async2::Context& cx, size_t min_bytes) override {
    write_allocation_future_.SetDesiredSize(min_bytes);
    return write_allocation_future_.Pend(cx);
  }

  Status DoStageWrite(multibuf::MultiBuf&& data) override;

  async2::Poll<Status> DoPendWrite(async2::Context&) override {
    return OkStatus();
  }

  async2::Poll<Status> DoPendClose(async2::Context&) override {
    return async2::Ready(OkStatus());
  }

  stream::Reader& reader_;
  stream::Writer& writer_;
  internal::StreamChannelReadState read_state_;
  internal::StreamChannelWriteState write_state_;
  multibuf::v1::MultiBufAllocationFuture read_allocation_future_;
  multibuf::v1::MultiBufAllocationFuture write_allocation_future_;
};

/// @}

}  // namespace pw::channel
