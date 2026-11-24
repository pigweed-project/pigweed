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

// SensorAggregator client that makes its RPC from a C++20 coroutine.
//
// Usage: client_coro [port]
//
// Connects to port 8000 on 127.0.0.1 by default. Exits with a nonzero status if
// the RPC fails.

#define PW_LOG_MODULE_NAME "SENSOR_CLIENT"

#include <charconv>
#include <cinttypes>
#include <cstdint>
#include <cstring>

#include "pw_allocator/allocator.h"
#include "pw_allocator/libc_allocator.h"
#include "pw_async2/basic_dispatcher.h"
#include "pw_async2/coro.h"
#include "pw_async2/dispatcher.h"
#include "pw_async2/future_task.h"
#include "pw_log/log.h"
#include "pw_result/result.h"
#include "pw_rpc2/client.h"
#include "pw_rpc2/status.h"
#include "pw_status/try.h"
#include "pw_transport/framed_tcp.h"
#include "pw_transport/transport.h"
#include "sensor_pb/sensor.pwpb.rpc2.h"

using pw::async2::BasicDispatcher;
using pw::async2::Coro;
using pw::async2::CoroContext;
using pw::async2::Dispatcher;
using pw::async2::FutureTask;

namespace {

using GetTemperatureAggregatesResponse =
    examples::pwpb::GetTemperatureAggregatesResponse::Message;
using SensorAggregatorClient =
    examples::pw_rpc2::pwpb::SensorAggregator::Client;

Coro<pw::rpc2::RpcStatus> CallGetTemperatureAggregates(
    CoroContext, SensorAggregatorClient client) {
  PW_CO_TRY_ASSIGN(GetTemperatureAggregatesResponse response,
                   co_await client.GetTemperatureAggregates({}));
  PW_LOG_INFO("GetTemperatureAggregates: min=%" PRId32 ", max=%" PRId32
              ", avg=%" PRId32,
              response.min_temp_celsius,
              response.max_temp_celsius,
              response.avg_temp_celsius);
  co_return pw::rpc2::RpcStatus();
}

Coro<pw::rpc2::RpcStatus> RunClient(
    CoroContext cx,
    Dispatcher& dispatcher,
    pw::Allocator& allocator,
    pw::transport::ReliableConnector& connector) {
  PW_CO_TRY_ASSIGN(
      pw::rpc2::Client client,
      co_await pw::rpc2::Client::Connect(dispatcher, allocator, connector));
  SensorAggregatorClient aggregator_client(client);

  PW_CO_TRY(co_await CallGetTemperatureAggregates(cx, aggregator_client));
  co_return co_await client.Close();
}

}  // namespace

int main(int argc, char* argv[]) {
  uint16_t port = 8000;
  if (argc > 1) {
    std::from_chars(argv[1], argv[1] + std::strlen(argv[1]), port);
  }

  pw::Allocator& allocator = pw::allocator::GetLibCAllocator();
  BasicDispatcher dispatcher;
  pw::transport::FramedTcpConnector connector(allocator, "127.0.0.1", port);

  CoroContext coro_cx(allocator);
  FutureTask task(RunClient(coro_cx, dispatcher, allocator, connector));
  dispatcher.Post(task);
  dispatcher.RunToCompletion();
  return task.value().ok() ? 0 : 1;
}
