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

#include "pw_allocator/unique_ptr.h"
#include "pw_bytes/span.h"
#include "pw_function/function.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"

namespace pw::grpc {

// Source of serialized HTTP/2 frames pulled by a transport SendQueue.
class FrameSource {
 public:
  // Minimum packet buffer size accepted by FillPacket(). Must be large enough
  // for any single HTTP/2 control frame (SETTINGS is 27 B, GOAWAY/PING 17 B,
  // RST_STREAM/WINDOW_UPDATE 13 B), response HEADERS (~30 B) or trailers
  // (~23 B), or a DATA frame header (9 B) plus gRPC prefix (5 B) and payload.
  // 64 B is a small power-of-two floor that fits all of these without zero-
  // progress stalls.
  static constexpr size_t kMinPacketSize = 64;

  virtual ~FrameSource() = default;

  // Called by the transport thread when it can send one more packet.
  // Writes zero or more complete HTTP/2 frames into `packet` and returns the
  // number of bytes written (0 if nothing is currently ready to send), or an
  // error status on fatal failure. Returns INVALID_ARGUMENT if
  // `packet.size() < kMinPacketSize`.
  //
  // Must be called without any transport lock held.
  virtual StatusWithSize FillPacket(ByteSpan packet) = 0;
};

// Abstract interface for sending response buffers. Buffers are written to the
// response in the order they are queued.
//
// Callers should use either push mode (QueueSend) or pull mode
// (SetFrameSource), not both. When a FrameSource is registered, an
// implementation may drain queued push buffers before pulling, so pushed and
// pulled frames are not ordered relative to each other.
class SendQueue {
 public:
  using ErrorHandler = pw::Function<void(pw::Status)>;
  using SpaceAvailableCallback = pw::Function<void()>;

  virtual ~SendQueue() {}

  // Thread safe. Queues buffer to be sent on send thread. Returns false if
  // there was no queue space available from allocator.
  virtual bool QueueSend(UniquePtr<std::byte[]>&& buffer) = 0;

  // Sets callback to be called when write to socket fails. QueueSend must not
  // be called from this callback.
  virtual void set_on_error(ErrorHandler&& error_handler) = 0;

  // Sets callback to be called when space becomes available in the send queue.
  // This callback may be called with internal queue mutexes held. Callers
  // must not call back into this SendQueue (such as QueueSend) from within
  // this callback.
  virtual void set_on_space_available(SpaceAvailableCallback&&) {}

  // Returns true if this SendQueue pulls frames via FrameSource::FillPacket()
  // instead of expecting frames pushed via QueueSend().
  virtual bool SupportsPull() const { return false; }

  // Returns the maximum payload size (in bytes) that this transport sends in a
  // single underlying packet, or 0 if unbounded or unknown. Wrapper transports
  // (such as TLS) can use this to size each record to fit one transport frame.
  virtual size_t MaxPacketSize() const { return 0; }

  // Registers, replaces, or clears the pull FrameSource. Passing nullptr or a
  // different source stops pulls from the previous source and blocks until any
  // FillPacket() call in progress on that source has returned. Must not be
  // called from inside FrameSource::FillPacket(), as waiting for the in-flight
  // pull would deadlock.
  virtual void SetFrameSource(FrameSource* /*source*/) {}

  // Notifies the transport that the FrameSource has new data ready to pull.
  // Must not block and must not call back into FrameSource synchronously.
  // Safe to call with the caller's internal locks held.
  virtual void NotifyDataReady() {}

  // Runs this send queue.
  virtual void Run() = 0;

  // Stops the current Run() call.
  virtual void RequestStop() = 0;
};

}  // namespace pw::grpc
