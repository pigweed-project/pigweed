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

// Downstream Sensor node that returns local temperature measurements.
//
// Usage: node [port] [temperature_celsius]
//
// Listens on port 0 by default, which picks an available port.
// Reports 20 C by default.

#define PW_LOG_MODULE_NAME "SENSOR_SERVER"

#include <charconv>
#include <cinttypes>
#include <cstdint>
#include <cstring>

#include "pw_allocator/allocator.h"
#include "pw_allocator/libc_allocator.h"
#include "pw_assert/check.h"
#include "pw_async2/basic_dispatcher.h"
#include "pw_async2/coro.h"
#include "pw_log/log.h"
#include "pw_rpc2/server.h"
#include "pw_rpc2/writer.h"
#include "pw_transport/framed_tcp.h"
#include "sensor_pb/sensor.pwpb.rpc2.h"

using pw::async2::BasicDispatcher;
using pw::async2::Coro;
using pw::async2::CoroContext;

namespace {

using GetTemperatureRequest = examples::pwpb::GetTemperatureRequest::Message;
using GetTemperatureResponse = examples::pwpb::GetTemperatureResponse::Message;

// Sensor service that returns local temperature measurements.
class SensorService
    : public examples::pw_rpc2::pwpb::Sensor::Service<SensorService> {
 public:
  explicit SensorService(int32_t temperature_celsius)
      : temperature_celsius_(temperature_celsius) {}

  Coro<void> GetTemperature(
      CoroContext,
      GetTemperatureRequest,
      pw::rpc2::UnaryWriter<GetTemperatureResponse> responder) {
    PW_LOG_INFO("Server GetTemperature");
    co_await responder.Finish({.temp_celsius = temperature_celsius_});
  }

 private:
  int32_t temperature_celsius_;
};

}  // namespace

int main(int argc, char* argv[]) {
  uint16_t port = 0;
  int32_t temperature_celsius = 20;
  if (argc > 1) {
    std::from_chars(argv[1], argv[1] + std::strlen(argv[1]), port);
  }
  if (argc > 2) {
    std::from_chars(
        argv[2], argv[2] + std::strlen(argv[2]), temperature_celsius);
  }

  pw::Allocator& allocator = pw::allocator::GetLibCAllocator();
  BasicDispatcher dispatcher;

  pw::transport::FramedTcpListener listener(allocator);
  if (!listener.Listen(port).ok()) {
    return 1;
  }
  PW_LOG_INFO("Listening on port %u", static_cast<unsigned>(listener.port()));

  SensorService service(temperature_celsius);
  pw::rpc2::Server server(allocator, dispatcher);
  PW_CHECK_OK(server.RegisterService(service));
  PW_CHECK_OK(server.RegisterListenerBlocking(listener));
  server.Start();

  // Serves RPCs until the process is terminated.
  dispatcher.RunToCompletion();
  return 0;
}
