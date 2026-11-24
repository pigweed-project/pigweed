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

// SensorAggregator client that makes its RPC from a manual async2 task.
//
// Usage: client_polling [port]
//
// Connects to port 8000 on 127.0.0.1 by default. Exits with a nonzero status if
// the RPC fails.

#define PW_LOG_MODULE_NAME "SENSOR_CLIENT"

#include <charconv>
#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <utility>

#include "pw_allocator/allocator.h"
#include "pw_allocator/libc_allocator.h"
#include "pw_async2/await.h"
#include "pw_async2/basic_dispatcher.h"
#include "pw_async2/context.h"
#include "pw_async2/dispatcher.h"
#include "pw_async2/poll.h"
#include "pw_async2/task.h"
#include "pw_log/log.h"
#include "pw_result/result.h"
#include "pw_rpc2/client.h"
#include "pw_rpc2/status.h"
#include "pw_transport/framed_tcp.h"
#include "pw_transport/transport.h"
#include "sensor_pb/sensor.pwpb.rpc2.h"

using pw::async2::BasicDispatcher;
using pw::async2::Context;
using pw::async2::Dispatcher;
using pw::async2::Poll;
using pw::async2::Ready;
using pw::async2::Task;

namespace {

using GetTemperatureAggregatesRequest =
    examples::pwpb::GetTemperatureAggregatesRequest::Message;
using GetTemperatureAggregatesResponse =
    examples::pwpb::GetTemperatureAggregatesResponse::Message;
using SensorAggregatorClient =
    examples::pw_rpc2::pwpb::SensorAggregator::Client;

// Connects to the server, calls GetTemperatureAggregates, and closes the
// connection.
class ClientTask : public Task {
 public:
  ClientTask(Dispatcher& dispatcher,
             pw::Allocator& allocator,
             pw::transport::ReliableConnector& connector)
      : dispatcher_(dispatcher), allocator_(allocator), connector_(connector) {}

  pw::rpc2::RpcStatus status() const { return status_; }

 private:
  enum class State {
    kConnecting,
    kGetTemperatureAggregates,
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
          PW_AWAIT(pw::rpc2::RpcResult<pw::rpc2::Client> client,
                   connect_future_,
                   cx);
          if (!client.ok()) {
            status_ = client.status();
            return Ready();
          }
          client_ = std::move(*client);
          aggregator_client_ = SensorAggregatorClient(client_);
          state_ = State::kGetTemperatureAggregates;
          break;
        }

        case State::kGetTemperatureAggregates: {
          if (!get_temp_future_.is_pendable()) {
            get_temp_future_ = aggregator_client_.GetTemperatureAggregates({});
          }
          PW_AWAIT(
              pw::rpc2::RpcResult<GetTemperatureAggregatesResponse> response,
              get_temp_future_,
              cx);
          if (!response.ok()) {
            status_ = response.status();
            state_ = State::kClosing;
            break;
          }
          PW_LOG_INFO("GetTemperatureAggregates: min=%" PRId32 ", max=%" PRId32
                      ", avg=%" PRId32,
                      response->min_temp_celsius,
                      response->max_temp_celsius,
                      response->avg_temp_celsius);
          state_ = State::kClosing;
          break;
        }

        case State::kClosing: {
          if (!close_future_.is_pendable()) {
            close_future_ = client_.Close();
          }
          PW_AWAIT(pw::rpc2::RpcStatus close_status, close_future_, cx);
          status_.Update(close_status);
          return Ready();
        }
      }
    }
  }

  Dispatcher& dispatcher_;
  pw::Allocator& allocator_;
  pw::transport::ReliableConnector& connector_;

  State state_ = State::kConnecting;
  pw::rpc2::RpcStatus status_;
  pw::rpc2::Client client_;
  SensorAggregatorClient aggregator_client_;

  pw::rpc2::ClientFuture connect_future_;
  pw::rpc2::UnaryFuture<GetTemperatureAggregatesRequest,
                        GetTemperatureAggregatesResponse>
      get_temp_future_;
  pw::rpc2::ControlFuture close_future_;
};

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
