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

// Echo client that makes its RPCs from futures and a task, without coroutines.
//
// Usage: client_polling [port]
//
// Connects to port 8000 on 127.0.0.1 by default. Exits with a nonzero status if
// any RPC fails.

#define PW_LOG_MODULE_NAME "ECHO_CLIENT"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>

#include "echo_pb/echo.pwpb.rpc2.h"
#include "pw_allocator/allocator.h"
#include "pw_allocator/libc_allocator.h"
#include "pw_assert/check.h"
#include "pw_async2/await.h"
#include "pw_async2/basic_dispatcher.h"
#include "pw_async2/context.h"
#include "pw_async2/dispatcher.h"
#include "pw_async2/poll.h"
#include "pw_async2/task.h"
#include "pw_log/log.h"
#include "pw_result/result.h"
#include "pw_rpc2/client.h"
#include "pw_rpc2/reader.h"
#include "pw_rpc2/service_client.h"
#include "pw_rpc2/writer.h"
#include "pw_status/status.h"
#include "pw_transport/framed_tcp.h"
#include "pw_transport/socket.h"

using pw::async2::BasicDispatcher;
using pw::async2::Context;
using pw::async2::Dispatcher;
using pw::async2::Poll;
using pw::async2::Ready;
using pw::async2::Task;

namespace {

// DOCSTAG: [pw_rpc2-examples-echo-client-polling-repeat]
using EchoMessage = examples::pwpb::EchoMessage::Message;
using RepeatRequest = examples::pwpb::RepeatRequest::Message;

// `pw::rpc2::Client` is a connection to a server, and the generated `Echo`
// client binds a `pw::rpc2::Client` to the `Echo` service. Both are cheap to
// copy, so pass them by value.
using EchoClient = examples::pw_rpc2::pwpb::Echo::Client;

// Server streaming RPC: sends one request, then reads responses until the
// server finishes its stream.
class RepeatCall {
 public:
  using value_type = pw::Status;

  RepeatCall() = default;
  explicit RepeatCall(EchoClient client)
      : state_(State::kCalling), client_(std::move(client)) {}

  bool is_pendable() const {
    return state_ != State::kUninitialized && state_ != State::kDone;
  }
  bool is_complete() const { return state_ == State::kDone; }

  Poll<pw::Status> Pend(Context& cx) {
    while (true) {
      switch (state_) {
        case State::kCalling: {
          if (!call_future_.is_pendable()) {
            call_future_ = client_.Repeat({.msg = "ping", .count = 3});
          }
          PW_AWAIT(pw::Result<pw::rpc2::Reader<EchoMessage>> reader,
                   call_future_,
                   cx);
          if (!reader.ok()) {
            return Complete(reader.status());
          }
          reader_ = std::move(*reader);
          state_ = State::kReading;
          break;
        }

        case State::kReading: {
          if (!read_future_.is_pendable()) {
            read_future_ = reader_.Read();
          }
          PW_AWAIT(pw::Result<EchoMessage> response, read_future_, cx);
          if (response.status().IsOutOfRange()) {
            return Complete(pw::OkStatus());  // The server finished its stream.
          }
          if (!response.ok()) {
            return Complete(response.status());
          }
          PW_LOG_INFO("Repeat: %s", response->msg.c_str());
          break;
        }

        case State::kUninitialized:
        case State::kDone:
          PW_CRASH("Polled a future that is not pendable");
      }
    }
  }

 private:
  enum class State { kUninitialized, kCalling, kReading, kDone };

  Poll<pw::Status> Complete(pw::Status status) {
    state_ = State::kDone;
    return Ready(status);
  }

  State state_ = State::kUninitialized;
  EchoClient client_;
  pw::rpc2::ServerStreamFuture<RepeatRequest, EchoMessage> call_future_;
  pw::rpc2::Reader<EchoMessage> reader_;
  pw::rpc2::ReadFuture<EchoMessage> read_future_;
};
// DOCSTAG: [pw_rpc2-examples-echo-client-polling-repeat]

// Client streaming RPC: streams requests, then receives one response.
class CollectCall {
 public:
  using value_type = pw::Status;

  CollectCall() = default;
  explicit CollectCall(EchoClient client)
      : state_(State::kCalling), client_(std::move(client)) {}

  bool is_pendable() const {
    return state_ != State::kUninitialized && state_ != State::kDone;
  }
  bool is_complete() const { return state_ == State::kDone; }

  Poll<pw::Status> Pend(Context& cx) {
    while (true) {
      switch (state_) {
        case State::kCalling: {
          if (!call_future_.is_pendable()) {
            call_future_ = client_.Collect();
          }
          PW_AWAIT(auto call, call_future_, cx);
          if (!call.ok()) {
            return Complete(call.status());
          }
          call_ = std::move(*call);
          state_ = State::kWriting;
          break;
        }

        case State::kWriting: {
          if (words_sent_ == kWords.size()) {
            state_ = State::kFinishing;
            break;
          }
          if (!write_future_.is_pendable()) {
            write_future_ = call_.writer().Write({.msg = kWords[words_sent_]});
          }
          PW_AWAIT(pw::Status status, write_future_, cx);
          if (!status.ok()) {
            return Complete(status);
          }
          ++words_sent_;
          break;
        }

        case State::kFinishing: {
          if (!finish_future_.is_pendable()) {
            finish_future_ = call_.writer().Finish();
          }
          PW_AWAIT(pw::Status status, finish_future_, cx);
          if (!status.ok()) {
            return Complete(status);
          }
          state_ = State::kReceiving;
          break;
        }

        case State::kReceiving: {
          PW_AWAIT(pw::Result<EchoMessage> response, call_.response(), cx);
          if (!response.ok()) {
            return Complete(response.status());
          }
          PW_LOG_INFO("Collect: %s", response->msg.c_str());
          return Complete(pw::OkStatus());
        }

        case State::kUninitialized:
        case State::kDone:
          PW_CRASH("Polled a future that is not pendable");
      }
    }
  }

 private:
  enum class State {
    kUninitialized,
    kCalling,
    kWriting,
    kFinishing,
    kReceiving,
    kDone,
  };

  static constexpr std::array<std::string_view, 3> kWords = {
      "one", "two", "three"};

  Poll<pw::Status> Complete(pw::Status status) {
    state_ = State::kDone;
    return Ready(status);
  }

  State state_ = State::kUninitialized;
  EchoClient client_;
  size_t words_sent_ = 0;
  pw::rpc2::ClientStreamFuture<EchoMessage, EchoMessage> call_future_;
  pw::rpc2::ClientStreamCall<EchoMessage, EchoMessage> call_;
  pw::rpc2::WriteFuture<EchoMessage> write_future_;
  pw::rpc2::WriteFuture<> finish_future_;
};

// Bidirectional streaming RPC: streams requests and reads the server's stream
// of responses. The writer and reader are independent; they could also be used
// from separate tasks.
class EchoStreamCall {
 public:
  using value_type = pw::Status;

  EchoStreamCall() = default;
  explicit EchoStreamCall(EchoClient client)
      : state_(State::kCalling), client_(std::move(client)) {}

  bool is_pendable() const {
    return state_ != State::kUninitialized && state_ != State::kDone;
  }
  bool is_complete() const { return state_ == State::kDone; }

  Poll<pw::Status> Pend(Context& cx) {
    while (true) {
      switch (state_) {
        case State::kCalling: {
          if (!call_future_.is_pendable()) {
            call_future_ = client_.EchoStream();
          }
          PW_AWAIT(auto call, call_future_, cx);
          if (!call.ok()) {
            return Complete(call.status());
          }
          call_ = std::move(*call);
          state_ = State::kWriting;
          break;
        }

        case State::kWriting: {
          if (words_sent_ == kWords.size()) {
            state_ = State::kFinishing;
            break;
          }
          if (!write_future_.is_pendable()) {
            write_future_ = call_.writer().Write({.msg = kWords[words_sent_]});
          }
          PW_AWAIT(pw::Status status, write_future_, cx);
          if (!status.ok()) {
            return Complete(status);
          }
          ++words_sent_;
          break;
        }

        case State::kFinishing: {
          if (!finish_future_.is_pendable()) {
            finish_future_ = call_.writer().Finish();
          }
          PW_AWAIT(pw::Status status, finish_future_, cx);
          if (!status.ok()) {
            return Complete(status);
          }
          state_ = State::kReading;
          break;
        }

        case State::kReading: {
          if (!read_future_.is_pendable()) {
            read_future_ = call_.reader().Read();
          }
          PW_AWAIT(pw::Result<EchoMessage> response, read_future_, cx);
          if (response.status().IsOutOfRange()) {
            return Complete(pw::OkStatus());  // The server finished its stream.
          }
          if (!response.ok()) {
            return Complete(response.status());
          }
          PW_LOG_INFO("EchoStream: %s", response->msg.c_str());
          break;
        }

        case State::kUninitialized:
        case State::kDone:
          PW_CRASH("Polled a future that is not pendable");
      }
    }
  }

 private:
  enum class State {
    kUninitialized,
    kCalling,
    kWriting,
    kFinishing,
    kReading,
    kDone,
  };

  static constexpr std::array<std::string_view, 2> kWords = {"alpha", "beta"};

  Poll<pw::Status> Complete(pw::Status status) {
    state_ = State::kDone;
    return Ready(status);
  }

  State state_ = State::kUninitialized;
  EchoClient client_;
  size_t words_sent_ = 0;
  pw::rpc2::BidiStreamFuture<EchoMessage, EchoMessage> call_future_;
  pw::rpc2::BidiStreamCall<EchoMessage, EchoMessage> call_;
  pw::rpc2::WriteFuture<EchoMessage> write_future_;
  pw::rpc2::WriteFuture<> finish_future_;
  pw::rpc2::ReadFuture<EchoMessage> read_future_;
};

// DOCSTAG: [pw_rpc2-examples-echo-client-polling-task]
// Connects to the server, makes each type of RPC, and closes the connection.
class ClientTask : public Task {
 public:
  ClientTask(Dispatcher& dispatcher,
             pw::Allocator& allocator,
             pw::transport::ReliableDatagramConnector& connector)
      : dispatcher_(dispatcher), allocator_(allocator), connector_(connector) {}

  // Returns the result of the RPCs once the task has completed.
  pw::Status status() const { return status_; }

 private:
  enum class State {
    kConnecting,
    kEcho,
    kRepeat,
    kCollect,
    kEchoStream,
    kClosing,
  };

  Poll<> DoPend(Context& cx) override {
    while (true) {
      switch (state_) {
        case State::kConnecting: {
          if (!connect_future_.is_pendable()) {
            connect_future_ =
                pw::rpc2::Client::Connect(dispatcher_, allocator_, connector_);
          }
          PW_AWAIT(pw::Result<pw::rpc2::Client> client, connect_future_, cx);
          if (!client.ok()) {
            status_ = client.status();
            return Ready();
          }
          client_ = std::move(*client);
          echo_client_ = EchoClient(client_);
          state_ = State::kEcho;
          break;
        }

        // Unary RPC: sends one request and receives one response.
        case State::kEcho: {
          if (!echo_future_.is_pendable()) {
            echo_future_ = echo_client_.Echo({.msg = "hello"});
          }
          PW_AWAIT(pw::Result<EchoMessage> response, echo_future_, cx);
          if (response.ok()) {
            PW_LOG_INFO("Echo: %s", response->msg.c_str());
          }
          status_ = response.status();
          state_ = status_.ok() ? State::kRepeat : State::kClosing;
          break;
        }

        case State::kRepeat: {
          if (!repeat_.is_pendable()) {
            repeat_ = RepeatCall(echo_client_);
          }
          PW_AWAIT(status_, repeat_, cx);
          state_ = status_.ok() ? State::kCollect : State::kClosing;
          break;
        }

        case State::kCollect: {
          if (!collect_.is_pendable()) {
            collect_ = CollectCall(echo_client_);
          }
          PW_AWAIT(status_, collect_, cx);
          state_ = status_.ok() ? State::kEchoStream : State::kClosing;
          break;
        }

        case State::kEchoStream: {
          if (!echo_stream_.is_pendable()) {
            echo_stream_ = EchoStreamCall(echo_client_);
          }
          PW_AWAIT(status_, echo_stream_, cx);
          state_ = State::kClosing;
          break;
        }

        case State::kClosing: {
          if (!close_future_.is_pendable()) {
            close_future_ = client_.Close();
          }
          PW_AWAIT(pw::Status close_status, close_future_, cx);
          status_.Update(close_status);
          return Ready();
        }
      }
    }
  }

  Dispatcher& dispatcher_;
  pw::Allocator& allocator_;
  pw::transport::ReliableDatagramConnector& connector_;

  State state_ = State::kConnecting;
  pw::Status status_;
  pw::rpc2::Client client_;
  EchoClient echo_client_;

  pw::rpc2::ClientFuture connect_future_;
  pw::rpc2::UnaryFuture<EchoMessage, EchoMessage> echo_future_;
  RepeatCall repeat_;
  CollectCall collect_;
  EchoStreamCall echo_stream_;
  pw::rpc2::ControlFuture close_future_;
};
// DOCSTAG: [pw_rpc2-examples-echo-client-polling-task]

}  // namespace

int main(int argc, char* argv[]) {
  uint16_t port = 8000;
  if (argc > 1) {
    std::from_chars(argv[1], argv[1] + std::strlen(argv[1]), port);
  }

  pw::Allocator& allocator = pw::allocator::GetLibCAllocator();
  BasicDispatcher dispatcher;
  pw::transport::FramedTcpConnector connector(allocator, "127.0.0.1", port);

  ClientTask task(dispatcher, allocator, connector);
  dispatcher.Post(task);
  dispatcher.RunToCompletion();
  return task.status().ok() ? 0 : 1;
}
