// Copyright 2022 The Pigweed Authors
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

// IWYU pragma: private, include "pw_rpc/pwpb/client_server_testing_threaded.h"

#include <cinttypes>

#include "pw_function/function.h"
#include "pw_rpc/channel.h"
#include "pw_rpc/client_server.h"
#include "pw_rpc/internal/client_server_testing.h"  // IWYU pragma: export
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_sync/binary_semaphore.h"
#include "pw_sync/mutex.h"
#include "pw_thread/thread.h"

namespace pw::rpc {
namespace internal {

// Expands on a Forwarding Channel Output implementation to allow for
// observation of packets.
template <typename FakeChannelOutputImpl,
          size_t kOutputSize,
          size_t kMaxPackets,
          size_t kPayloadsBufferSizeBytes>
class WatchableChannelOutput
    : public ForwardingChannelOutput<FakeChannelOutputImpl,
                                     kOutputSize,
                                     kMaxPackets,
                                     kPayloadsBufferSizeBytes> {
 private:
  using Base = ForwardingChannelOutput<FakeChannelOutputImpl,
                                       kOutputSize,
                                       kMaxPackets,
                                       kPayloadsBufferSizeBytes>;

 public:
  size_t MaximumTransmissionUnit() PW_LOCKS_EXCLUDED(mutex_) override {
    std::lock_guard lock(mutex_);
    return Base::MaximumTransmissionUnit();
  }

  Status Send(span<const std::byte> buffer) PW_LOCKS_EXCLUDED(mutex_) override {
    Status status;
    mutex_.lock();
    status = Base::Send(buffer);
    mutex_.unlock();
    output_semaphore_.release();
    return status;
  }

  // Returns true if should continue waiting for additional output
  bool WaitForOutput() PW_LOCKS_EXCLUDED(mutex_) {
    output_semaphore_.acquire();
    std::lock_guard lock(mutex_);
    return should_wait_;
  }

  void StopWaitingForOutput() PW_LOCKS_EXCLUDED(mutex_) {
    std::lock_guard lock(mutex_);
    should_wait_ = false;
    output_semaphore_.release();
  }

 protected:
  explicit WatchableChannelOutput(
      TestPacketProcessor&& server_packet_processor = nullptr,
      TestPacketProcessor&& client_packet_processor = nullptr)
      : Base(std::move(server_packet_processor),
             std::move(client_packet_processor)) {}

  size_t PacketCount() const PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_) override {
    return Base::PacketCount();
  }

  sync::Mutex mutex_;

 private:
  Result<ConstByteSpan> EncodeNextUnsentPacket(
      std::array<std::byte, kPayloadsBufferSizeBytes>& packet_buffer)
      PW_LOCKS_EXCLUDED(mutex_) override {
    std::lock_guard lock(mutex_);
    return Base::EncodeNextUnsentPacket(packet_buffer);
  }
  sync::BinarySemaphore output_semaphore_;
  bool should_wait_ PW_GUARDED_BY(mutex_) = true;
};

// Provides a testing context with a real client and server
template <typename WatchableChannelOutputImpl,
          size_t kOutputSize = 128,
          size_t kMaxPackets = 16,
          size_t kPayloadsBufferSizeBytes = 128>
class ClientServerTestContextThreaded
    : public ClientServerTestContext<WatchableChannelOutputImpl,
                                     kOutputSize,
                                     kMaxPackets,
                                     kPayloadsBufferSizeBytes> {
 private:
  using Instance = ClientServerTestContextThreaded<WatchableChannelOutputImpl,
                                                   kOutputSize,
                                                   kMaxPackets,
                                                   kPayloadsBufferSizeBytes>;
  using Base = ClientServerTestContext<WatchableChannelOutputImpl,
                                       kOutputSize,
                                       kMaxPackets,
                                       kPayloadsBufferSizeBytes>;

 public:
  ~ClientServerTestContextThreaded() {
    Base::channel_output_.StopWaitingForOutput();
    thread_.join();
  }

 protected:
  explicit ClientServerTestContextThreaded(
      const thread::Options& options,
      TestPacketProcessor&& server_packet_processor = nullptr,
      TestPacketProcessor&& client_packet_processor = nullptr)
      : Base(std::move(server_packet_processor),
             std::move(client_packet_processor)),
        thread_(options, [this] { Run(); }) {}

 private:
  using Base::ForwardNewPackets;
  void Run() {
    auto& ctx = *static_cast<Instance*>(this);
    while (ctx.channel_output_.WaitForOutput()) {
      ctx.ForwardNewPackets();
    }
  }
  Thread thread_;
};

}  // namespace internal
}  // namespace pw::rpc
