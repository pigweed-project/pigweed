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

// Echo server that implements its RPCs as C++20 coroutines.
//
// Usage: server_coro [port]
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
#include "pw_async2/basic_dispatcher.h"
#include "pw_async2/coro.h"
#include "pw_log/log.h"
#include "pw_result/result.h"
#include "pw_rpc2/reader.h"
#include "pw_rpc2/server.h"
#include "pw_rpc2/writer.h"
#include "pw_status/status.h"
#include "pw_string/util.h"
#include "pw_transport/framed_tcp.h"

using pw::async2::BasicDispatcher;
using pw::async2::Coro;
using pw::async2::CoroContext;

namespace {

// DOCSTAG: [pw_rpc2-examples-echo-server-coro]
using EchoMessage = examples::pwpb::EchoMessage::Message;
using RepeatRequest = examples::pwpb::RepeatRequest::Message;

// Implements each RPC as a coroutine member function. Requests are taken by
// value so that they stay valid across `co_await`.
class EchoService : public examples::pw_rpc2::pwpb::Echo::Service<EchoService> {
 public:
  Coro<void> Echo(CoroContext,
                  EchoMessage request,
                  pw::rpc2::UnaryWriter<EchoMessage> responder) {
    PW_LOG_INFO("Echo: %s", request.msg.c_str());
    co_await responder.Finish(std::move(request));
  }

  Coro<void> Repeat(CoroContext,
                    RepeatRequest request,
                    pw::rpc2::Writer<EchoMessage> writer) {
    PW_LOG_INFO("Repeat: %s x%u", request.msg.c_str(), request.count);
    const EchoMessage response{.msg = request.msg};
    for (uint32_t i = 0; i < request.count; ++i) {
      pw::Status status = co_await writer.Write(response);
      if (!status.ok()) {
        co_return;  // The call ended early, e.g. the client cancelled it.
      }
    }
    co_await writer.Finish();
  }

  Coro<void> Collect(CoroContext,
                     pw::rpc2::Reader<EchoMessage> reader,
                     pw::rpc2::UnaryWriter<EchoMessage> responder) {
    EchoMessage response;
    while (true) {
      pw::Result<EchoMessage> request = co_await reader.Read();
      if (request.status().IsOutOfRange()) {
        break;  // The client finished its stream.
      }
      if (!request.ok()) {
        co_return;
      }
      PW_LOG_INFO("Collect: %s", request->msg.c_str());

      // Messages that don't fit in the response are truncated.
      if (!response.msg.empty()) {
        pw::string::Append(response.msg, " ").IgnoreError();
      }
      pw::string::Append(response.msg, request->msg).IgnoreError();
    }
    co_await responder.Finish(std::move(response));
  }

  Coro<void> EchoStream(CoroContext,
                        pw::rpc2::Reader<EchoMessage> reader,
                        pw::rpc2::Writer<EchoMessage> writer) {
    while (true) {
      pw::Result<EchoMessage> request = co_await reader.Read();
      if (request.status().IsOutOfRange()) {
        break;  // The client finished its stream.
      }
      if (!request.ok()) {
        co_return;
      }
      PW_LOG_INFO("EchoStream: %s", request->msg.c_str());

      pw::Status status = co_await writer.Write(std::move(*request));
      if (!status.ok()) {
        co_return;
      }
    }
    co_await writer.Finish();
  }
};
// DOCSTAG: [pw_rpc2-examples-echo-server-coro]

}  // namespace

int main(int argc, char* argv[]) {
  uint16_t port = 8000;
  if (argc > 1) {
    std::from_chars(argv[1], argv[1] + std::strlen(argv[1]), port);
  }

  // DOCSTAG: [pw_rpc2-examples-echo-server-main]
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
  // DOCSTAG: [pw_rpc2-examples-echo-server-main]
  return 0;
}
