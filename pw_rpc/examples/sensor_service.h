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
#pragma once

#include "pw_rpc/examples/sensor_service.rpc.pb.h"
#include "pw_rpc/nanopb/server_reader_writer.h"
#include "pw_rpc/server.h"
#include "pw_status/status.h"

namespace pw::rpc::examples {

// DOCSTAG: [pw_rpc-examples-sensor-service-decl]
class SensorServiceImpl final
    : public pw::rpc::examples::pw_rpc::nanopb::SensorService::Service<
          SensorServiceImpl> {
 public:
  // 1a. Synchronous Unary RPC
  pw::Status GetReading(const pw_rpc_examples_SensorRequest& request,
                        pw_rpc_examples_SensorResponse& response);

  // 1b. Asynchronous Unary RPC (Alternative)
  void GetReadingAsync(
      const pw_rpc_examples_SensorRequest& request,
      pw::rpc::NanopbUnaryResponder<pw_rpc_examples_SensorResponse>&
          new_responder);

  // 2. Server Streaming RPC
  void StreamReadings(
      const pw_rpc_examples_StreamRequest& request,
      pw::rpc::NanopbServerWriter<pw_rpc_examples_SensorResponse>& writer);

  // 3. Bidirectional Streaming RPC
  void Calibrate(
      pw::rpc::NanopbServerReaderWriter<pw_rpc_examples_CalibrationPoint,
                                        pw_rpc_examples_CalibrationStatus>&
          stream);

 private:
  pw::rpc::NanopbUnaryResponder<pw_rpc_examples_SensorResponse>
      async_responder_;
  pw::rpc::NanopbServerReaderWriter<pw_rpc_examples_CalibrationPoint,
                                    pw_rpc_examples_CalibrationStatus>
      calib_stream_;
};
// DOCSTAG: [pw_rpc-examples-sensor-service-decl]

}  // namespace pw::rpc::examples
