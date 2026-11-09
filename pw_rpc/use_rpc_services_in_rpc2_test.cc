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

#include <array>
#include <chrono>
#include <utility>

#include "pw_allocator/allocator.h"
#include "pw_allocator/testing.h"
#include "pw_assert/check.h"
#include "pw_async2/dispatcher.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/future_task.h"
#include "pw_rpc/channel.h"
#include "pw_rpc/examples/sensor_service.pwpb.rpc2.h"
#include "pw_rpc/rpc2_adapter.h"
#include "pw_rpc/server.h"
#include "pw_rpc2/client.h"
#include "pw_rpc2/internal/test_utils.h"
#include "pw_rpc2/reader.h"
#include "pw_rpc2/server.h"
#include "pw_rpc2/status.h"
#include "pw_status/status.h"
#include "pw_thread/options.h"
#include "pw_thread/sleep.h"
#include "pw_thread/test_thread_context.h"
#include "pw_transport/forwarding_socket.h"
#include "pw_unit_test/framework.h"
#include "pw_unit_test/status_macros.h"

namespace pw::rpc::examples {

void ServeWithExistingRpcServer(pw::Allocator& allocator,
                                pw::Allocator& adapter_allocator,
                                pw::async2::Dispatcher& dispatcher,
                                const pw::thread::Options& thread_options);

void ServeWithoutRpcServer(pw::Allocator& allocator,
                           pw::Allocator& adapter_allocator,
                           pw::async2::Dispatcher& dispatcher,
                           const pw::thread::Options& thread_options);

void RegisterOwnedService(pw::rpc2::Server& rpc2_server,
                          pw::rpc::Rpc2ServerAdapter& adapter);

void RegisterExistingService(pw::rpc::Server& rpc_server,
                             pw::rpc2::Server& rpc2_server,
                             pw::rpc::Rpc2ServerAdapter& adapter);

void RegisterOwnedWithNonOkResponses(pw::rpc2::Server& rpc2_server,
                                     pw::rpc::Rpc2ServerAdapter& adapter);

void RegisterExistingWithNonOkResponses(pw::rpc::Server& rpc_server,
                                        pw::rpc2::Server& rpc2_server,
                                        pw::rpc::Rpc2ServerAdapter& adapter);

namespace {

using SensorResponse = pw::rpc::examples::pwpb::SensorResponse::Message;

constexpr std::chrono::seconds kTimeout(30);
constexpr std::chrono::milliseconds kPollInterval(1);

// Runs `dispatcher` until `future` completes. The adapter's thread wakes the
// dispatcher from another thread, so this polls until the future is done.
template <typename Fut>
typename Fut::value_type RunToCompletion(pw::async2::DispatcherForTest& d,
                                         Fut future) {
  pw::async2::FutureTask task(std::move(future));
  d.Post(task);
  for (auto i = kTimeout / kPollInterval; i > 0 && !task.has_value(); --i) {
    d.RunUntilStalled();
    if (!task.has_value()) {
      pw::this_thread::sleep_for(kPollInterval);
    }
  }
  PW_CHECK(task.has_value(), "The future did not complete");
  return std::move(task.value());
}

struct TestContext {
  pw::allocator::test::AllocatorForTest<32768> allocator;
  pw::allocator::test::AllocatorForTest<4096> adapter_allocator;
  pw::async2::DispatcherForTest dispatcher;
  pw::thread::test::TestThreadContext adapter_thread;
  float expected_temperature = 24.2f;
  bool expect_non_ok_responses = false;
};

TestContext* current_context = nullptr;

}  // namespace

void RunServer(pw::rpc2::Server& rpc2_server,
               pw::rpc::Rpc2ServerAdapter& adapter) {
  PW_CHECK_NOTNULL(current_context);
  auto& cx = *current_context;

  pw::rpc2::test::MockTransport transport;
  PW_CHECK_OK(rpc2_server.RegisterListenerBlocking(transport));
  rpc2_server.Start();

  // Connect a pw_rpc2 client over an in-memory connection.
  pw::transport::ForwardingSocketPair sockets =
      pw::transport::ForwardingSocketPair::Create(cx.allocator);
  transport.ResolveAccept(sockets.second());
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      pw::rpc2::Client rpc2_client,
      RunToCompletion(cx.dispatcher,
                      pw::rpc2::Client::Connect(
                          cx.dispatcher, cx.allocator, sockets.first())));

  pw::rpc::examples::pw_rpc2::pwpb::SensorService::Client client(rpc2_client);

  PW_TEST_ASSERT_OK_AND_ASSIGN(
      SensorResponse reading,
      RunToCompletion(cx.dispatcher, client.GetReading({.sensor_id = 1})));
  EXPECT_FLOAT_EQ(reading.temperature, cx.expected_temperature);
  EXPECT_FLOAT_EQ(reading.humidity, 45.0f);
  EXPECT_FALSE(reading.error);

  // `sensor_id = 0` sets `response.error = true` and returns `NOT_FOUND`.
  if (cx.expect_non_ok_responses) {
    PW_TEST_ASSERT_OK_AND_ASSIGN(
        SensorResponse failed,
        RunToCompletion(cx.dispatcher, client.GetReading({.sensor_id = 0})));
    EXPECT_TRUE(failed.error);
  } else {
    EXPECT_EQ(
        RunToCompletion(cx.dispatcher, client.GetReading({.sensor_id = 0}))
            .status(),
        pw::rpc2::RpcStatus::Cancelled());
  }

  PW_TEST_ASSERT_OK_AND_ASSIGN(
      pw::rpc2::Reader<SensorResponse> readings,
      RunToCompletion(
          cx.dispatcher,
          client.StreamReadings(
              {.sensor_id = 1, .sample_count = 3, .interval_ms = 0})));
  for (int i = 0; i < 3; ++i) {
    PW_TEST_ASSERT_OK_AND_ASSIGN(
        SensorResponse sample, RunToCompletion(cx.dispatcher, readings.Read()));
    EXPECT_FLOAT_EQ(sample.temperature, 20.0f + static_cast<float>(i));
  }
  EXPECT_EQ(RunToCompletion(cx.dispatcher, readings.Read()).status(),
            pw::rpc2::RpcStatus::EndOfStream());

  // Tear down in order: the client, the pw_rpc2 server, then the adapter.
  PW_TEST_EXPECT_OK(RunToCompletion(cx.dispatcher, rpc2_client.Close()));
  rpc2_client = pw::rpc2::Client();
  transport.ResolveAccept(pw::Status::Cancelled());
  PW_TEST_EXPECT_OK(RunToCompletion(cx.dispatcher, rpc2_server.Close()));
  adapter.Stop();
}

namespace {

TEST(Rpc2AdapterExample, ServeWithExistingRpcServer) {
  TestContext cx;
  current_context = &cx;
  ServeWithExistingRpcServer(cx.allocator,
                             cx.adapter_allocator,
                             cx.dispatcher,
                             cx.adapter_thread.options());
  current_context = nullptr;
}

TEST(Rpc2AdapterExample, ServeWithoutRpcServer) {
  TestContext cx;
  current_context = &cx;
  ServeWithoutRpcServer(cx.allocator,
                        cx.adapter_allocator,
                        cx.dispatcher,
                        cx.adapter_thread.options());
  current_context = nullptr;
}

TEST(Rpc2AdapterExample, RegisterOwnedService) {
  TestContext cx;
  cx.expected_temperature = 24.7f;
  current_context = &cx;
  pw::rpc2::Server rpc2_server(cx.allocator, cx.dispatcher);
  pw::rpc::Rpc2Adapter adapter(cx.adapter_allocator,
                               cx.adapter_thread.options());
  RegisterOwnedService(rpc2_server, adapter);
  current_context = nullptr;
}

TEST(Rpc2AdapterExample, RegisterExistingService) {
  TestContext cx;
  current_context = &cx;
  std::array<pw::rpc::Channel, 4> channels{};
  pw::rpc::Server rpc_server(channels);
  pw::rpc2::Server rpc2_server(cx.allocator, cx.dispatcher);
  pw::rpc::Rpc2ServerAdapter adapter(
      rpc_server, cx.adapter_allocator, cx.adapter_thread.options());
  RegisterExistingService(rpc_server, rpc2_server, adapter);
  current_context = nullptr;
}

TEST(Rpc2AdapterExample, RegisterOwnedWithNonOkResponses) {
  TestContext cx;
  cx.expected_temperature = 24.7f;
  cx.expect_non_ok_responses = true;
  current_context = &cx;
  pw::rpc2::Server rpc2_server(cx.allocator, cx.dispatcher);
  pw::rpc::Rpc2Adapter adapter(cx.adapter_allocator,
                               cx.adapter_thread.options());
  RegisterOwnedWithNonOkResponses(rpc2_server, adapter);
  current_context = nullptr;
}

TEST(Rpc2AdapterExample, RegisterExistingWithNonOkResponses) {
  TestContext cx;
  cx.expect_non_ok_responses = true;
  current_context = &cx;
  std::array<pw::rpc::Channel, 4> channels{};
  pw::rpc::Server rpc_server(channels);
  pw::rpc2::Server rpc2_server(cx.allocator, cx.dispatcher);
  pw::rpc::Rpc2ServerAdapter adapter(
      rpc_server, cx.adapter_allocator, cx.adapter_thread.options());
  RegisterExistingWithNonOkResponses(rpc_server, rpc2_server, adapter);
  current_context = nullptr;
}

}  // namespace
}  // namespace pw::rpc::examples
