// Copyright 2023 The Pigweed Authors
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

#include <algorithm>
#include <mutex>

#include "pw_bytes/span.h"
#include "pw_containers/intrusive_queue.h"
#include "pw_result/result.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_sync/lock_annotations.h"
#include "pw_sync/mutex.h"

namespace pw::rpc::internal {

// A simple thread-safe FIFO for queueing packets. Used by LocalRpcEgress to
// decouple receiving locally-destined RPC packets from their processing.
template <size_t kMaxPacketSize>
class PacketBufferQueue {
 public:
  class PacketBuffer : public IntrusiveQueue<PacketBuffer>::Item {
   public:
    Status CopyPacket(ConstByteSpan packet) {
      if (packet.size() > buffer_.size()) {
        return Status::ResourceExhausted();
      }
      std::copy(packet.begin(), packet.end(), buffer_.begin());
      size_ = packet.size();
      return OkStatus();
    }

    Result<ConstByteSpan> GetPacket() {
      auto buffer_span = span(buffer_);
      return buffer_span.first(size_);
    }

   protected:
    friend PacketBufferQueue;

   private:
    std::array<std::byte, kMaxPacketSize> buffer_ = {};
    size_t size_ = 0;
  };

  PacketBufferQueue() = default;
  explicit PacketBufferQueue(span<PacketBuffer> packets) {
    for (auto& packet : packets) {
      packet_list_.push_back(packet);
    }
  }

  ~PacketBufferQueue() { packet_list_.clear(); }

  // Push a packet to the end of the queue.
  void Push(PacketBuffer& packet) {
    const LockGuard guard(lock_);
    packet_list_.push_back(packet);
  }

  // Pop a packet from the head of the queue.
  // Returns a pointer to the packet popped from the queue, or
  // ResourceExhausted() if the queue is empty.
  Result<PacketBuffer*> Pop() {
    const LockGuard lock(lock_);

    if (packet_list_.empty()) {
      return Status::ResourceExhausted();
    }

    // Return the first available packet in the list.
    PacketBufferQueue::PacketBuffer& front = packet_list_.front();
    packet_list_.pop_front();
    return &front;
  }

 private:
  using LockGuard = ::std::lock_guard<sync::Mutex>;
  sync::Mutex lock_;
  IntrusiveQueue<PacketBuffer> packet_list_ PW_GUARDED_BY(lock_);
};

}  // namespace pw::rpc::internal
