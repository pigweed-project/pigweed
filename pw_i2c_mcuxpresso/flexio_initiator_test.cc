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
#include "pw_i2c_mcuxpresso/flexio_initiator.h"

#include <cstdint>

#include "fsl_clock.h"
#include "pw_unit_test/framework.h"

namespace pw::i2c {
namespace {

constexpr uint32_t kFlexIoBaseAddress = FLEXIO0_BASE;  // Example address
constexpr uint8_t kSdaChannel = 0;
constexpr uint8_t kSclChannel = 1;
constexpr uint32_t kI2CBaudRate = 100000;

constexpr FlexIoMcuxpressoInitiator::Config kConfig = {
    .flexio_address = kFlexIoBaseAddress,
    .clock_name = kCLOCK_FlexioClk,
    .baud_rate_bps = kI2CBaudRate,
    .flexio_sda_channel = kSdaChannel,
    .flexio_scl_channel = kSclChannel,
};

TEST(FlexIoInitiatorTest, Init) {
  // Simple test only meant to ensure module is compiled and can be enabled.
  FlexIoMcuxpressoInitiator initiator{kConfig};
  initiator.Enable();
  initiator.Disable();
}

}  // namespace
}  // namespace pw::i2c
