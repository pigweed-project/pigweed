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

// Echo server that implements its RPCs as futures, without coroutines.
//
// Usage: server_polling [port]
//
// Listens on port 8000 by default. Port 0 picks an available port.

#define PW_LOG_MODULE_NAME "ECHO_SERVER"

#include <charconv>
#include <cstdint>
#include <cstring>
#include <utility>

#include "echo_pb/echo.pwpb.rpc2.h"
#include "pw_allocator/allocator.h"
#include "pw_allocator/libc_allocator.h"
#include "pw_assert/check.h"
#include "pw_async2/await.h"
#include "pw_async2/basic_dispatcher.h"
#include "pw_async2/context.h"
#include "pw_async2/poll.h"
#include "pw_log/log.h"
#include "pw_result/result.h"
#include "pw_rpc2/reader.h"
#include "pw_rpc2/server.h"
#include "pw_rpc2/writer.h"
#include "pw_status/status.h"
#include "pw_string/util.h"
#include "pw_transport/framed_tcp.h"

using pw::async2::BasicDispatcher;
using pw::async2::Context;
using pw::async2::Poll;
using pw::async2::Ready;

namespace {

// DOCSTAG: [pw_rpc2-examples-echo-server-polling-unary]
using EchoMessage = examples::pwpb::EchoMessage::Message;
using RepeatRequest = examples::pwpb::RepeatRequest::Message;

// Implements each RPC with a nested `<Method>Future` type. For each call,
// pw_rpc2 constructs the future from the RPC's arguments and polls it until it
// completes.
//
// `<Method>Future` constructors can be declared in two ways, depending on
// whether the future needs a reference to the service instance:
// - `<Method>Future(EchoService& service, <RPC arguments>...)`
// - `<Method>Future(<RPC arguments>...)`
class EchoService : public examples::pw_rpc2::pwpb::Echo::Service<EchoService> {
 public:
  class EchoFuture {
   public:
    using value_type = void;

    EchoFuture() = default;
    EchoFuture(EchoMessage request,
               pw::rpc2::UnaryWriter<EchoMessage> responder)
        : state_(State::kStarting),
          request_(std::move(request)),
          responder_(std::move(responder)) {}

    bool is_pendable() const {
      return state_ != State::kUninitialized && state_ != State::kDone;
    }
    bool is_complete() const { return state_ == State::kDone; }

    Poll<> Pend(Context& cx) {
      if (state_ == State::kStarting) {
        PW_LOG_INFO("Echo: %s", request_.msg.c_str());
        response_future_ = responder_.Finish(std::move(request_));
        state_ = State::kResponding;
      }
      PW_AWAIT(response_future_, cx);
      state_ = State::kDone;
      return Ready();
    }

   private:
    enum class State { kUninitialized, kStarting, kResponding, kDone };

    State state_ = State::kUninitialized;
    EchoMessage request_;
    pw::rpc2::UnaryWriter<EchoMessage> responder_;
    pw::rpc2::WriteFuture<EchoMessage> response_future_;
  };
  // DOCSTAG: [pw_rpc2-examples-echo-server-polling-unary]

  class RepeatFuture {
   public:
    using value_type = void;

    RepeatFuture() = default;
    RepeatFuture(RepeatRequest request, pw::rpc2::Writer<EchoMessage> writer)
        : state_(State::kStarting),
          response_{.msg = request.msg},
          count_(request.count),
          writer_(std::move(writer)) {}

    bool is_pendable() const {
      return state_ != State::kUninitialized && state_ != State::kDone;
    }
    bool is_complete() const { return state_ == State::kDone; }

    Poll<> Pend(Context& cx) {
      while (true) {
        switch (state_) {
          case State::kStarting:
            PW_LOG_INFO("Repeat: %s x%u", response_.msg.c_str(), count_);
            state_ = State::kWriting;
            break;

          case State::kWriting: {
            if (sent_ == count_) {
              state_ = State::kFinishing;
              break;
            }
            if (!write_future_.is_pendable()) {
              write_future_ = writer_.Write(response_);
            }
            PW_AWAIT(pw::Status status, write_future_, cx);
            if (!status.ok()) {
              state_ = State::kDone;  // The call ended early.
              return Ready();
            }
            ++sent_;
            break;
          }

          case State::kFinishing: {
            if (!finish_future_.is_pendable()) {
              finish_future_ = writer_.Finish();
            }
            PW_AWAIT(finish_future_, cx);
            state_ = State::kDone;
            return Ready();
          }

          case State::kUninitialized:
          case State::kDone:
            PW_CRASH("Polled a future that is not pendable");
        }
      }
    }

   private:
    enum class State { kUninitialized, kStarting, kWriting, kFinishing, kDone };

    State state_ = State::kUninitialized;
    EchoMessage response_;
    uint32_t count_ = 0;
    uint32_t sent_ = 0;
    pw::rpc2::Writer<EchoMessage> writer_;
    pw::rpc2::WriteFuture<EchoMessage> write_future_;
    pw::rpc2::WriteFuture<> finish_future_;
  };

  class CollectFuture {
   public:
    using value_type = void;

    CollectFuture() = default;
    CollectFuture(pw::rpc2::Reader<EchoMessage> reader,
                  pw::rpc2::UnaryWriter<EchoMessage> responder)
        : state_(State::kReading),
          reader_(std::move(reader)),
          responder_(std::move(responder)) {}

    bool is_pendable() const {
      return state_ != State::kUninitialized && state_ != State::kDone;
    }
    bool is_complete() const { return state_ == State::kDone; }

    Poll<> Pend(Context& cx) {
      while (true) {
        switch (state_) {
          case State::kReading: {
            if (!read_future_.is_pendable()) {
              read_future_ = reader_.Read();
            }
            PW_AWAIT(pw::Result<EchoMessage> request, read_future_, cx);
            if (request.status().IsOutOfRange()) {
              state_ = State::kResponding;  // The client finished its stream.
              break;
            }
            if (!request.ok()) {
              state_ = State::kDone;
              return Ready();
            }
            PW_LOG_INFO("Collect: %s", request->msg.c_str());

            // Messages that don't fit in the response are truncated.
            if (!response_.msg.empty()) {
              pw::string::Append(response_.msg, " ").IgnoreError();
            }
            pw::string::Append(response_.msg, request->msg).IgnoreError();
            break;
          }

          case State::kResponding: {
            if (!response_future_.is_pendable()) {
              response_future_ = responder_.Finish(std::move(response_));
            }
            PW_AWAIT(response_future_, cx);
            state_ = State::kDone;
            return Ready();
          }

          case State::kUninitialized:
          case State::kDone:
            PW_CRASH("Polled a future that is not pendable");
        }
      }
    }

   private:
    enum class State { kUninitialized, kReading, kResponding, kDone };

    State state_ = State::kUninitialized;
    EchoMessage response_;
    pw::rpc2::Reader<EchoMessage> reader_;
    pw::rpc2::UnaryWriter<EchoMessage> responder_;
    pw::rpc2::ReadFuture<EchoMessage> read_future_;
    pw::rpc2::WriteFuture<EchoMessage> response_future_;
  };

  // DOCSTAG: [pw_rpc2-examples-echo-server-polling-bidi]
  class EchoStreamFuture {
   public:
    using value_type = void;

    EchoStreamFuture() = default;
    EchoStreamFuture(pw::rpc2::Reader<EchoMessage> reader,
                     pw::rpc2::Writer<EchoMessage> writer)
        : state_(State::kReading),
          reader_(std::move(reader)),
          writer_(std::move(writer)) {}

    bool is_pendable() const {
      return state_ != State::kUninitialized && state_ != State::kDone;
    }
    bool is_complete() const { return state_ == State::kDone; }

    Poll<> Pend(Context& cx) {
      while (true) {
        switch (state_) {
          case State::kReading: {
            if (!read_future_.is_pendable()) {
              read_future_ = reader_.Read();
            }
            PW_AWAIT(pw::Result<EchoMessage> request, read_future_, cx);
            if (request.status().IsOutOfRange()) {
              state_ = State::kFinishing;  // The client finished its stream.
              break;
            }
            if (!request.ok()) {
              state_ = State::kDone;
              return Ready();
            }
            PW_LOG_INFO("EchoStream: %s", request->msg.c_str());
            write_future_ = writer_.Write(std::move(*request));
            state_ = State::kWriting;
            break;
          }

          case State::kWriting: {
            PW_AWAIT(pw::Status status, write_future_, cx);
            if (!status.ok()) {
              state_ = State::kDone;
              return Ready();
            }
            state_ = State::kReading;
            break;
          }

          case State::kFinishing: {
            if (!finish_future_.is_pendable()) {
              finish_future_ = writer_.Finish();
            }
            PW_AWAIT(finish_future_, cx);
            state_ = State::kDone;
            return Ready();
          }

          case State::kUninitialized:
          case State::kDone:
            PW_CRASH("Polled a future that is not pendable");
        }
      }
    }

   private:
    enum class State { kUninitialized, kReading, kWriting, kFinishing, kDone };

    State state_ = State::kUninitialized;
    pw::rpc2::Reader<EchoMessage> reader_;
    pw::rpc2::Writer<EchoMessage> writer_;
    pw::rpc2::ReadFuture<EchoMessage> read_future_;
    pw::rpc2::WriteFuture<EchoMessage> write_future_;
    pw::rpc2::WriteFuture<> finish_future_;
  };
  // DOCSTAG: [pw_rpc2-examples-echo-server-polling-bidi]
};

}  // namespace

int main(int argc, char* argv[]) {
  uint16_t port = 8000;
  if (argc > 1) {
    std::from_chars(argv[1], argv[1] + std::strlen(argv[1]), port);
  }

  pw::Allocator& allocator = pw::allocator::GetLibCAllocator();
  BasicDispatcher dispatcher;

  pw::transport::FramedTcpListener listener(allocator);
  if (!listener.Listen(port).ok()) {
    return 1;
  }
  PW_LOG_INFO("Listening on port %u", static_cast<unsigned>(listener.port()));

  EchoService service;
  pw::rpc2::Server server(allocator, dispatcher);
  PW_CHECK_OK(server.RegisterService(service));
  PW_CHECK_OK(server.RegisterListenerBlocking(listener));
  server.Start();

  // Serves RPCs until the process is terminated.
  dispatcher.RunToCompletion();
  return 0;
}
