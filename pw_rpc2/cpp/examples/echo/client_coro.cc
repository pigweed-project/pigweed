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

// Echo client that makes its RPCs from C++20 coroutines.
//
// Usage: client_coro [port]
//
// Connects to port 8000 on 127.0.0.1 by default. Exits with a nonzero status if
// any RPC fails.

#define PW_LOG_MODULE_NAME "ECHO_CLIENT"

#include <charconv>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string_view>

#include "echo_pb/echo.pwpb.rpc2.h"
#include "pw_allocator/allocator.h"
#include "pw_allocator/libc_allocator.h"
#include "pw_async2/basic_dispatcher.h"
#include "pw_async2/coro.h"
#include "pw_async2/dispatcher.h"
#include "pw_async2/future_task.h"
#include "pw_log/log.h"
#include "pw_result/result.h"
#include "pw_rpc2/client.h"
#include "pw_rpc2/reader.h"
#include "pw_rpc2/writer.h"
#include "pw_status/status.h"
#include "pw_status/try.h"
#include "pw_transport/framed_tcp.h"
#include "pw_transport/socket.h"

using pw::async2::BasicDispatcher;
using pw::async2::Coro;
using pw::async2::CoroContext;
using pw::async2::Dispatcher;
using pw::async2::FutureTask;

namespace {

// DOCSTAG: [pw_rpc2-examples-echo-client-coro-calls]
using EchoMessage = examples::pwpb::EchoMessage::Message;

// `pw::rpc2::Client` is a connection to a server, and the generated `Echo`
// client binds a `pw::rpc2::Client` to the `Echo` service. Both are cheap to
// copy, so pass them by value.
using EchoClient = examples::pw_rpc2::pwpb::Echo::Client;

// Unary RPC: sends one request and receives one response.
Coro<pw::Status> CallEcho(CoroContext, EchoClient client) {
  PW_CO_TRY_ASSIGN(EchoMessage response,
                   co_await client.Echo({.msg = "hello"}));
  PW_LOG_INFO("Echo: %s", response.msg.c_str());
  co_return pw::OkStatus();
}

// Server streaming RPC: sends one request, then reads responses until the
// server finishes its stream.
Coro<pw::Status> CallRepeat(CoroContext, EchoClient client) {
  PW_CO_TRY_ASSIGN(pw::rpc2::Reader<EchoMessage> reader,
                   co_await client.Repeat({.msg = "ping", .count = 3}));
  while (true) {
    pw::Result<EchoMessage> response = co_await reader.Read();
    if (response.status().IsOutOfRange()) {
      co_return pw::OkStatus();  // The server finished its stream.
    }
    PW_CO_TRY(response.status());
    PW_LOG_INFO("Repeat: %s", response->msg.c_str());
  }
}

// Client streaming RPC: streams requests, then receives one response.
Coro<pw::Status> CallCollect(CoroContext, EchoClient client) {
  PW_CO_TRY_ASSIGN(auto call, co_await client.Collect());
  pw::rpc2::Writer<EchoMessage>& writer = call.writer();
  for (std::string_view word : {"one", "two", "three"}) {
    PW_CO_TRY(co_await writer.Write({.msg = word}));
  }
  PW_CO_TRY(co_await writer.Finish());

  PW_CO_TRY_ASSIGN(EchoMessage response, co_await call.response());
  PW_LOG_INFO("Collect: %s", response.msg.c_str());
  co_return pw::OkStatus();
}

// Bidirectional streaming RPC: streams requests and reads the server's stream
// of responses. The writer and reader are independent; they could also be used
// from separate tasks.
Coro<pw::Status> CallEchoStream(CoroContext, EchoClient client) {
  PW_CO_TRY_ASSIGN(auto call, co_await client.EchoStream());
  pw::rpc2::Writer<EchoMessage>& writer = call.writer();
  for (std::string_view word : {"alpha", "beta"}) {
    PW_CO_TRY(co_await writer.Write({.msg = word}));
  }
  PW_CO_TRY(co_await writer.Finish());

  pw::rpc2::Reader<EchoMessage>& reader = call.reader();
  while (true) {
    pw::Result<EchoMessage> response = co_await reader.Read();
    if (response.status().IsOutOfRange()) {
      co_return pw::OkStatus();  // The server finished its stream.
    }
    PW_CO_TRY(response.status());
    PW_LOG_INFO("EchoStream: %s", response->msg.c_str());
  }
}
// DOCSTAG: [pw_rpc2-examples-echo-client-coro-calls]

// DOCSTAG: [pw_rpc2-examples-echo-client-coro-run]
// Connects to the server, makes each type of RPC, and closes the connection.
Coro<pw::Status> RunClient(
    CoroContext cx,
    Dispatcher& dispatcher,
    pw::Allocator& allocator,
    pw::transport::ReliableDatagramConnector& connector) {
  PW_CO_TRY_ASSIGN(
      pw::rpc2::Client client,
      co_await pw::rpc2::Client::Connect(dispatcher, allocator, connector));
  EchoClient echo_client(client);

  PW_CO_TRY(co_await CallEcho(cx, echo_client));
  PW_CO_TRY(co_await CallRepeat(cx, echo_client));
  PW_CO_TRY(co_await CallCollect(cx, echo_client));
  PW_CO_TRY(co_await CallEchoStream(cx, echo_client));
  co_return co_await client.Close();
}
// DOCSTAG: [pw_rpc2-examples-echo-client-coro-run]

}  // namespace

int main(int argc, char* argv[]) {
  uint16_t port = 8000;
  if (argc > 1) {
    std::from_chars(argv[1], argv[1] + std::strlen(argv[1]), port);
  }

  // DOCSTAG: [pw_rpc2-examples-echo-client-coro-main]
  pw::Allocator& allocator = pw::allocator::GetLibCAllocator();
  BasicDispatcher dispatcher;
  pw::transport::FramedTcpConnector connector(allocator, "127.0.0.1", port);

  CoroContext coro_cx(allocator);
  FutureTask task(RunClient(coro_cx, dispatcher, allocator, connector));
  dispatcher.Post(task);
  dispatcher.RunToCompletion();
  return task.value().ok() ? 0 : 1;
  // DOCSTAG: [pw_rpc2-examples-echo-client-coro-main]
}
