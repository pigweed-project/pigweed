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

// Sensor aggregator server that fans out requests to 3 downstream sensor nodes
// and fans in their measurements using manual polling and async2 futures.
//
// Usage: server_polling <port> <node_port> <node_port> <node_port>
//
// Connects to a node on each node port on 127.0.0.1, then listens on port.
// Port 0 picks an available port.

#define PW_LOG_MODULE_NAME "SENSOR_AGGREGATOR"

#include <algorithm>
#include <array>
#include <charconv>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

#include "pw_allocator/allocator.h"
#include "pw_allocator/libc_allocator.h"
#include "pw_assert/check.h"
#include "pw_async2/await.h"
#include "pw_async2/basic_dispatcher.h"
#include "pw_async2/context.h"
#include "pw_async2/dispatcher.h"
#include "pw_async2/future.h"
#include "pw_async2/join.h"
#include "pw_async2/poll.h"
#include "pw_async2/task.h"
#include "pw_log/log.h"
#include "pw_result/result.h"
#include "pw_rpc2/client.h"
#include "pw_rpc2/server.h"
#include "pw_rpc2/status.h"
#include "pw_rpc2/writer.h"
#include "pw_span/span.h"
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
using GetTemperatureRequest = examples::pwpb::GetTemperatureRequest::Message;
using GetTemperatureResponse = examples::pwpb::GetTemperatureResponse::Message;
using SensorClient = examples::pw_rpc2::pwpb::Sensor::Client;

constexpr size_t kNumDownstreamServers = 3;

uint16_t ParsePort(const char* arg) {
  uint16_t port = 0;
  std::from_chars(arg, arg + std::strlen(arg), port);
  return port;
}

GetTemperatureAggregatesResponse AggregateResponses(
    pw::span<const pw::rpc2::RpcResult<GetTemperatureResponse>> results) {
  PW_CHECK(!results.empty());
  int32_t min_temp = std::numeric_limits<int32_t>::max();
  int32_t max_temp = std::numeric_limits<int32_t>::min();
  int64_t sum_temp = 0;

  for (const pw::rpc2::RpcResult<GetTemperatureResponse>& res : results) {
    PW_CHECK_OK(res.status());
    min_temp = std::min(min_temp, res->temp_celsius);
    max_temp = std::max(max_temp, res->temp_celsius);
    sum_temp += res->temp_celsius;
  }

  return {
      .min_temp_celsius = min_temp,
      .max_temp_celsius = max_temp,
      .avg_temp_celsius =
          static_cast<int32_t>(sum_temp / static_cast<int64_t>(results.size())),
  };
}

// SensorAggregator service that fans out GetTemperature to 3 downstream
// sensors and aggregates their measurements.
class SensorAggregatorService
    : public examples::pw_rpc2::pwpb::SensorAggregator::Service<
          SensorAggregatorService> {
 public:
  class GetTemperatureAggregatesFuture {
   public:
    using value_type = void;

    GetTemperatureAggregatesFuture() = default;
    GetTemperatureAggregatesFuture(
        SensorAggregatorService& service,
        GetTemperatureAggregatesRequest,
        pw::rpc2::UnaryWriter<GetTemperatureAggregatesResponse> responder)
        : state_(State::kStarting),
          service_(&service),
          responder_(std::move(responder)) {}

    bool is_pendable() const {
      return state_ != State::kUninitialized && state_ != State::kDone;
    }
    bool is_complete() const { return state_ == State::kDone; }

    // DOCSTAG: [pw_rpc2-examples-fan-in-fan-out-aggregator-polling]
    Poll<> Pend(Context& cx) {
      while (true) {
        switch (state_) {
          case State::kStarting:
            PW_LOG_INFO("Aggregator GetTemperatureAggregates");
            // pw::async2::Join requires a fixed number of futures known at
            // compile time, which is why `clients_` is stored and passed as a
            // std::array.
            join_future_ =
                pw::async2::Join(service_->clients_[0].GetTemperature({}),
                                 service_->clients_[1].GetTemperature({}),
                                 service_->clients_[2].GetTemperature({}));
            state_ = State::kJoining;
            break;

          case State::kJoining: {
            PW_AWAIT(auto joined, join_future_, cx);
            auto& [res0, res1, res2] = joined;
            std::array results = {
                std::move(res0), std::move(res1), std::move(res2)};
            GetTemperatureAggregatesResponse response =
                AggregateResponses(results);
            PW_LOG_INFO("Aggregator gathered temperatures: min=%" PRId32
                        ", max=%" PRId32 ", avg=%" PRId32,
                        response.min_temp_celsius,
                        response.max_temp_celsius,
                        response.avg_temp_celsius);
            response_future_ = responder_.Finish(response);
            state_ = State::kResponding;
            break;
          }

          case State::kResponding:
            PW_AWAIT(response_future_, cx);
            state_ = State::kDone;
            return Ready();

          case State::kUninitialized:
          case State::kDone:
            PW_CRASH("Polled a future that is not pendable");
        }
      }
    }
    // DOCSTAG: [pw_rpc2-examples-fan-in-fan-out-aggregator-polling]

   private:
    using CallFuture =
        pw::rpc2::UnaryFuture<GetTemperatureRequest, GetTemperatureResponse>;
    using JoinCallsFuture =
        pw::async2::JoinFuture<CallFuture, CallFuture, CallFuture>;

    enum class State {
      kUninitialized,
      kStarting,
      kJoining,
      kResponding,
      kDone,
    };

    State state_ = State::kUninitialized;
    SensorAggregatorService* service_ = nullptr;
    pw::rpc2::UnaryWriter<GetTemperatureAggregatesResponse> responder_;
    JoinCallsFuture join_future_;
    pw::rpc2::WriteFuture<GetTemperatureAggregatesResponse> response_future_;
  };

  void set_clients(std::array<SensorClient, kNumDownstreamServers> clients) {
    clients_ = std::move(clients);
  }

 private:
  std::array<SensorClient, kNumDownstreamServers> clients_;
};

static_assert(pw::async2::Future<
              SensorAggregatorService::GetTemperatureAggregatesFuture>);

// Connects to the 3 downstream servers, then starts the aggregator server.
class StartAggregatorTask : public Task {
 public:
  StartAggregatorTask(Dispatcher& dispatcher,
                      pw::Allocator& allocator,
                      pw::transport::ReliableConnector& connector0,
                      pw::transport::ReliableConnector& connector1,
                      pw::transport::ReliableConnector& connector2,
                      SensorAggregatorService& aggregator_service,
                      pw::rpc2::Server& aggregator_server,
                      uint16_t port)
      : dispatcher_(dispatcher),
        allocator_(allocator),
        connector0_(connector0),
        connector1_(connector1),
        connector2_(connector2),
        aggregator_service_(aggregator_service),
        aggregator_server_(aggregator_server),
        port_(port) {}

  pw::rpc2::RpcStatus status() const { return status_; }

 private:
  using JoinConnectFuture = pw::async2::JoinFuture<pw::rpc2::ClientFuture,
                                                   pw::rpc2::ClientFuture,
                                                   pw::rpc2::ClientFuture>;

  Poll<> DoPend(Context& cx) override {
    if (!join_future_.is_pendable()) {
      join_future_ = pw::async2::Join(
          pw::rpc2::Client::Connect(dispatcher_, allocator_, connector0_),
          pw::rpc2::Client::Connect(dispatcher_, allocator_, connector1_),
          pw::rpc2::Client::Connect(dispatcher_, allocator_, connector2_));
    }
    PW_AWAIT(auto clients, join_future_, cx);
    auto& [client0, client1, client2] = clients;
    status_.Update(client0.status());
    status_.Update(client1.status());
    status_.Update(client2.status());
    if (!status_.ok()) {
      return Ready();
    }

    aggregator_service_.set_clients({
        SensorClient(*client0),
        SensorClient(*client1),
        SensorClient(*client2),
    });
    aggregator_server_.Start();
    PW_LOG_INFO("Listening on port %u", static_cast<unsigned>(port_));
    return Ready();
  }

  Dispatcher& dispatcher_;
  pw::Allocator& allocator_;
  pw::transport::ReliableConnector& connector0_;
  pw::transport::ReliableConnector& connector1_;
  pw::transport::ReliableConnector& connector2_;
  SensorAggregatorService& aggregator_service_;
  pw::rpc2::Server& aggregator_server_;
  uint16_t port_;
  pw::rpc2::RpcStatus status_;
  JoinConnectFuture join_future_;
};

}  // namespace

int main(int argc, char* argv[]) {
  if (static_cast<size_t>(argc) != 2 + kNumDownstreamServers) {
    PW_LOG_ERROR("Usage: %s <port> <node_port> <node_port> <node_port>",
                 argv[0]);
    return 1;
  }

  pw::Allocator& allocator = pw::allocator::GetLibCAllocator();
  BasicDispatcher dispatcher;

  pw::transport::FramedTcpConnector connector0(
      allocator, "127.0.0.1", ParsePort(argv[2]));
  pw::transport::FramedTcpConnector connector1(
      allocator, "127.0.0.1", ParsePort(argv[3]));
  pw::transport::FramedTcpConnector connector2(
      allocator, "127.0.0.1", ParsePort(argv[4]));

  pw::transport::FramedTcpListener listener(allocator);
  if (!listener.Listen(ParsePort(argv[1])).ok()) {
    return 1;
  }

  SensorAggregatorService aggregator_service;
  pw::rpc2::Server aggregator_server(allocator, dispatcher);
  PW_CHECK_OK(aggregator_server.RegisterService(aggregator_service));
  PW_CHECK_OK(aggregator_server.RegisterListenerBlocking(listener));

  StartAggregatorTask start_task(dispatcher,
                                 allocator,
                                 connector0,
                                 connector1,
                                 connector2,
                                 aggregator_service,
                                 aggregator_server,
                                 listener.port());
  dispatcher.Post(start_task);

  // Serves RPCs until the process is terminated. Returns early only if the
  // aggregator can't connect to the servers.
  dispatcher.RunToCompletion();
  return start_task.status().ok() ? 0 : 1;
}
