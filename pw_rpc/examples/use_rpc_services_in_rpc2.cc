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

#include "pw_allocator/allocator.h"
#include "pw_assert/check.h"
#include "pw_async2/dispatcher.h"
#include "pw_rpc/channel.h"
#include "pw_rpc/examples/sensor_service.h"
#include "pw_rpc/rpc2_adapter.h"
#include "pw_rpc/server.h"
#include "pw_rpc2/server.h"
#include "pw_thread/options.h"

namespace pw::rpc::examples {

void RunServer(pw::rpc2::Server& rpc2_server,
               pw::rpc::Rpc2ServerAdapter& adapter);

void ServeWithExistingRpcServer(pw::Allocator& allocator,
                                pw::Allocator& adapter_allocator,
                                pw::async2::Dispatcher& dispatcher,
                                const pw::thread::Options& thread_options) {
  // DOCSTAG: [pw_rpc-examples-rpc2-server-adapter]
  // Set up an existing pw_rpc server and service. Any channels already
  // configured on the server keep working, and unassigned channels are used for
  // pw_rpc2 connections.
  std::array<pw::rpc::Channel, 4> channels{};
  pw::rpc::Server rpc_server(channels);
  SensorServiceImpl sensor_service;
  rpc_server.RegisterService(sensor_service);

  pw::rpc2::Server rpc2_server(allocator, dispatcher);

  // Bridge the pw_rpc2 server and the existing pw_rpc server.
  pw::rpc::Rpc2ServerAdapter adapter(
      rpc_server, adapter_allocator, thread_options);

  // Wrap the existing pw_rpc service for registration with the pw_rpc2 server.
  pw::rpc::Rpc2ServiceAdapter adapted_sensor(adapter, sensor_service);
  PW_CHECK_OK(rpc2_server.RegisterService(adapted_sensor));
  // DOCSTAG: [pw_rpc-examples-rpc2-server-adapter]

  RunServer(rpc2_server, adapter);
}

void ServeWithoutRpcServer(pw::Allocator& allocator,
                           pw::Allocator& adapter_allocator,
                           pw::async2::Dispatcher& dispatcher,
                           const pw::thread::Options& thread_options) {
  // DOCSTAG: [pw_rpc-examples-rpc2-adapter]
  pw::rpc2::Server rpc2_server(allocator, dispatcher);

  // Create an adapter for running pw_rpc services without an existing
  // pw::rpc::Server.
  pw::rpc::Rpc2Adapter adapter(adapter_allocator, thread_options);

  // Construct, own, and register SensorServiceImpl with the adapter.
  pw::rpc::Rpc2ServiceFor<SensorServiceImpl> sensor_service(adapter);
  PW_CHECK_OK(rpc2_server.RegisterService(sensor_service));
  // DOCSTAG: [pw_rpc-examples-rpc2-adapter]

  RunServer(rpc2_server, adapter);
}

void RegisterOwnedService(pw::rpc2::Server& rpc2_server,
                          pw::rpc::Rpc2ServerAdapter& adapter) {
  // DOCSTAG: [pw_rpc-examples-rpc2-service-for]
  pw::rpc::Rpc2ServiceFor<SensorServiceImpl> sensor_service(
      adapter, /*temperature_offset=*/0.5f);
  PW_CHECK_OK(rpc2_server.RegisterService(sensor_service));
  // DOCSTAG: [pw_rpc-examples-rpc2-service-for]

  RunServer(rpc2_server, adapter);
}

void RegisterExistingService(pw::rpc::Server& rpc_server,
                             pw::rpc2::Server& rpc2_server,
                             pw::rpc::Rpc2ServerAdapter& adapter) {
  // DOCSTAG: [pw_rpc-examples-rpc2-service-adapter]
  SensorServiceImpl sensor_service;
  rpc_server.RegisterService(sensor_service);

  pw::rpc::Rpc2ServiceAdapter adapted_sensor(adapter, sensor_service);
  PW_CHECK_OK(rpc2_server.RegisterService(adapted_sensor));
  // DOCSTAG: [pw_rpc-examples-rpc2-service-adapter]

  RunServer(rpc2_server, adapter);
}

void RegisterOwnedWithNonOkResponses(pw::rpc2::Server& rpc2_server,
                                     pw::rpc::Rpc2ServerAdapter& adapter) {
  // DOCSTAG: [pw_rpc-examples-rpc2-non-ok-service-for]
  // Deliver non-OK responses for specific methods. Arguments after the policy
  // tag are forwarded to SensorServiceImpl's constructor.
  pw::rpc::Rpc2ServiceFor<SensorServiceImpl> sensor_service(
      adapter,
      pw::rpc::DeliverNonOkResponses<
          SensorServiceImpl::ServiceInfo::GetReading>(),
      /*temperature_offset=*/0.5f);
  PW_CHECK_OK(rpc2_server.RegisterService(sensor_service));
  // DOCSTAG: [pw_rpc-examples-rpc2-non-ok-service-for]

  RunServer(rpc2_server, adapter);
}

void RegisterExistingWithNonOkResponses(pw::rpc::Server& rpc_server,
                                        pw::rpc2::Server& rpc2_server,
                                        pw::rpc::Rpc2ServerAdapter& adapter) {
  SensorServiceImpl sensor_service;
  rpc_server.RegisterService(sensor_service);

  // DOCSTAG: [pw_rpc-examples-rpc2-non-ok-service-adapter]
  // Deliver non-OK responses for all unary and client streaming methods.
  pw::rpc::Rpc2ServiceAdapter adapted_sensor(
      adapter, pw::rpc::DeliverNonOkResponses(), sensor_service);
  PW_CHECK_OK(rpc2_server.RegisterService(adapted_sensor));
  // DOCSTAG: [pw_rpc-examples-rpc2-non-ok-service-adapter]

  RunServer(rpc2_server, adapter);
}

}  // namespace pw::rpc::examples
