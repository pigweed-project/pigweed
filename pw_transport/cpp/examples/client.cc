// Copyright 2026 The Pigweed Authors
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

// Example client for the framed TCP transport. Sends several messages to the
// server and waits for a reply to each one.
//
// Usage: client [port]
//
// The client connects to port 8000 on 127.0.0.1 by default. It exits with a
// nonzero status if any message is not answered.

#define PW_LOG_MODULE_NAME "TRANSPORT_CLIENT"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

#include "pw_allocator/allocator.h"
#include "pw_allocator/libc_allocator.h"
#include "pw_async2/await.h"
#include "pw_async2/basic_dispatcher.h"
#include "pw_async2/system_time_provider.h"
#include "pw_async2/task.h"
#include "pw_async2/time_provider.h"
#include "pw_buf/buf.h"
#include "pw_chrono/system_clock.h"
#include "pw_log/log.h"
#include "pw_result/result.h"
#include "pw_string/string_builder.h"
#include "pw_transport/framed_tcp.h"
#include "pw_transport/socket.h"
#include "pw_transport/transport.h"

namespace pw::transport::examples {
namespace {

constexpr const char* kHost = "127.0.0.1";
constexpr uint16_t kDefaultPort = 8000;
constexpr int kMessageCount = 5;
constexpr std::chrono::milliseconds kMessageInterval(50);

// Parses a port number. Returns `std::nullopt` if `arg` is not a valid port.
std::optional<uint16_t> ParsePort(std::string_view arg) {
  uint16_t port = 0;
  const std::from_chars_result result =
      std::from_chars(arg.data(), arg.data() + arg.size(), port);
  if (result.ec != std::errc() || result.ptr != arg.data() + arg.size()) {
    return std::nullopt;
  }
  return port;
}

// Connects to the server, sends messages, and waits for a reply to each one.
class ClientTask : public async2::Task {
 public:
  ClientTask(ReliableDatagramConnector& connector,
             async2::TimeProvider<chrono::SystemClock>& time,
             int message_count)
      : connector_(connector), time_(time), message_count_(message_count) {}

  // Returns whether every message was sent and answered.
  bool succeeded() const { return succeeded_; }

 private:
  enum class State {
    kConnecting,
    kSending,
    kReceivingReply,
    kWaiting,
    kClosing,
  };

  async2::Poll<> DoPend(async2::Context& cx) override {
    while (true) {
      switch (state_) {
        case State::kConnecting: {
          if (!connect_future_.is_pendable()) {
            connect_future_ = connector_.Connect();
          }
          PW_AWAIT(Result<ReliableDatagramSocket> socket, connect_future_, cx);
          if (!socket.ok()) {
            PW_LOG_ERROR("Failed to connect: %s", socket.status().str());
            return async2::Ready();
          }
          socket_ = std::move(*socket);
          PW_LOG_INFO("Connected to server");
          StartSending();
          break;
        }

        case State::kSending: {
          PW_AWAIT(
              std::optional<WriteReservation> reservation, reserve_future_, cx);
          if (!reservation.has_value()) {
            PW_LOG_ERROR("Connection closed before message #%d was sent",
                         messages_sent_ + 1);
            return async2::Ready();
          }
          std::memcpy(reservation->data(), message_.data(), message_.size());
          if (!reservation->Commit(message_.size())) {
            PW_LOG_ERROR("Connection closed before message #%d was sent",
                         messages_sent_ + 1);
            return async2::Ready();
          }
          ++messages_sent_;
          PW_LOG_INFO("Sent message #%d", messages_sent_);
          read_future_ = socket_.Read();
          state_ = State::kReceivingReply;
          break;
        }

        case State::kReceivingReply: {
          PW_AWAIT(ConstBuf reply, read_future_, cx);
          if (reply == nullptr) {
            PW_LOG_ERROR("Server closed the connection");
            return async2::Ready();
          }
          PW_LOG_INFO("Received reply: %.*s",
                      static_cast<int>(reply.size()),
                      reinterpret_cast<const char*>(reply.data()));

          if (messages_sent_ < message_count_) {
            delay_future_ = time_.WaitFor(
                chrono::SystemClock::for_at_least(kMessageInterval));
            state_ = State::kWaiting;
          } else {
            close_future_ = socket_.Close();
            state_ = State::kClosing;
          }
          break;
        }

        case State::kWaiting: {
          PW_AWAIT(delay_future_, cx);
          StartSending();
          break;
        }

        case State::kClosing: {
          PW_AWAIT(close_future_, cx);
          PW_LOG_INFO("Closed connection");
          succeeded_ = true;
          return async2::Ready();
        }
      }
    }
  }

  // Prepares the next message and starts reserving space for it.
  void StartSending() {
    message_.clear();
    message_.Format("Hello from client! Message #%d", messages_sent_ + 1);
    reserve_future_ = socket_.ReserveWrite(message_.size());
    state_ = State::kSending;
  }

  ReliableDatagramConnector& connector_;
  async2::TimeProvider<chrono::SystemClock>& time_;
  const int message_count_;

  State state_ = State::kConnecting;
  ReliableDatagramSocket socket_;
  int messages_sent_ = 0;
  bool succeeded_ = false;
  StringBuffer<64> message_;

  ReliableDatagramConnector::ConnectFuture connect_future_;
  ReserveWriteFuture reserve_future_;
  ReadFuture read_future_;
  async2::TimeFuture<chrono::SystemClock> delay_future_;
  CloseFuture close_future_;
};

}  // namespace
}  // namespace pw::transport::examples

int main(int argc, char* argv[]) {
  // Print logs immediately, even when stdout is a pipe.
  std::setbuf(stdout, nullptr);

  uint16_t port = pw::transport::examples::kDefaultPort;
  if (argc > 2) {
    PW_LOG_ERROR("Usage: %s [port]", argv[0]);
    return 1;
  }
  if (argc == 2) {
    std::optional<uint16_t> parsed =
        pw::transport::examples::ParsePort(argv[1]);
    if (!parsed.has_value() || *parsed == 0) {
      PW_LOG_ERROR("Invalid port: %s", argv[1]);
      return 1;
    }
    port = *parsed;
  }

  pw::Allocator& allocator = pw::allocator::GetLibCAllocator();
  pw::transport::FramedTcpConnector connector(
      allocator, pw::transport::examples::kHost, port);

  pw::async2::BasicDispatcher dispatcher;
  pw::transport::examples::ClientTask task(
      connector,
      pw::async2::GetSystemTimeProvider(),
      pw::transport::examples::kMessageCount);
  dispatcher.Post(task);
  dispatcher.RunToCompletion();
  return task.succeeded() ? 0 : 1;
}
