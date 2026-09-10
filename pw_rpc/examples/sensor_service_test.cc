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

#include "pw_rpc/examples/sensor_service.h"

#include "pw_rpc/nanopb/test_method_context.h"
#include "pw_unit_test/framework.h"

namespace pw::rpc::examples {
namespace {

// DOCSTAG: [pw_rpc-examples-sensor-unary-test]
TEST(SensorServiceTest, GetReading_ReturnsValidData) {
  PW_NANOPB_TEST_METHOD_CONTEXT(SensorServiceImpl, GetReading) context;

  pw_rpc_examples_SensorRequest request{.sensor_id = 1};
  EXPECT_EQ(pw::OkStatus(), context.call(request));

  EXPECT_TRUE(context.done());
  EXPECT_FLOAT_EQ(24.2f, context.response().temperature);
}
// DOCSTAG: [pw_rpc-examples-sensor-unary-test]

// DOCSTAG: [pw_rpc-examples-sensor-stream-test]
TEST(SensorServiceTest, StreamReadings_StreamsMultipleResponses) {
  PW_NANOPB_TEST_METHOD_CONTEXT(SensorServiceImpl, StreamReadings) context;

  pw_rpc_examples_StreamRequest request{
      .sensor_id = 1,
      .sample_count = 3,
      .interval_ms = 10,
  };
  context.call(request);

  EXPECT_TRUE(context.done());
  ASSERT_EQ(3u, context.responses().size());
  EXPECT_FLOAT_EQ(20.0f, context.responses()[0].temperature);
}
// DOCSTAG: [pw_rpc-examples-sensor-stream-test]

}  // namespace
}  // namespace pw::rpc::examples
