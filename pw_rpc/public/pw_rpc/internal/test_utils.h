// Copyright 2021 The Pigweed Authors
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

// Internal-only testing utilities. public/pw_rpc/test_method_context.h provides
// improved public-facing utilities for testing RPC services.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "pw_assert/assert.h"
#include "pw_rpc/channel.h"
#include "pw_rpc/client.h"
#include "pw_rpc/internal/lock.h"
#include "pw_rpc/internal/method.h"
#include "pw_rpc/internal/packet.h"
#include "pw_rpc/raw/fake_channel_output.h"
#include "pw_rpc/server.h"
#include "pw_span/span.h"
#include "pw_unit_test/framework.h"

namespace pw::rpc::internal {

// Callback capture that acquires rpc_lock() in its destructor and records that
// it was destroyed. If pw_rpc destroys a callback that holds this capture while
// rpc_lock() is held, the test self-deadlocks and fails with a timeout rather
// than an assertion failure.
class LockAcquiringCapture {
 public:
  explicit constexpr LockAcquiringCapture(bool& destroyed)
      : destroyed_(&destroyed) {}

  LockAcquiringCapture(LockAcquiringCapture&& other) noexcept
      : destroyed_(std::exchange(other.destroyed_, nullptr)) {}

  LockAcquiringCapture& operator=(LockAcquiringCapture&& other) noexcept {
    if (this != &other) {
      Reset();
      destroyed_ = std::exchange(other.destroyed_, nullptr);
    }
    return *this;
  }

  ~LockAcquiringCapture() { Reset(); }

 private:
  void Reset() {
    if (destroyed_ != nullptr) {
      RpcLockGuard lock;
      *destroyed_ = true;
      destroyed_ = nullptr;
    }
  }

  bool* destroyed_;
};

// Owns a call object and records when it is destroyed. A call object acquires
// rpc_lock() when it is destroyed, so if pw_rpc destroys a callback that
// captures a CallOwner while holding rpc_lock(), the test deadlocks. If pw_rpc
// destroys the capture while the owned call's callback is still running, the
// call's destructor waits for its own callback and crashes.
template <typename CallType>
struct CallOwner {
  CallType call;
  bool* destroyed = nullptr;

  ~CallOwner() {
    if (destroyed != nullptr) {
      *destroyed = true;
    }
  }
};

// Version of the Server with extra methods exposed for testing.
class TestServer : public Server {
 public:
  using Server::calls_end;
  using Server::CloseCallAndMarkForCleanup;
  using Server::FindCall;
};

template <typename Service,
          uint32_t kChannelId = 99,
          uint32_t kServiceId = 16,
          uint32_t kDefaultCallId = 437>
class ServerContextForTest {
 public:
  static constexpr uint32_t channel_id() { return kChannelId; }
  static constexpr uint32_t service_id() { return kServiceId; }

  ServerContextForTest(const internal::Method& method)
      : channel_(Channel::Create<kChannelId>(&output_)),
        server_(span(&channel_, 1)),
        service_(kServiceId),
        context_(server_, channel_.id(), service_, method, kDefaultCallId) {
    server_.RegisterService(service_);
  }

  // Create packets for this context's channel, service, and method.
  internal::Packet request(span<const std::byte> payload) const {
    return internal::Packet(internal::pwpb::PacketType::REQUEST,
                            kChannelId,
                            kServiceId,
                            context_.method().id(),
                            kDefaultCallId,
                            payload);
  }

  internal::Packet response(Status status,
                            span<const std::byte> payload = {}) const {
    return internal::Packet(internal::pwpb::PacketType::RESPONSE,
                            kChannelId,
                            kServiceId,
                            context_.method().id(),
                            kDefaultCallId,
                            payload,
                            status);
  }

  internal::Packet server_stream(span<const std::byte> payload) const {
    return internal::Packet(internal::pwpb::PacketType::SERVER_STREAM,
                            kChannelId,
                            kServiceId,
                            context_.method().id(),
                            kDefaultCallId,
                            payload);
  }

  internal::Packet client_stream(span<const std::byte> payload) const {
    return internal::Packet(internal::pwpb::PacketType::CLIENT_STREAM,
                            kChannelId,
                            kServiceId,
                            context_.method().id(),
                            kDefaultCallId,
                            payload);
  }

  CallContext get(uint32_t id = kDefaultCallId) const {
    return CallContext(context_.server(),
                       context_.channel_id(),
                       context_.service(),
                       context_.method(),
                       id);
  }

  internal::test::FakeChannelOutput& output() { return output_; }
  TestServer& server() { return static_cast<TestServer&>(server_); }
  Service& service() { return service_; }

 private:
  RawFakeChannelOutput<5> output_;
  rpc::Channel channel_;
  rpc::Server server_;
  Service service_;

  const internal::CallContext context_;
};

template <size_t kInputBufferSize = 128,
          uint32_t kChannelId = 99,
          uint32_t kServiceId = 16,
          uint32_t kMethodId = 111>
class ClientContextForTest {
 public:
  static constexpr uint32_t channel_id() { return kChannelId; }
  static constexpr uint32_t service_id() { return kServiceId; }
  static constexpr uint32_t method_id() { return kMethodId; }

  ClientContextForTest()
      : channel_(Channel::Create<kChannelId>(&output_)),
        client_(span(&channel_, 1)) {}

  const internal::test::FakeChannelOutput& output() const { return output_; }
  Channel& channel() { return static_cast<Channel&>(channel_); }
  Client& client() { return client_; }

  // Sends a packet to be processed by the client. Returns the client's
  // ProcessPacket status.
  Status SendPacket(internal::pwpb::PacketType type,
                    Status status = OkStatus(),
                    span<const std::byte> payload = {}) {
    uint32_t call_id =
        output().total_packets() > 0 ? output().last_packet().call_id() : 0;

    internal::Packet packet(
        type, kChannelId, kServiceId, kMethodId, call_id, payload, status);
    std::byte buffer[kInputBufferSize];
    Result result = packet.Encode(buffer);
    EXPECT_EQ(result.status(), OkStatus());
    return client_.ProcessPacket(result.value_or(ConstByteSpan()));
  }

  Status SendResponse(Status status, span<const std::byte> payload = {}) {
    return SendPacket(internal::pwpb::PacketType::RESPONSE, status, payload);
  }

  Status SendServerStream(span<const std::byte> payload) {
    return SendPacket(
        internal::pwpb::PacketType::SERVER_STREAM, OkStatus(), payload);
  }

 private:
  RawFakeChannelOutput<5> output_;
  rpc::Channel channel_;
  Client client_;
};

}  // namespace pw::rpc::internal
