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

#include <cstdint>

#include "enum_example/i2c_status.h"
#include "pw_status/try.h"
#include "pw_unit_test/framework.h"

namespace {

using my_driver::I2cStatus;

// DOCSTAG: [pw_enum-examples-status-enum-usage]
I2cStatus WriteByte(uint8_t address, uint8_t byte) {
  if (address == 0) {
    return I2cStatus::Nack();  // Factory for I2cStatusCode::kNack
  }
  if (byte == 0xFF) {
    return I2cStatus(I2cStatus::Code::kBusError);  // Explicit construction
  }
  return I2cStatus();  // Default constructed as OK
}

I2cStatus WritePacket(uint8_t address, uint8_t header, uint8_t payload) {
  PW_TRY(WriteByte(address, header));  // Returns early on error
  PW_TRY(WriteByte(address, payload));
  return I2cStatus();
}

bool ShouldRetry(I2cStatus status) {
  return status.IsNack() || status.IsTimeout();  // Query the code
}
// DOCSTAG: [pw_enum-examples-status-enum-usage]

TEST(StatusEnumExample, Ok) {
  I2cStatus status = WritePacket(0x42, 0x01, 0x02);
  PW_TEST_EXPECT_OK(status);
  EXPECT_EQ(status, I2cStatus::Code::kOk);
  EXPECT_STREQ(status.str(), "OK");
  EXPECT_FALSE(ShouldRetry(status));
}

TEST(StatusEnumExample, Error) {
  I2cStatus status = WritePacket(0, 0x01, 0x02);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status, I2cStatus::Nack());
  EXPECT_EQ(status.code(), I2cStatus::Code::kNack);
  EXPECT_STREQ(status.str(), "NACK");
  EXPECT_TRUE(status.IsNack());
  EXPECT_FALSE(status.IsBusError());
  EXPECT_TRUE(ShouldRetry(status));

  I2cStatus bus_error = WritePacket(0x42, 0x01, 0xFF);
  EXPECT_TRUE(bus_error.IsBusError());
  EXPECT_FALSE(ShouldRetry(bus_error));
}

}  // namespace
