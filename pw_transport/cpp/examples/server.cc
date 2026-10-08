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

// Example server for the framed TCP transport. Replies to every datagram that
// a client sends.
//
// Usage: server [port]
//
// The server listens on port 8000 by default. Port 0 picks an available port.

#define PW_LOG_MODULE_NAME "TRANSPORT_SERVER"

#include <charconv>
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
#include "pw_async2/dispatcher.h"
#include "pw_async2/task.h"
#include "pw_buf/buf.h"
#include "pw_log/log.h"
#include "pw_result/result.h"
#include "pw_string/string_builder.h"
#include "pw_transport/framed_tcp.h"
#include "pw_transport/socket.h"
#include "pw_transport/transport.h"

namespace pw::transport::examples {
namespace {

constexpr uint16_t kDefaultPort = 8000;

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

// Replies to each datagram received on one connection.
class ConnectionTask : public async2::Task {
 public:
  ConnectionTask(int id, ReliableDatagramSocket socket)
      : id_(id), socket_(std::move(socket)) {}

 private:
  enum class State {
    kReceiving,
    kReplying,
  };

  async2::Poll<> DoPend(async2::Context& cx) override {
    while (true) {
      if (state_ == State::kReceiving) {
        if (!read_future_.is_pendable()) {
          read_future_ = socket_.Read();
        }
        PW_AWAIT(ConstBuf request, read_future_, cx);
        if (request == nullptr) {
          PW_LOG_INFO("[Conn #%d] Connection closed", id_);
          return async2::Ready();
        }

        ++requests_received_;
        PW_LOG_INFO("[Conn #%d] Received %zu bytes: %.*s",
                    id_,
                    request.size(),
                    static_cast<int>(request.size()),
                    reinterpret_cast<const char*>(request.data()));

        reply_.clear();
        reply_.Format("Server ACK for message #%d", requests_received_);
        reserve_future_ = socket_.ReserveWrite(reply_.size());
        state_ = State::kReplying;
      }

      PW_AWAIT(
          std::optional<WriteReservation> reservation, reserve_future_, cx);
      if (!reservation.has_value()) {
        PW_LOG_INFO("[Conn #%d] Connection closed before reply was sent", id_);
        return async2::Ready();
      }
      std::memcpy(reservation->data(), reply_.data(), reply_.size());
      if (!reservation->Commit(reply_.size())) {
        PW_LOG_INFO("[Conn #%d] Connection closed before reply was sent", id_);
        return async2::Ready();
      }
      PW_LOG_INFO("[Conn #%d] Sent reply #%d", id_, requests_received_);
      state_ = State::kReceiving;
    }
  }

  const int id_;
  ReliableDatagramSocket socket_;
  State state_ = State::kReceiving;
  int requests_received_ = 0;
  StringBuffer<64> reply_;
  ReadFuture read_future_;
  ReserveWriteFuture reserve_future_;
};

// Accepts connections and posts a `ConnectionTask` for each one.
class AcceptTask : public async2::Task {
 public:
  AcceptTask(Allocator& allocator,
             async2::Dispatcher& dispatcher,
             ReliableDatagramListener& listener)
      : allocator_(allocator), dispatcher_(dispatcher), listener_(listener) {}

 private:
  async2::Poll<> DoPend(async2::Context& cx) override {
    while (true) {
      if (!accept_future_.is_pendable()) {
        accept_future_ = listener_.Accept();
      }
      PW_AWAIT(Result<ReliableDatagramSocket> socket, accept_future_, cx);
      if (socket.status().IsResourceExhausted()) {
        PW_LOG_WARN("Failed to accept a connection; continuing");
        continue;
      }
      if (!socket.ok()) {
        PW_LOG_ERROR("Stopped accepting connections: %s",
                     socket.status().str());
        return async2::Ready();
      }

      const int id = next_connection_id_++;
      PW_LOG_INFO("[Conn #%d] Accepted connection", id);
      auto task = allocator_.MakeShared<ConnectionTask>(id, std::move(*socket));
      if (task == nullptr) {
        PW_LOG_ERROR("[Conn #%d] Failed to allocate connection task", id);
        continue;
      }
      dispatcher_.PostShared(task);
    }
  }

  Allocator& allocator_;
  async2::Dispatcher& dispatcher_;
  ReliableDatagramListener& listener_;
  ReliableDatagramListener::AcceptFuture accept_future_;
  int next_connection_id_ = 1;
};

}  // namespace
}  // namespace pw::transport::examples

int main(int argc, char* argv[]) {
  // Print logs immediately, even when stdout is a pipe. run_harness.py waits
  // for the "Listening on port" message.
  std::setbuf(stdout, nullptr);

  uint16_t port = pw::transport::examples::kDefaultPort;
  if (argc > 2) {
    PW_LOG_ERROR("Usage: %s [port]", argv[0]);
    return 1;
  }
  if (argc == 2) {
    std::optional<uint16_t> parsed =
        pw::transport::examples::ParsePort(argv[1]);
    if (!parsed.has_value()) {
      PW_LOG_ERROR("Invalid port: %s", argv[1]);
      return 1;
    }
    port = *parsed;
  }

  pw::Allocator& allocator = pw::allocator::GetLibCAllocator();
  pw::transport::FramedTcpListener listener(allocator);
  if (!listener.Listen(port).ok()) {
    return 1;  // Listen() logs the reason.
  }
  // LINT.IfChange(listening_log)
  PW_LOG_INFO("Listening on port %u", static_cast<unsigned>(listener.port()));
  // LINT.ThenChange(//pw_transport/cpp/examples/run_harness.py:listening_log)

  pw::async2::BasicDispatcher dispatcher;
  pw::transport::examples::AcceptTask accept_task(
      allocator, dispatcher, listener);
  dispatcher.Post(accept_task);
  dispatcher.RunToCompletion();
  return 0;
}
